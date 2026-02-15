ROCP_SDK_ROOT ?= /opt/rocm-7.3.0

ROCP_SDK_INCL=-I$(ROCP_SDK_ROOT)/include     \
              -I$(ROCP_SDK_ROOT)/include/hsa \
              -I$(ROCP_SDK_ROOT)/hsa/include \
              -I$(ROCP_SDK_ROOT)/include/rocprofiler-sdk

AMDCXX   ?= amdclang++
CFLAGS    = $(OPTFLAGS)
CPPFLAGS += $(INCLUDE) $(ROCP_SDK_INCL)
LDFLAGS  += -L$(ROCP_SDK_ROOT)/lib -lrocprofiler-sdk

GPUARCH = $(shell rocm_agent_enumerator 2>/dev/null | grep -v "gfx000" | head -1)
ifneq ($(GPUARCH),)
    ARCHFLAG=--offload-arch=$(GPUARCH)
endif
GPUFLAGS=$(ARCHFLAG) --hip-link --rtlib=compiler-rt -unwindlib=libgcc

TESTS = driver driver_pcs
template_tests: $(TESTS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPTFLAGS) -D__HIP_PLATFORM_AMD__ -g -c -o $@ $<

rocp_csdk: rocp_csdk.cpp
	$(CXX) -D__HIP_PLATFORM_AMD__ -Bdynamic -fPIC -shared -Wl,-soname -Wl,libcntr.so $(ROCP_SDK_INCL) rocp_csdk.cpp -o libcntr.so

kernel.o: kernel.cpp
	$(AMDCXX) -D__HIP_ROCclr__=1 -O2 -g $(ARCHFLAG) -W -Wall -Wextra -Wshadow -o kernel.o -x hip -c kernel.cpp

driver: driver.o kernel.o rocp_csdk
	$(AMDCXX) -g $(GPUFLAGS) driver.o kernel.o -o driver $(LDFLAGS) -pthread -L. -lcntr $(ROCP_SDK_INCL)

driver_pcs.o: driver_pcs.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OPTFLAGS) -D__HIP_PLATFORM_AMD__ -g -c -o $@ $<

driver_pcs: driver_pcs.o kernel.o rocp_csdk
	$(AMDCXX) -g $(GPUFLAGS) driver_pcs.o kernel.o -o driver_pcs $(LDFLAGS) -pthread -L. -lcntr $(ROCP_SDK_INCL)

clean:
	rm -f $(TESTS) *.o *.so
