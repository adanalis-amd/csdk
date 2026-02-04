#ifndef __ROCP_CSDK_INTERNAL_H__
#define __ROCP_CSDK_INTERNAL_H__

#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <rocprofiler-sdk/buffer.h>
#include <rocprofiler-sdk/registration.h>
#include <rocprofiler-sdk/device_counting_service.h>
#include <rocprofiler-sdk/rocprofiler.h>

#include <dlfcn.h>
#include <cxxabi.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <set>
#include <map>
#include <unordered_map>
#include <list>
#include <mutex>
#if (__cplusplus >= 201402L) // c++14
#include <shared_mutex>
#endif
#include <condition_variable>
#include <regex>
#include <string>
#include <string_view>
#include <sstream>
#include <thread>
#include <vector>
#include <algorithm>

#if defined(ROCP_CSDK_DEBUG)
#define ROCPROFILER_CALL(result, msg)                                                              \
    {                                                                                              \
        rocprofiler_status_t CHECKSTATUS = result;                                                 \
        if(CHECKSTATUS != ROCPROFILER_STATUS_SUCCESS)                                              \
        {                                                                                          \
            std::string status_msg = rocprofiler_get_status_string(CHECKSTATUS);                   \
            std::cerr << "[" #result "][" << __FILE__ << ":" << __LINE__ << "] " << msg            \
                      << " failed with error code " << CHECKSTATUS << ": " << status_msg           \
                      << std::endl;                                                                \
        }                                                                                          \
    }
#else
#define ROCPROFILER_CALL(result, msg) {(void)result;}
#endif

#define ROCP_CSDK_MODE_DISPATCH        (0)
#define ROCP_CSDK_MODE_DEVICE_SAMPLING (1)

#define ROCP_CSDK_AES_STOPPED (0x0)
#define ROCP_CSDK_AES_OPEN    (0x1)
#define ROCP_CSDK_AES_RUNNING (0x2)

//extern unsigned int _rocp_sdk_lock;

// Type definitions
using agent_map_t = std::map<uint64_t, const rocprofiler_agent_v0_t*>;
using dim_t = std::pair<uint64_t, unsigned long>;
using dim_vector_t = std::vector<dim_t>;

// Base event information
struct base_event_info_t {
    rocprofiler_counter_info_v0_t counter_info;
    std::vector<rocprofiler_record_dimension_info_t> dim_info;
};

// Event instance information (event with qualifiers)
struct event_instance_info_t {
    uint64_t qualifiers_present;
    std::string event_inst_name;
    rocprofiler_counter_info_v0_t counter_info;
    std::vector<rocprofiler_record_dimension_info_t> dim_info;
    dim_vector_t dim_instances;
    int device;
};

// Record information for dimension matching
struct rec_info_t {
    rocprofiler_counter_id_t counter_id;
    uint64_t device;
    dim_vector_t recorded_dims;
};

/**
 * @class RocpCSDK
 * @brief Main SDK class encapsulating all rocprofiler-sdk functionality.
 *
 * This class uses the singleton pattern to maintain a single instance
 * that manages GPU profiling state. The C API functions delegate to this
 * class through the instance() method.
 */
class RocpCSDK {
public:
    // Singleton access
    static RocpCSDK& instance();

    // Lifecycle
    int init(rocprofiler_client_finalize_t fini_func, void* tool_data);
    void fini(void* tool_data);

    // Profiling control
    void startCounting(int event_count);
    void stopCounting();
    int readSample();

    // Event management
    int evtNameToId(const std::string& event_name, unsigned int* event_id);
    int setActiveEventSet(const char** event_list, int num_events);
    void emptyActiveEventSet();
    int setProfileCache();

    // Event list management
    void populateEventList();
    void deleteEventList();

    // Getters
    int getProfilingMode() const;
    long long int* getCounterValues();
    int getEventCount() const;
    rocprofiler_context_id_t& getClientCtx();
    rocprofiler_buffer_id_t& getBuffer();

private:
    // Private constructor for singleton
    RocpCSDK();
    ~RocpCSDK();

    // Disable copy and assignment
    RocpCSDK(const RocpCSDK&) = delete;
    RocpCSDK& operator=(const RocpCSDK&) = delete;

    // Initialization helpers
    agent_map_t getGPUAgentInfo();

    // Event processing
    std::vector<rocprofiler_record_dimension_info_t> counterDimensions(rocprofiler_counter_id_t counter);
    bool dimensionsMatch(dim_vector_t dim_instances, dim_vector_t recorded_dims);
    int buildEventInfoFromName(const std::string& event_name, event_instance_info_t* ev_inst_info);
    int assignIdToEvent(const std::string& event_name, event_instance_info_t ev_inst_info);
    int evtNameToCode(const char* event_name, unsigned int* event_code);

    // Static callbacks (must be static for rocprofiler-sdk, access instance via user_data)
    static void recordCallback(rocprofiler_dispatch_counting_service_data_t dispatch_data,
                               rocprofiler_record_counter_t* record_data,
                               size_t record_count,
                               rocprofiler_user_data_t user_data,
                               void* callback_data_args);

    static void dispatchCallback(rocprofiler_dispatch_counting_service_data_t dispatch_data,
                                 rocprofiler_profile_config_id_t* config,
                                 rocprofiler_user_data_t* user_data,
                                 void* callback_data_args);

    static void setProfile(rocprofiler_context_id_t context_id,
                           rocprofiler_agent_id_t agent,
                           rocprofiler_agent_set_profile_callback_t set_config,
                           void* user_data);

    static void bufferedCallback(rocprofiler_context_id_t context_id,
                                 rocprofiler_buffer_id_t buffer_id,
                                 rocprofiler_record_header_t** headers,
                                 size_t num_headers,
                                 void* user_data,
                                 uint64_t drop_count);

    // Member variables (previously global)
    std::atomic<unsigned int> global_event_count_{0};
    std::atomic<unsigned int> base_event_count_{0};

    // Thread synchronization (version-dependent shared mutex)
#if (__cplusplus >= 201703L) // c++17
    std::shared_mutex profile_cache_mutex_;
#elif (__cplusplus >= 201402L) // c++14
    std::shared_timed_mutex profile_cache_mutex_;
#else
    std::mutex profile_cache_mutex_;
#endif
    std::mutex agent_mutex_;
    std::condition_variable agent_cond_var_;
    bool data_is_ready_{false};

    std::string error_string_;
    long long int* counter_values_{nullptr};
    int event_count_{0};
    int profiling_mode_{ROCP_CSDK_MODE_DEVICE_SAMPLING};

    agent_map_t gpu_agents_;
    std::unordered_map<std::string, base_event_info_t> base_events_by_name_;
    std::set<int> active_device_set_;
    unsigned int* active_event_ids_{nullptr};
    int active_num_events_{0};
    int active_state_{ROCP_CSDK_AES_STOPPED};
    std::vector<bool> index_mapping_;

    std::unordered_map<uint64_t, rocprofiler_profile_config_id_t> profile_cache_;
    std::unordered_map<unsigned int, event_instance_info_t> id_to_event_instance_;
    std::unordered_map<std::string, unsigned int> event_instance_name_to_id_;

    rocprofiler_context_id_t client_ctx_;
    rocprofiler_buffer_id_t buffer_;
};

#endif
