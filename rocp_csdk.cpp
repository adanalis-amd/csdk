/**
 * @file    rocp_csdk.cpp
 * @author  Anthony Danalis
 *          adanalis@amd.com
 *
 */

#include "rocp_csdk.h"
#include "rocp_csdk_internal.hpp"

// Define shared/unique lock macros based on C++ version
#if (__cplusplus >= 201703L) // c++17
#define SHARED_LOCK std::shared_lock
#define UNIQUE_LOCK std::unique_lock
#elif (__cplusplus >= 201402L) // c++14
#define SHARED_LOCK std::shared_lock<std::shared_timed_mutex>
#define UNIQUE_LOCK std::unique_lock<std::shared_timed_mutex>
#elif (__cplusplus >= 201103L) // c++11
#define SHARED_LOCK std::lock_guard<std::mutex>
#define UNIQUE_LOCK std::lock_guard<std::mutex>
#else
#error "c++11 or higher is required"
#endif

//--------------------------------------------------------------------------------
// RocpCSDK Implementation
//--------------------------------------------------------------------------------

RocpCSDK& RocpCSDK::instance() {
    static RocpCSDK instance;
    return instance;
}

RocpCSDK::RocpCSDK() = default;

RocpCSDK::~RocpCSDK() {
    delete[] counter_values_;
    counter_values_ = nullptr;
}

//--------------------------------------------------------------------------------
// Getters
//--------------------------------------------------------------------------------

rocprofiler_context_id_t& RocpCSDK::getClientCtx() {
    return client_ctx_;
}

rocprofiler_buffer_id_t& RocpCSDK::getBuffer() {
    return buffer_;
}

int RocpCSDK::getProfilingMode() const {
    return profiling_mode_;
}

long long int* RocpCSDK::getCounterValues() {
    return counter_values_;
}

int RocpCSDK::getEventCount() const {
    return event_count_;
}

//--------------------------------------------------------------------------------
// Helper Methods
//--------------------------------------------------------------------------------

/**
 * For a given counter, query the dimensions that it has.
 */
std::vector<rocprofiler_record_dimension_info_t>
RocpCSDK::counterDimensions(rocprofiler_counter_id_t counter) {
    std::vector<rocprofiler_record_dimension_info_t> dims;
    rocprofiler_available_dimensions_cb_t cb;

    cb = [](rocprofiler_counter_id_t,
            const rocprofiler_record_dimension_info_t* dim_info,
            size_t num_dims,
            void* user_data) {
        auto* vec = static_cast<std::vector<rocprofiler_record_dimension_info_t>*>(user_data);
        for (size_t i = 0; i < num_dims; i++) {
            vec->push_back(dim_info[i]);
        }
        return ROCPROFILER_STATUS_SUCCESS;
    };

    ROCPROFILER_CALL(rocprofiler_iterate_counter_dimensions(counter, cb, &dims),
                     "Could not iterate counter dimensions");
    return dims;
}

bool RocpCSDK::dimensionsMatch(dim_vector_t dim_instances, dim_vector_t recorded_dims) {
    // Traverse all the dimensions in the event instance (i.e. base_event+qualifiers)
    for (const auto& ev_inst_dim : dim_instances) {
        bool found_dim_id = false;
        // Traverse all the dimensions of the event in the record_callback() data
        for (const auto& recorded_dim : recorded_dims) {
            if (ev_inst_dim.first == recorded_dim.first) {
                found_dim_id = true;
                // If the ids of two dimensions match, we compare the positions.
                if (ev_inst_dim.second != recorded_dim.second) {
                    return false;
                }
                break;
            }
        }
        // If the record_callback() data does not have one of the dimensions, they didn't match.
        if (!found_dim_id) {
            return false;
        }
    }
    return true;
}

//--------------------------------------------------------------------------------
// Static Callbacks
//--------------------------------------------------------------------------------

void RocpCSDK::recordCallback(rocprofiler_dispatch_counting_service_data_t dispatch_data,
                              rocprofiler_record_counter_t* record_data,
                              size_t record_count,
                              rocprofiler_user_data_t,
                              void* callback_data_args) {
    auto* self = static_cast<RocpCSDK*>(callback_data_args);
    uint64_t device;

    if ((nullptr == self->counter_values_) ||
        (nullptr == self->active_event_ids_) ||
        (0 == (self->active_state_ & ROCP_CSDK_AES_RUNNING))) {
        return;
    }

    // Find the logical GPU id of this dispatch.
    auto agent = self->gpu_agents_.find(dispatch_data.dispatch_info.agent_id.handle);
    if (self->gpu_agents_.end() != agent) {
        device = agent->second->logical_node_type_id;
    } else {
        device = -1;
    }

    // Create the mapping from events in the eventset to entries in the "record_data" array.
    if (self->index_mapping_.empty()) {
        rec_info_t event_set_to_rec_mapping[record_count];

        self->index_mapping_.resize(record_count * (self->active_num_events_), false);

        // Traverse all the recorded entries and cache some information about them
        for (size_t i = 0; i < record_count; ++i) {
            rocprofiler_counter_id_t counter_id;
            rec_info_t& rec_info = event_set_to_rec_mapping[i];

            rec_info.device = device;

            ROCPROFILER_CALL(rocprofiler_query_record_counter_id(record_data[i].id, &counter_id),
                           "Could not retrieve counter_id");
            rec_info.counter_id = counter_id;

            std::vector<rocprofiler_record_dimension_info_t> dimensions =
                self->counterDimensions(counter_id);
            for (auto& dim : dimensions) {
                unsigned long pos = 0;
                ROCPROFILER_CALL(
                    rocprofiler_query_record_dimension_position(record_data[i].id, dim.id, &pos),
                    "Could not retrieve dimension");
                rec_info.recorded_dims.emplace_back(std::make_pair(dim.id, pos));
            }
        }

        // Traverse all events in the active event set and find which recorded entry matches
        for (int ei = 0; ei < self->active_num_events_; ei++) {
            auto e_tmp = self->id_to_event_instance_.find(self->active_event_ids_[ei]);
            if (self->id_to_event_instance_.end() == e_tmp) {
                continue;
            }
            event_instance_info_t e_inst = e_tmp->second;
            uint32_t e_id_32 = static_cast<uint32_t>(e_inst.counter_info.id.handle);

            for (size_t i = 0; i < record_count; ++i) {
                rec_info_t& rec_info = event_set_to_rec_mapping[i];
                uint32_t r_id_32 = static_cast<uint32_t>(rec_info.counter_id.handle);
                if ((e_inst.device != static_cast<int>(rec_info.device)) ||
                    (e_id_32 != r_id_32) ||
                    !self->dimensionsMatch(e_inst.dim_instances, rec_info.recorded_dims)) {
                    continue;
                }
                self->index_mapping_[ei * record_count + i] = true;
            }
        }
    }

    // Traverse all events in the active event set and accumulate counter values
    for (int ei = 0; ei < self->active_num_events_; ei++) {
        double counter_value_sum = 0.0;

        for (size_t i = 0; i < record_count; ++i) {
            if (self->index_mapping_[ei * record_count + i]) {
                counter_value_sum += record_data[i].counter_value;
            }
        }
        // Accumulate counter values (+=) instead of overwriting
        self->counter_values_[ei] += counter_value_sum;
    }
}

void RocpCSDK::dispatchCallback(rocprofiler_dispatch_counting_service_data_t dispatch_data,
                                rocprofiler_profile_config_id_t* config,
                                rocprofiler_user_data_t*,
                                void* callback_data_args) {
    auto* self = static_cast<RocpCSDK*>(callback_data_args);

    const SHARED_LOCK rlock(self->profile_cache_mutex_);

    auto pos = self->profile_cache_.find(dispatch_data.dispatch_info.agent_id.handle);
    if (self->profile_cache_.end() != pos) {
        *config = pos->second;
    }
}

void RocpCSDK::setProfile(rocprofiler_context_id_t context_id,
                          rocprofiler_agent_id_t agent,
                          rocprofiler_agent_set_profile_callback_t set_config,
                          void* user_data) {
    auto* self = static_cast<RocpCSDK*>(user_data);

    const SHARED_LOCK rlock(self->profile_cache_mutex_);

    auto pos = self->profile_cache_.find(agent.handle);
    if (self->profile_cache_.end() != pos) {
        set_config(context_id, pos->second);
    }
}

void RocpCSDK::bufferedCallback(rocprofiler_context_id_t,
                                rocprofiler_buffer_id_t,
                                rocprofiler_record_header_t**,
                                size_t,
                                void*,
                                uint64_t) {
    // Currently unused
}

//--------------------------------------------------------------------------------
// PC Sampling Implementation
//--------------------------------------------------------------------------------

void RocpCSDK::pcSamplingCallback(rocprofiler_context_id_t,
                                   rocprofiler_buffer_id_t,
                                   rocprofiler_record_header_t** headers,
                                   size_t num_headers,
                                   void* user_data,
                                   uint64_t drop_count) {
    auto* self = static_cast<RocpCSDK*>(user_data);
    self->pcs_total_drops_ += drop_count;

    std::lock_guard<std::mutex> lock(self->pcs_samples_mutex_);

    for (size_t i = 0; i < num_headers; i++) {
        auto* hdr = headers[i];
        if (hdr->category != ROCPROFILER_BUFFER_CATEGORY_PC_SAMPLING)
            continue;

        rocp_csdk_pcs_sample_raw_t raw;
        if (hdr->kind == ROCPROFILER_PC_SAMPLING_RECORD_HOST_TRAP_V0_SAMPLE) {
            raw.record_kind = hdr->kind;
            raw.data.host_trap =
                *static_cast<rocprofiler_pc_sampling_record_host_trap_v0_t*>(hdr->payload);
        } else if (hdr->kind == ROCPROFILER_PC_SAMPLING_RECORD_STOCHASTIC_V0_SAMPLE) {
            raw.record_kind = hdr->kind;
            raw.data.stochastic =
                *static_cast<rocprofiler_pc_sampling_record_stochastic_v0_t*>(hdr->payload);
        } else {
            continue;
        }

        // Enforce buffer limit - drop oldest samples
        while (self->pcs_samples_.size() >= self->pcs_max_samples_) {
            self->pcs_samples_.pop_front();
            self->pcs_total_drops_++;
        }
        self->pcs_samples_.push_back(raw);
    }
}

int RocpCSDK::pcsQueryCapabilities(int device_id, rocp_csdk_pcs_caps_t* caps) {
    if (!caps) return RETVAL_FAIL;

    memset(caps, 0, sizeof(*caps));

    // Cache PC sampling configurations if not done yet
    if (!pcs_configs_cached_) {
        pcs_agent_configs_.clear();

        for (const auto& agent_pair : gpu_agents_) {
            std::vector<pcs_config_info_t> configs;

            auto cb = [](const rocprofiler_pc_sampling_configuration_t* cfgs,
                         size_t num_cfgs, void* user_data) {
                auto* vec = static_cast<std::vector<pcs_config_info_t>*>(user_data);
                for (size_t i = 0; i < num_cfgs; i++) {
                    vec->emplace_back(pcs_config_info_t{
                        cfgs[i].method, cfgs[i].unit,
                        cfgs[i].min_interval, cfgs[i].max_interval,
                        cfgs[i].flags
                    });
                }
                return ROCPROFILER_STATUS_SUCCESS;
            };

            rocprofiler_query_pc_sampling_agent_configurations(
                agent_pair.second->id, cb, &configs);

            pcs_agent_configs_[agent_pair.second->logical_node_type_id] = std::move(configs);
        }
        pcs_configs_cached_ = true;
    }

    // Find capabilities for the requested device
    auto it = pcs_agent_configs_.find(static_cast<uint64_t>(device_id));
    if (it == pcs_agent_configs_.end() && device_id >= 0) {
        return RETVAL_FAIL;  // Device not found
    }

    // If device_id == -1, return capabilities of first device
    if (pcs_agent_configs_.empty()) {
        caps->supported = 0;
        return RETVAL_SUCCESS;
    }

    const auto& configs = (device_id < 0)
        ? pcs_agent_configs_.begin()->second
        : it->second;

    if (configs.empty()) {
        caps->supported = 0;
        return RETVAL_SUCCESS;
    }

    caps->supported = 1;
    for (const auto& cfg : configs) {
        if (cfg.method == ROCPROFILER_PC_SAMPLING_METHOD_STOCHASTIC) {
            caps->stochastic_available = 1;
            caps->stochastic_min_interval = cfg.min_interval;
            caps->stochastic_max_interval = cfg.max_interval;
        } else if (cfg.method == ROCPROFILER_PC_SAMPLING_METHOD_HOST_TRAP) {
            caps->host_trap_available = 1;
            caps->host_trap_min_interval = cfg.min_interval;
            caps->host_trap_max_interval = cfg.max_interval;
        }
    }

    return RETVAL_SUCCESS;
}

int RocpCSDK::pcsStart(const rocp_csdk_pcs_config_t* config) {
    // Check mutual exclusion with counter profiling
    if (active_state_ == ROCP_CSDK_AES_RUNNING) {
        return RETVAL_FAIL;  // Counter profiling is active
    }
    if (pcs_active_) {
        return RETVAL_FAIL;  // PC sampling already active
    }

    // Query configurations if not cached
    if (!pcs_configs_cached_) {
        rocp_csdk_pcs_caps_t dummy;
        pcsQueryCapabilities(-1, &dummy);
    }

    // Create PC sampling context
    ROCPROFILER_CALL(rocprofiler_create_context(&pcs_ctx_), "create PC sampling context");

    pcs_max_samples_ = (config && config->max_samples > 0) ? config->max_samples : 10000;
    pcs_samples_.clear();
    pcs_total_drops_ = 0;
    pcs_agent_states_.clear();
    pcs_buffer_ids_.clear();

    // Configure PC sampling for each agent
    for (const auto& agent_pair : gpu_agents_) {
        const auto* agent = agent_pair.second;
        uint64_t logical_id = agent->logical_node_type_id;

        // Skip if user specified a specific device and this isn't it
        if (config && config->device_id >= 0 &&
            static_cast<uint64_t>(config->device_id) != logical_id) {
            continue;
        }

        auto cfg_it = pcs_agent_configs_.find(logical_id);
        if (cfg_it == pcs_agent_configs_.end() || cfg_it->second.empty()) {
            continue;  // No PC sampling support for this agent
        }

        // Select method (prefer stochastic if AUTO)
        const pcs_config_info_t* picked_cfg = nullptr;
        rocprofiler_pc_sampling_method_t desired_method = ROCPROFILER_PC_SAMPLING_METHOD_NONE;

        if (config) {
            if (config->method == ROCP_CSDK_PCS_METHOD_HOST_TRAP)
                desired_method = ROCPROFILER_PC_SAMPLING_METHOD_HOST_TRAP;
            else if (config->method == ROCP_CSDK_PCS_METHOD_STOCHASTIC)
                desired_method = ROCPROFILER_PC_SAMPLING_METHOD_STOCHASTIC;
        }

        for (const auto& cfg : cfg_it->second) {
            if (desired_method != ROCPROFILER_PC_SAMPLING_METHOD_NONE) {
                if (cfg.method == desired_method) {
                    picked_cfg = &cfg;
                    break;
                }
            } else {
                // AUTO: prefer stochastic
                if (cfg.method == ROCPROFILER_PC_SAMPLING_METHOD_STOCHASTIC) {
                    picked_cfg = &cfg;
                    break;
                } else if (!picked_cfg) {
                    picked_cfg = &cfg;
                }
            }
        }

        if (!picked_cfg) continue;

        // Create buffer for this agent
        rocprofiler_buffer_id_t buffer_id;
        ROCPROFILER_CALL(rocprofiler_create_buffer(
            pcs_ctx_,
            64 * 1024,  // 64KB buffer
            32 * 1024,  // Watermark
            ROCPROFILER_BUFFER_POLICY_LOSSLESS,
            &RocpCSDK::pcSamplingCallback,
            this,
            &buffer_id),
            "create PC sampling buffer");

        pcs_buffer_ids_.push_back(buffer_id);

        // Determine interval
        uint64_t interval = picked_cfg->min_interval;
        if (config && config->interval > 0) {
            interval = std::max(picked_cfg->min_interval,
                               std::min(config->interval, picked_cfg->max_interval));
        }

        // Configure PC sampling service
        ROCPROFILER_CALL(rocprofiler_configure_pc_sampling_service(
            pcs_ctx_,
            agent->id,
            picked_cfg->method,
            picked_cfg->unit,
            interval,
            buffer_id,
            0),  // flags
            "configure PC sampling service");

        // Create and assign callback thread
        rocprofiler_callback_thread_t cb_thread;
        ROCPROFILER_CALL(rocprofiler_create_callback_thread(&cb_thread),
                         "create callback thread");
        ROCPROFILER_CALL(rocprofiler_assign_callback_thread(buffer_id, cb_thread),
                         "assign callback thread");

        pcs_agent_states_.push_back({
            agent->id, logical_id, buffer_id, picked_cfg->method, true
        });
    }

    if (pcs_agent_states_.empty()) {
        return RETVAL_FAIL;  // No agents configured
    }

    // Start context
    ROCPROFILER_CALL(rocprofiler_start_context(pcs_ctx_), "start PC sampling context");
    pcs_active_ = true;

    return RETVAL_SUCCESS;
}

int RocpCSDK::pcsStop() {
    if (!pcs_active_) {
        return RETVAL_SUCCESS;  // Already stopped
    }

    // Stop context
    ROCPROFILER_CALL(rocprofiler_stop_context(pcs_ctx_), "stop PC sampling context");

    // Flush and destroy buffers
    for (auto& buf_id : pcs_buffer_ids_) {
        ROCPROFILER_CALL(rocprofiler_flush_buffer(buf_id), "flush PC sampling buffer");
        ROCPROFILER_CALL(rocprofiler_destroy_buffer(buf_id), "destroy PC sampling buffer");
    }

    pcs_buffer_ids_.clear();
    pcs_agent_states_.clear();
    pcs_active_ = false;

    return RETVAL_SUCCESS;
}

int RocpCSDK::pcsRead(rocp_csdk_pcs_sample_t* samples, size_t* num_samples, size_t max) {
    if (!samples || !num_samples) return RETVAL_FAIL;

    std::lock_guard<std::mutex> lock(pcs_samples_mutex_);
    size_t count = std::min(max, pcs_samples_.size());

    for (size_t i = 0; i < count; i++) {
        auto& raw = pcs_samples_.front();
        auto& out = samples[i];
        memset(&out, 0, sizeof(out));

        if (raw.record_kind == ROCPROFILER_PC_SAMPLING_RECORD_HOST_TRAP_V0_SAMPLE) {
            auto& ht = raw.data.host_trap;
            out.code_object_id = ht.pc.code_object_id;
            out.code_object_offset = ht.pc.code_object_offset;
            out.timestamp = ht.timestamp;
            out.exec_mask = ht.exec_mask;
            out.dispatch_id = ht.dispatch_id;
            out.workgroup_x = ht.workgroup_id.x;
            out.workgroup_y = ht.workgroup_id.y;
            out.workgroup_z = ht.workgroup_id.z;
            out.wave_in_group = ht.wave_in_group;
            out.chiplet = ht.hw_id.chiplet;
            out.cu_id = ht.hw_id.cu_or_wgp_id;
            out.simd_id = ht.hw_id.simd_id;
            out.shader_engine_id = ht.hw_id.shader_engine_id;
            out.wave_issued = 1;  // Host trap doesn't distinguish
            out.stall_reason = 0;
        } else if (raw.record_kind == ROCPROFILER_PC_SAMPLING_RECORD_STOCHASTIC_V0_SAMPLE) {
            auto& st = raw.data.stochastic;
            out.code_object_id = st.pc.code_object_id;
            out.code_object_offset = st.pc.code_object_offset;
            out.timestamp = st.timestamp;
            out.exec_mask = st.exec_mask;
            out.dispatch_id = st.dispatch_id;
            out.workgroup_x = st.workgroup_id.x;
            out.workgroup_y = st.workgroup_id.y;
            out.workgroup_z = st.workgroup_id.z;
            out.wave_in_group = st.wave_in_group;
            out.chiplet = st.hw_id.chiplet;
            out.cu_id = st.hw_id.cu_or_wgp_id;
            out.simd_id = st.hw_id.simd_id;
            out.shader_engine_id = st.hw_id.shader_engine_id;
            out.wave_issued = st.wave_issued;
            out.inst_type = st.inst_type;
            out.stall_reason = st.snapshot.reason_not_issued;
        }

        pcs_samples_.pop_front();
    }

    *num_samples = count;
    return RETVAL_SUCCESS;
}

int RocpCSDK::pcsReadRaw(rocp_csdk_pcs_sample_raw_t* samples, size_t* num_samples, size_t max) {
    if (!samples || !num_samples) return RETVAL_FAIL;

    std::lock_guard<std::mutex> lock(pcs_samples_mutex_);
    size_t count = std::min(max, pcs_samples_.size());

    for (size_t i = 0; i < count; i++) {
        samples[i] = pcs_samples_.front();
        pcs_samples_.pop_front();
    }

    *num_samples = count;
    return RETVAL_SUCCESS;
}

uint64_t RocpCSDK::pcsGetDropCount() const {
    return pcs_total_drops_.load();
}

int RocpCSDK::initPcSampling() {
    // Query and cache PC sampling configurations
    pcs_agent_configs_.clear();
    for (const auto& agent_pair : gpu_agents_) {
        std::vector<pcs_config_info_t> configs;

        auto cb = [](const rocprofiler_pc_sampling_configuration_t* cfgs,
                     size_t num_cfgs, void* user_data) {
            auto* vec = static_cast<std::vector<pcs_config_info_t>*>(user_data);
            for (size_t i = 0; i < num_cfgs; i++) {
                vec->emplace_back(pcs_config_info_t{
                    cfgs[i].method, cfgs[i].unit,
                    cfgs[i].min_interval, cfgs[i].max_interval,
                    cfgs[i].flags
                });
            }
            return ROCPROFILER_STATUS_SUCCESS;
        };

        rocprofiler_query_pc_sampling_agent_configurations(
            agent_pair.second->id, cb, &configs);

        if (!configs.empty()) {
            pcs_agent_configs_[agent_pair.second->logical_node_type_id] = std::move(configs);
        }
    }
    pcs_configs_cached_ = true;

    if (pcs_agent_configs_.empty()) {
        // No agents support PC sampling
        return -1;
    }

    // Create PC sampling context
    ROCPROFILER_CALL(rocprofiler_create_context(&pcs_ctx_), "create PC sampling context");

    // Read configuration from environment
    pcs_max_samples_ = 50000;  // Default
    if (const char* max_env = getenv("ROCP_CSDK_PCS_MAX_SAMPLES")) {
        pcs_max_samples_ = std::strtoul(max_env, nullptr, 10);
    }

    // Determine method preference from environment
    rocp_csdk_pcs_method_t method_pref = ROCP_CSDK_PCS_METHOD_AUTO;
    if (const char* method_env = getenv("ROCP_CSDK_PCS_METHOD")) {
        if (strcmp(method_env, "host_trap") == 0) {
            method_pref = ROCP_CSDK_PCS_METHOD_HOST_TRAP;
        } else if (strcmp(method_env, "stochastic") == 0) {
            method_pref = ROCP_CSDK_PCS_METHOD_STOCHASTIC;
        }
    }

    // Determine interval from environment (0 = use default)
    uint64_t interval_pref = 0;
    if (const char* interval_env = getenv("ROCP_CSDK_PCS_INTERVAL")) {
        interval_pref = std::strtoull(interval_env, nullptr, 10);
    }

    pcs_samples_.clear();
    pcs_total_drops_ = 0;
    pcs_agent_states_.clear();
    pcs_buffer_ids_.clear();

    // Configure PC sampling for each GPU agent
    for (const auto& agent_pair : gpu_agents_) {
        const auto* agent = agent_pair.second;
        uint64_t logical_id = agent->logical_node_type_id;

        auto cfg_it = pcs_agent_configs_.find(logical_id);
        if (cfg_it == pcs_agent_configs_.end() || cfg_it->second.empty()) {
            continue;
        }

        // Select configuration (prefer stochastic if AUTO)
        const pcs_config_info_t* picked_cfg = nullptr;
        rocprofiler_pc_sampling_method_t desired_method = ROCPROFILER_PC_SAMPLING_METHOD_NONE;

        if (method_pref == ROCP_CSDK_PCS_METHOD_HOST_TRAP)
            desired_method = ROCPROFILER_PC_SAMPLING_METHOD_HOST_TRAP;
        else if (method_pref == ROCP_CSDK_PCS_METHOD_STOCHASTIC)
            desired_method = ROCPROFILER_PC_SAMPLING_METHOD_STOCHASTIC;

        for (const auto& cfg : cfg_it->second) {
            if (desired_method != ROCPROFILER_PC_SAMPLING_METHOD_NONE) {
                if (cfg.method == desired_method) {
                    picked_cfg = &cfg;
                    break;
                }
            } else {
                // AUTO: prefer stochastic
                if (cfg.method == ROCPROFILER_PC_SAMPLING_METHOD_STOCHASTIC) {
                    picked_cfg = &cfg;
                    break;
                } else if (!picked_cfg) {
                    picked_cfg = &cfg;
                }
            }
        }

        if (!picked_cfg) continue;

        // Create buffer for this agent
        rocprofiler_buffer_id_t buffer_id;
        ROCPROFILER_CALL(rocprofiler_create_buffer(
            pcs_ctx_,
            64 * 1024,  // 64KB buffer
            32 * 1024,  // Watermark
            ROCPROFILER_BUFFER_POLICY_LOSSLESS,
            &RocpCSDK::pcSamplingCallback,
            this,
            &buffer_id),
            "create PC sampling buffer");

        pcs_buffer_ids_.push_back(buffer_id);

        // Determine interval
        uint64_t interval;
        if (picked_cfg->method == ROCPROFILER_PC_SAMPLING_METHOD_STOCHASTIC) {
            // Default: 2^20 cycles for stochastic
            interval = (interval_pref > 0) ? interval_pref : 1048576;
        } else {
            // Default: 10000 microseconds (10ms) for host trap
            interval = (interval_pref > 0) ? interval_pref : 10000;
        }
        // Clamp to valid range
        interval = std::max(picked_cfg->min_interval,
                           std::min(interval, picked_cfg->max_interval));

        // Configure PC sampling service
        rocprofiler_status_t status = rocprofiler_configure_pc_sampling_service(
            pcs_ctx_,
            agent->id,
            picked_cfg->method,
            picked_cfg->unit,
            interval,
            buffer_id,
            0);

        if (status != ROCPROFILER_STATUS_SUCCESS) {
            continue;  // Skip this agent
        }

        // Create and assign callback thread
        rocprofiler_callback_thread_t cb_thread;
        ROCPROFILER_CALL(rocprofiler_create_callback_thread(&cb_thread),
                         "create callback thread");
        ROCPROFILER_CALL(rocprofiler_assign_callback_thread(buffer_id, cb_thread),
                         "assign callback thread");

        pcs_agent_states_.push_back({
            agent->id, logical_id, buffer_id, picked_cfg->method, true
        });
    }

    if (pcs_agent_states_.empty()) {
        return -1;  // No agents configured
    }

    // Start context immediately
    ROCPROFILER_CALL(rocprofiler_start_context(pcs_ctx_), "start PC sampling context");
    pcs_active_ = true;

    return 0;
}

//--------------------------------------------------------------------------------
// Agent Management
//--------------------------------------------------------------------------------

agent_map_t RocpCSDK::getGPUAgentInfo() {
    auto iterate_cb = [](rocprofiler_agent_version_t agents_ver,
                         const void** agents_arr,
                         size_t num_agents,
                         void* user_data) {
        if (agents_ver != ROCPROFILER_AGENT_INFO_VERSION_0)
            throw std::runtime_error{"unexpected rocprofiler agent version"};

        auto* agents_v = static_cast<agent_map_t*>(user_data);
        for (size_t i = 0; i < num_agents; ++i) {
            const auto* itr = static_cast<const rocprofiler_agent_v0_t*>(agents_arr[i]);
            if (ROCPROFILER_AGENT_TYPE_GPU == itr->type) {
                agents_v->emplace(itr->id.handle, itr);
            }
        }
        return ROCPROFILER_STATUS_SUCCESS;
    };

    auto agents = agent_map_t{};
    ROCPROFILER_CALL(rocprofiler_query_available_agents(ROCPROFILER_AGENT_INFO_VERSION_0,
                                                        iterate_cb,
                                                        sizeof(rocprofiler_agent_t),
                                                        static_cast<void*>(&agents)),
                     "query available agents");

    return agents;
}

//--------------------------------------------------------------------------------
// Event Management
//--------------------------------------------------------------------------------

void RocpCSDK::deleteEventList() {
    base_events_by_name_.clear();
}

int RocpCSDK::assignIdToEvent(const std::string& event_name, event_instance_info_t ev_inst_info) {
    // Note: global_event_count_ is std::atomic, so this is thread safe.
    int event_id = global_event_count_++;
    id_to_event_instance_[event_id] = ev_inst_info;
    event_instance_name_to_id_[event_name] = event_id;

    return event_id;
}

int RocpCSDK::evtNameToCode(const char* event_name, unsigned int* event_code) {
    // If "device" qualifier is not provided by the user, make it zero.
    if (nullptr == strstr(event_name, "device=")) {
        std::string amended_event_name = std::string(event_name) + ":device=0";
        if (amended_event_name.length() > 1024) {
            return RETVAL_FAIL;
        }
        return evtNameToId(amended_event_name, event_code);
    } else {
        return evtNameToId(event_name, event_code);
    }
}

void RocpCSDK::populateEventList() {
    // If the event list is already populated, return without doing anything.
    if (!base_events_by_name_.empty())
        return;

    // Pick the first agent, because we currently do not support heterogeneous GPUs
    if (0 == gpu_agents_.size())
        return;

    const rocprofiler_agent_v0_t* agent = gpu_agents_.begin()->second;

    // GPU Counter IDs
    std::vector<rocprofiler_counter_id_t> gpu_counters;

    auto itrt_cntr_cb = [](rocprofiler_agent_id_t,
                           rocprofiler_counter_id_t* counters,
                           size_t num_counters,
                           void* udata) {
        auto* vec = static_cast<std::vector<rocprofiler_counter_id_t>*>(udata);
        for (size_t i = 0; i < num_counters; i++) {
            vec->push_back(counters[i]);
        }
        return ROCPROFILER_STATUS_SUCCESS;
    };

    // Get the counters available through the selected agent.
    ROCPROFILER_CALL(
        rocprofiler_iterate_agent_supported_counters(agent->id, itrt_cntr_cb,
                                                     static_cast<void*>(&gpu_counters)),
        "Could not fetch supported counters");

    for (auto& counter : gpu_counters) {
        rocprofiler_counter_info_v0_t counter_info;
        ROCPROFILER_CALL(
            rocprofiler_query_counter_info(counter, ROCPROFILER_COUNTER_INFO_VERSION_0,
                                           static_cast<void*>(&counter_info)),
            "Could not query info");

        std::vector<rocprofiler_record_dimension_info_t> dim_info;
        dim_info = counterDimensions(counter_info.id);

        base_events_by_name_[counter_info.name].counter_info = counter_info;
        base_events_by_name_[counter_info.name].dim_info = dim_info;

        ++base_event_count_;

        event_instance_info_t ev_inst_info;
        ev_inst_info.qualifiers_present = 0;
        ev_inst_info.event_inst_name = counter_info.name;
        ev_inst_info.counter_info = counter_info;
        ev_inst_info.dim_info = dim_info;
        ev_inst_info.dim_instances = {};
        ev_inst_info.device = -1;
        (void)assignIdToEvent(counter_info.name, ev_inst_info);
    }
}

int RocpCSDK::buildEventInfoFromName(const std::string& event_name,
                                     event_instance_info_t* ev_inst_info) {
    size_t pos = 0, ppos = 0;
    std::vector<std::string> qualifiers = {};
    dim_vector_t dim_instances = {};
    std::string base_event_name;
    uint64_t qualifiers_present = 0;
    int device_qualifier_value = -1;

    pos = event_name.find(':');
    if (pos == std::string::npos) {
        base_event_name = event_name;
    } else {
        base_event_name = event_name.substr(0, pos);
        ppos = pos + 1;
        // Tokenize the event name and keep the qualifiers in a vector.
        while ((pos = event_name.find(':', ppos)) != std::string::npos) {
            std::string qual_tuple = event_name.substr(ppos, pos - ppos);
            qualifiers.emplace_back(qual_tuple);
            ppos = pos + 1;
        }
        // Add the last qualifier we found
        qualifiers.emplace_back(event_name.substr(ppos));
    }

    auto it0 = base_events_by_name_.find(base_event_name);
    if (base_events_by_name_.end() == it0) {
        return RETVAL_FAIL;
    }
    base_event_info_t base_event_info = it0->second;

    for (const auto& qual : qualifiers) {
        // All qualifiers must have the form "qual_name=qual_value".
        pos = qual.find('=');
        if (pos == std::string::npos) {
            return RETVAL_FAIL;
        }

        std::string qual_name = qual.substr(0, pos);
        int qual_val = std::stoi(qual.substr(pos + 1));

        // The "device" qualifier does not appear as a rocprofiler-sdk dimension.
        if (qual_name.compare("device") == 0) {
            // We use the most significant bit to designate the presence of "device" qualifier.
            qualifiers_present |= (1ULL << base_event_info.dim_info.size());
            device_qualifier_value = qual_val;
        } else {
            int qual_i = 0;
            // Make sure that the qualifier name corresponds to one of the known dimensions
            for (const auto& dim : base_event_info.dim_info) {
                if (qual_name.compare(dim.name) == 0) {
                    // Make sure that the qualifier value is within the proper range.
                    if (static_cast<size_t>(qual_val) >= dim.instance_size) {
                        return RETVAL_FAIL;
                    }
                    dim_instances.emplace_back(std::make_pair(dim.id, qual_val));
                    // Mark which qualifiers we have found
                    if (qual_i < 64) {
                        qualifiers_present |= (1ULL << qual_i);
                    }
                }
                ++qual_i;
            }
        }
    }

    // Sort the qualifiers (dimension instances) based on dimension id.
    std::sort(dim_instances.begin(), dim_instances.end(),
              [](const dim_t& a, const dim_t& b) { return (a.first < b.first); });

    ev_inst_info->qualifiers_present = qualifiers_present;
    ev_inst_info->event_inst_name = event_name;
    ev_inst_info->counter_info = base_event_info.counter_info;
    ev_inst_info->dim_info = base_event_info.dim_info;
    ev_inst_info->dim_instances = dim_instances;
    ev_inst_info->device = device_qualifier_value;

    return RETVAL_SUCCESS;
}

int RocpCSDK::evtNameToId(const std::string& event_name, unsigned int* event_id) {
    event_instance_info_t ev_inst_info;
    unsigned int eid;

    // If the event already exists in our metadata, return its id.
    auto it1 = event_instance_name_to_id_.find(event_name);
    if (event_instance_name_to_id_.end() != it1) {
        eid = it1->second;
    } else {
        // If we've never seen this event before, insert the info into our metadata.
        int ret_val = buildEventInfoFromName(event_name, &ev_inst_info);
        if (RETVAL_SUCCESS != ret_val) {
            return ret_val;
        }
        eid = assignIdToEvent(event_name, ev_inst_info);
    }

    *event_id = eid;
    return RETVAL_SUCCESS;
}

//--------------------------------------------------------------------------------
// Event Set Management
//--------------------------------------------------------------------------------

int RocpCSDK::setActiveEventSet(const char** event_list, int num_events) {
    if (nullptr == event_list || num_events <= 0) {
        return RETVAL_FAIL;
    }

    auto* event_ids = new unsigned int[num_events]();  // () zero-initializes

    for (int i = 0; i < num_events; i++) {
        if (nullptr == event_list[i]) {
            delete[] event_ids;
            return RETVAL_FAIL;
        }
        int ret = evtNameToCode(event_list[i], &(event_ids[i]));
        if (ret != RETVAL_SUCCESS) {
            delete[] event_ids;
            return ret;
        }
    }

    active_event_ids_ = event_ids;
    active_num_events_ = num_events;
    active_state_ = ROCP_CSDK_AES_OPEN;
    return RETVAL_SUCCESS;
}

void RocpCSDK::emptyActiveEventSet() {
    if (active_event_ids_) {
        delete[] active_event_ids_;
        active_event_ids_ = nullptr;
    }
    active_num_events_ = 0;
    active_state_ = ROCP_CSDK_AES_STOPPED;

    index_mapping_.clear();
    active_device_set_.clear();
}

int RocpCSDK::setProfileCache() {
    std::map<uint64_t, std::vector<event_instance_info_t>> active_events_per_device;

    // Acquire a unique lock so that no other thread can read the cache while modifying it.
    const UNIQUE_LOCK wlock(profile_cache_mutex_);

    profile_cache_.clear();

    for (int i = 0; i < active_num_events_; ++i) {
        // Make sure the event exists.
        auto it = id_to_event_instance_.find(active_event_ids_[i]);
        if (id_to_event_instance_.end() == it) {
            return RETVAL_FAIL;
        }

        active_device_set_.insert(it->second.device);
        active_events_per_device[it->second.device].emplace_back(it->second);
    }

    for (const auto& a_it : gpu_agents_) {
        rocprofiler_profile_config_id_t profile;

        auto agent = a_it.second;

        std::vector<rocprofiler_counter_id_t> event_vid_list = {};
        std::set<uint64_t> id_set = {};

        for (const auto& e_inst : active_events_per_device[agent->logical_node_type_id]) {
            rocprofiler_counter_id_t vid = e_inst.counter_info.id;
            // If the vid is not already in the list, add it.
            if (id_set.find(vid.handle) == id_set.end()) {
                event_vid_list.emplace_back(vid);
                id_set.emplace(vid.handle);
            }
        }

        ROCPROFILER_CALL(rocprofiler_create_profile_config(agent->id,
                                                           event_vid_list.data(),
                                                           event_vid_list.size(),
                                                           &profile),
                         "Could not construct profile cfg");

        profile_cache_.emplace(agent->id.handle, profile);
    }

    return RETVAL_SUCCESS;
}

//--------------------------------------------------------------------------------
// Lifecycle
//--------------------------------------------------------------------------------

int RocpCSDK::init(rocprofiler_client_finalize_t fini_func, void* tool_data) {
    if (nullptr != getenv("ROCP_CSDK_DISPATCH_MODE")) {
        profiling_mode_ = ROCP_CSDK_MODE_DISPATCH;
    } else if (nullptr != getenv("ROCP_CSDK_PC_SAMPLING_MODE")) {
        profiling_mode_ = ROCP_CSDK_MODE_PC_SAMPLING;
    }

    // Obtain the list of available (GPU) agents.
    gpu_agents_ = getGPUAgentInfo();

    if (ROCP_CSDK_MODE_PC_SAMPLING == getProfilingMode()) {
        // PC Sampling mode - initialize during tool_init
        return initPcSampling();
    }

    ROCPROFILER_CALL(rocprofiler_create_context(&getClientCtx()), "context creation");

    if (ROCP_CSDK_MODE_DEVICE_SAMPLING == getProfilingMode()) {
        ROCPROFILER_CALL(rocprofiler_create_buffer(getClientCtx(),
                                                   32 * 1024,
                                                   16 * 1024,
                                                   ROCPROFILER_BUFFER_POLICY_LOSSLESS,
                                                   &RocpCSDK::bufferedCallback,
                                                   this,
                                                   &getBuffer()),
                         "buffer creation failed");

        // Configure device_counting_service for all devices.
        for (auto g_it = gpu_agents_.begin(); g_it != gpu_agents_.end(); ++g_it) {
            ROCPROFILER_CALL(rocprofiler_configure_device_counting_service(
                                 getClientCtx(), getBuffer(), g_it->second->id,
                                 &RocpCSDK::setProfile, this),
                             "Could not setup sampling");
        }
    } else {
        ROCPROFILER_CALL(rocprofiler_configure_callback_dispatch_counting_service(
                             getClientCtx(), &RocpCSDK::dispatchCallback, this,
                             &RocpCSDK::recordCallback, this),
                         "Could not setup callback dispatch");
    }

    populateEventList();

    return 0;
}

void RocpCSDK::fini(void* tool_data) {
    if (profiling_mode_ == ROCP_CSDK_MODE_PC_SAMPLING) {
        // Stop PC sampling and flush buffers
        if (pcs_active_) {
            ROCPROFILER_CALL(rocprofiler_stop_context(pcs_ctx_), "stop PC sampling context");
            pcs_active_ = false;
        }
        for (auto& buf_id : pcs_buffer_ids_) {
            ROCPROFILER_CALL(rocprofiler_flush_buffer(buf_id), "flush PC sampling buffer");
            ROCPROFILER_CALL(rocprofiler_destroy_buffer(buf_id), "destroy PC sampling buffer");
        }
        pcs_buffer_ids_.clear();
        pcs_agent_states_.clear();
    } else {
        stopCounting();
        emptyActiveEventSet();
    }
}

//--------------------------------------------------------------------------------
// Profiling Control
//--------------------------------------------------------------------------------

void RocpCSDK::stopCounting() {
    int ctx_active, ctx_valid;
    delete[] counter_values_;
    counter_values_ = nullptr;
    event_count_ = 0;

    ROCPROFILER_CALL(rocprofiler_context_is_valid(getClientCtx(), &ctx_valid),
                     "check context validity");
    if (!ctx_valid) {
        return;
    }
    ROCPROFILER_CALL(rocprofiler_context_is_active(getClientCtx(), &ctx_active),
                     "check if context is active");
    if (!ctx_active) {
        return;
    }
    ROCPROFILER_CALL(rocprofiler_stop_context(getClientCtx()), "stop context");
}

void RocpCSDK::startCounting(int event_count) {
    counter_values_ = new long long[event_count]();  // () zero-initializes
    event_count_ = event_count;
    active_state_ = ROCP_CSDK_AES_RUNNING;

    ROCPROFILER_CALL(rocprofiler_start_context(getClientCtx()), "start context");
}

int RocpCSDK::readSample() {
    int ret_val = RETVAL_SUCCESS;
    rocprofiler_status_t tmp;
    size_t rec_count = 1024;
    rocprofiler_record_counter_t output_records[1024];

    if ((0 == event_count_) || (nullptr == counter_values_) ||
        (nullptr == active_event_ids_) ||
        (0 == (active_state_ & ROCP_CSDK_AES_RUNNING))) {
        return RETVAL_FAIL;
    }

    tmp = rocprofiler_sample_device_counting_service(
        getClientCtx(), {}, ROCPROFILER_COUNTER_FLAG_NONE,
        output_records, &rec_count);

    if (tmp != ROCPROFILER_STATUS_SUCCESS) {
        return RETVAL_FAIL;
    }

    // Create the mapping from events in the eventset to entries in the sample array.
    if (index_mapping_.empty()) {
        rec_info_t event_set_to_rec_mapping[rec_count];

        index_mapping_.resize(rec_count * (active_num_events_), false);

        // Traverse all the sampled entries and cache some information about them
        for (size_t i = 0; i < rec_count; ++i) {
            rocprofiler_counter_id_t counter_id;
            rec_info_t& rec_info = event_set_to_rec_mapping[i];

            auto agent = gpu_agents_.find(output_records[i].agent_id.handle);
            if (gpu_agents_.end() != agent) {
                rec_info.device = agent->second->logical_node_type_id;
            }

            ROCPROFILER_CALL(rocprofiler_query_record_counter_id(output_records[i].id, &counter_id),
                           "Could not retrieve counter_id");
            rec_info.counter_id = counter_id;

            std::vector<rocprofiler_record_dimension_info_t> dimensions =
                counterDimensions(counter_id);
            for (auto& dim : dimensions) {
                unsigned long pos = 0;
                ROCPROFILER_CALL(
                    rocprofiler_query_record_dimension_position(output_records[i].id, dim.id, &pos),
                    "Could not retrieve dimension");
                rec_info.recorded_dims.emplace_back(std::make_pair(dim.id, pos));
            }
        }

        // Traverse all events in the active event set and find which entries match
        for (int ei = 0; ei < active_num_events_; ei++) {
            auto tmp_it = id_to_event_instance_.find(active_event_ids_[ei]);
            if (id_to_event_instance_.end() == tmp_it) {
                continue;
            }
            event_instance_info_t e_inst = tmp_it->second;
            uint32_t e_id_32 = static_cast<uint32_t>(e_inst.counter_info.id.handle);

            for (size_t i = 0; i < rec_count; ++i) {
                rec_info_t& rec_info = event_set_to_rec_mapping[i];
                uint32_t r_id_32 = static_cast<uint32_t>(rec_info.counter_id.handle);
                if ((e_inst.device != static_cast<int>(rec_info.device)) ||
                    (e_id_32 != r_id_32) ||
                    !dimensionsMatch(e_inst.dim_instances, rec_info.recorded_dims)) {
                    continue;
                }
                index_mapping_[ei * rec_count + i] = true;
            }
            printf("\n");
        }
    }

    // Traverse all events in the active event set and find which entry matches
    for (int ei = 0; ei < active_num_events_; ei++) {
        double counter_value_sum = 0.0;

        for (size_t i = 0; i < rec_count; ++i) {
            if (index_mapping_[ei * rec_count + i]) {
                counter_value_sum += output_records[i].counter_value;
            }
        }
        counter_values_[ei] = counter_value_sum;
    }

    return ret_val;
}

//--------------------------------------------------------------------------------
// C API Implementation
//--------------------------------------------------------------------------------

extern "C" int
rocp_csdk_shutdown(void) {
    auto& sdk = RocpCSDK::instance();
    sdk.stopCounting();
    sdk.emptyActiveEventSet();
    sdk.deleteEventList();
    return RETVAL_SUCCESS;
}

extern "C" int
rocp_csdk_stop(void) {
    auto& sdk = RocpCSDK::instance();
    sdk.stopCounting();
    sdk.emptyActiveEventSet();
    return RETVAL_SUCCESS;
}

extern "C" int
rocp_csdk_start(const char** event_list, int event_count) {
    auto& sdk = RocpCSDK::instance();

    sdk.emptyActiveEventSet();

    int ret_val = sdk.setActiveEventSet(event_list, event_count);
    if (ret_val != RETVAL_SUCCESS) {
        return ret_val;
    }

    ret_val = sdk.setProfileCache();
    if (ret_val != RETVAL_SUCCESS) {
        sdk.emptyActiveEventSet();
        return ret_val;
    }

    sdk.startCounting(event_count);
    return RETVAL_SUCCESS;
}

extern "C" int
rocp_csdk_read(long long* values) {
    int ret_val = RETVAL_SUCCESS;
    auto& sdk = RocpCSDK::instance();

    // If the collection mode is DEVICE_SAMPLING get an explicit sample.
    if (ROCP_CSDK_MODE_DEVICE_SAMPLING == sdk.getProfilingMode()) {
        ret_val = sdk.readSample();
    }

    int cnt = sdk.getEventCount();
    long long int* tmp_val = sdk.getCounterValues();
    for (int i = 0; i < cnt; i++) {
        values[i] = tmp_val[i];
    }
    return ret_val;
}

//--------------------------------------------------------------------------------
// PC Sampling C API
//--------------------------------------------------------------------------------

extern "C" int
rocp_csdk_pcs_query_support(int device_id, rocp_csdk_pcs_caps_t* caps) {
    return RocpCSDK::instance().pcsQueryCapabilities(device_id, caps);
}

extern "C" int
rocp_csdk_pcs_start(rocp_csdk_pcs_config_t* config) {
    return RocpCSDK::instance().pcsStart(config);
}

extern "C" int
rocp_csdk_pcs_stop(void) {
    return RocpCSDK::instance().pcsStop();
}

extern "C" int
rocp_csdk_pcs_read(rocp_csdk_pcs_sample_t* samples, size_t* num_samples, size_t max_samples) {
    return RocpCSDK::instance().pcsRead(samples, num_samples, max_samples);
}

extern "C" uint64_t
rocp_csdk_pcs_get_drop_count(void) {
    return RocpCSDK::instance().pcsGetDropCount();
}

//--------------------------------------------------------------------------------
// Rocprofiler Entry Point
//--------------------------------------------------------------------------------

rocprofiler_tool_configure_result_t*
rocprofiler_configure(uint32_t version,
                      const char* runtime_version,
                      uint32_t priority,
                      rocprofiler_client_id_t* id) {
    // Set the client name
    id->name = "ROCPROFILER_SDK_C_WRAPPER";
    (void)setenv("ROCPROFILER_LOG_LEVEL", "fatal", 0);

    // Get pointer to singleton for use in callbacks
    auto* sdk_ptr = &RocpCSDK::instance();

    // Create configure data with lambdas that delegate to the singleton
    static auto cfg = rocprofiler_tool_configure_result_t{
        sizeof(rocprofiler_tool_configure_result_t),
        [](rocprofiler_client_finalize_t f, void* d) {
            return RocpCSDK::instance().init(f, d);
        },
        [](void* d) { RocpCSDK::instance().fini(d); },
        static_cast<void*>(sdk_ptr)
    };

    // Return pointer to configure data
    return &cfg;
}
