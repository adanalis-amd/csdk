#ifndef __ROCP_CSDK_H__
#define __ROCP_CSDK_H__

#include <stdint.h>
#include <stddef.h>

#define RETVAL_SUCCESS 0
#define RETVAL_FAIL    1

// Profiling modes
#define ROCP_CSDK_MODE_DISPATCH        0
#define ROCP_CSDK_MODE_DEVICE_SAMPLING 1
#define ROCP_CSDK_MODE_PC_SAMPLING     2

#ifdef __cplusplus
extern "C" {
#endif

#pragma GCC visibility push(default)

// ============================================================================
// Counter Profiling API
// ============================================================================

int rocp_csdk_start(const char **event_list, int event_count);
int rocp_csdk_stop(void);
int rocp_csdk_read(long long *counters);
int rocp_csdk_shutdown(void);

// ============================================================================
// PC Sampling API
// ============================================================================

// Sampling method selection
typedef enum {
    ROCP_CSDK_PCS_METHOD_AUTO = 0,      // Auto-select (prefers stochastic)
    ROCP_CSDK_PCS_METHOD_HOST_TRAP,     // MI200+ (gfx90a)
    ROCP_CSDK_PCS_METHOD_STOCHASTIC     // MI300+ (gfx942)
} rocp_csdk_pcs_method_t;

// Sampling interval unit
typedef enum {
    ROCP_CSDK_PCS_UNIT_AUTO = 0,        // Use default for method
    ROCP_CSDK_PCS_UNIT_TIME,            // Time-based intervals
    ROCP_CSDK_PCS_UNIT_CYCLES           // Cycle-based intervals
} rocp_csdk_pcs_unit_t;

// Configuration for PC sampling
typedef struct {
    int device_id;                       // -1 for all devices
    rocp_csdk_pcs_method_t method;       // Sampling method
    rocp_csdk_pcs_unit_t unit;           // Interval unit
    uint64_t interval;                   // Sampling interval (0 = default)
    size_t max_samples;                  // Max samples to buffer (0 = default 10000)
} rocp_csdk_pcs_config_t;

// Device PC sampling capabilities
typedef struct {
    int supported;                       // 1 if PC sampling supported
    int stochastic_available;            // 1 if stochastic method available
    int host_trap_available;             // 1 if host_trap method available
    uint64_t stochastic_min_interval;    // Min interval for stochastic
    uint64_t stochastic_max_interval;    // Max interval for stochastic
    uint64_t host_trap_min_interval;     // Min interval for host_trap
    uint64_t host_trap_max_interval;     // Max interval for host_trap
} rocp_csdk_pcs_caps_t;

// Simplified PC sample record (unified for both methods)
typedef struct {
    uint64_t code_object_id;             // Code object containing the PC
    uint64_t code_object_offset;         // Offset within code object
    uint64_t timestamp;                  // Sample timestamp
    uint64_t exec_mask;                  // Execution mask
    uint64_t dispatch_id;                // Dispatch identifier
    uint32_t device_id;                  // Logical GPU device ID
    uint32_t workgroup_x;                // Workgroup ID X
    uint32_t workgroup_y;                // Workgroup ID Y
    uint32_t workgroup_z;                // Workgroup ID Z
    uint8_t wave_in_group;               // Wave index within workgroup
    uint8_t wave_issued;                 // 1 if instruction was issued
    uint8_t inst_type;                   // Instruction type (if issued)
    uint8_t stall_reason;                // Stall reason (if not issued)
    uint8_t chiplet;                     // Chiplet ID
    uint8_t cu_id;                       // Compute unit ID
    uint8_t simd_id;                     // SIMD ID
    uint8_t shader_engine_id;            // Shader engine ID
} rocp_csdk_pcs_sample_t;

// PC Sampling C API
int rocp_csdk_pcs_query_support(int device_id, rocp_csdk_pcs_caps_t* caps);
int rocp_csdk_pcs_start(rocp_csdk_pcs_config_t* config);
int rocp_csdk_pcs_stop(void);
int rocp_csdk_pcs_read(rocp_csdk_pcs_sample_t* samples, size_t* num_samples, size_t max_samples);
uint64_t rocp_csdk_pcs_get_drop_count(void);

#pragma GCC visibility pop

#ifdef __cplusplus
}
#endif

#endif
