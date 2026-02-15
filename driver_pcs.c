#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "hip.h"
#include "rocp_csdk.h"

extern int launch_kernel(int device_id);

static void print_sample(rocp_csdk_pcs_sample_t* sample, int index) {
    printf("Sample %4d: ", index);
    printf("CO=%lu offset=0x%lx ts=%lu dispatch=%lu ",
           sample->code_object_id,
           sample->code_object_offset,
           sample->timestamp,
           sample->dispatch_id);
    printf("WG=(%u,%u,%u) wave=%u ",
           sample->workgroup_x, sample->workgroup_y, sample->workgroup_z,
           sample->wave_in_group);
    printf("SE=%u CU=%u SIMD=%u ",
           sample->shader_engine_id, sample->cu_id, sample->simd_id);
    if (sample->wave_issued) {
        printf("ISSUED inst_type=%u", sample->inst_type);
    } else {
        printf("STALL reason=%u", sample->stall_reason);
    }
    printf("\n");
}

int main(int argc, char *argv[])
{
    int dev_count, which_device = 0;
    int ret;

    // Check for required environment variables
    if (getenv("ROCPROFILER_PC_SAMPLING_BETA_ENABLED") == NULL) {
        fprintf(stderr, "Error: ROCPROFILER_PC_SAMPLING_BETA_ENABLED not set.\n");
        fprintf(stderr, "PC sampling requires: export ROCPROFILER_PC_SAMPLING_BETA_ENABLED=1\n");
        return 1;
    }

    if (getenv("ROCP_CSDK_PC_SAMPLING_MODE") == NULL) {
        fprintf(stderr, "Error: ROCP_CSDK_PC_SAMPLING_MODE not set.\n");
        fprintf(stderr, "PC sampling requires: export ROCP_CSDK_PC_SAMPLING_MODE=1\n");
        return 1;
    }

    if (hipGetDeviceCount(&dev_count) != hipSuccess) {
        fprintf(stderr, "Error while counting AMD devices\n");
        return 1;
    }
    printf("Found %d GPU device(s)\n\n", dev_count);

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            which_device = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [-d device_id]\n", argv[0]);
            printf("  -d device_id  GPU device to use (default: 0)\n");
            printf("\nEnvironment variables:\n");
            printf("  ROCPROFILER_PC_SAMPLING_BETA_ENABLED=1  (required)\n");
            printf("  ROCP_CSDK_PC_SAMPLING_MODE=1            (required)\n");
            printf("  ROCP_CSDK_PCS_METHOD=auto|host_trap|stochastic (optional)\n");
            printf("  ROCP_CSDK_PCS_INTERVAL=<cycles_or_us>   (optional)\n");
            printf("  ROCP_CSDK_PCS_MAX_SAMPLES=<count>       (optional, default 50000)\n");
            return 0;
        }
    }

    // PC sampling is already active (started during rocprofiler_configure)
    printf("=== PC Sampling Active (initialized during tool load) ===\n\n");

    // Show configuration from environment
    const char* method = getenv("ROCP_CSDK_PCS_METHOD");
    const char* interval = getenv("ROCP_CSDK_PCS_INTERVAL");
    printf("Configuration:\n");
    printf("  Method: %s\n", method ? method : "auto (prefer stochastic)");
    printf("  Interval: %s\n", interval ? interval : "default");
    printf("\n");

    // Launch kernel
    printf("=== Launching Kernel on Device %d ===\n\n", which_device);
    int kernel_ret = launch_kernel(which_device);
    if (kernel_ret != 0) {
        fprintf(stderr, "launch_kernel failed with code %d\n", kernel_ret);
    }

    // Give some time for samples to be collected
    usleep(100000);  // 100ms

    // Read samples
    printf("=== Reading PC Samples ===\n\n");

    #define MAX_READ_BATCH 1000
    rocp_csdk_pcs_sample_t samples[MAX_READ_BATCH];
    size_t num_samples = 0;
    size_t total_samples = 0;
    int batch = 0;

    do {
        ret = rocp_csdk_pcs_read(samples, &num_samples, MAX_READ_BATCH);
        if (ret != RETVAL_SUCCESS) {
            fprintf(stderr, "Failed to read samples\n");
            break;
        }

        if (num_samples > 0) {
            printf("Batch %d: Read %zu samples\n", batch++, num_samples);

            // Print first few samples of each batch
            size_t print_count = (num_samples < 15) ? num_samples : 15;
            for (size_t i = 0; i < print_count; i++) {
                print_sample(&samples[i], (int)(total_samples + i));
            }
            if (num_samples > 15) {
                printf("  ... (%zu more samples in this batch)\n", num_samples - 15);
            }
            printf("\n");

            total_samples += num_samples;
        }
    } while (num_samples == MAX_READ_BATCH);  // Continue if buffer was full

    // Print summary
    uint64_t drop_count = rocp_csdk_pcs_get_drop_count();
    printf("=== Summary ===\n");
    printf("Total samples collected: %zu\n", total_samples);
    printf("Samples dropped: %lu\n", drop_count);

    return 0;
}
