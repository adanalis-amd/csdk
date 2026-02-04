#ifndef __ROCP_CSDK_H__
#define __ROCP_CSDK_H__

#define RETVAL_SUCCESS 0
#define RETVAL_FAIL    1

#ifdef __cplusplus
extern "C" {
#endif

#pragma GCC visibility push(default)

int rocp_csdk_start(const char **event_list, int event_count);
int rocp_csdk_stop();
int rocp_csdk_read(long long *counters);
int rocp_csdk_shutdown(void);

#pragma GCC visibility pop

#ifdef __cplusplus
}
#endif

#endif
