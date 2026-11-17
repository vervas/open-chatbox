#ifndef CHATBOX_PROF_H
#define CHATBOX_PROF_H

#ifdef CHATBOX_PROFILE

#include <time.h>
#include "lvgl.h"

#define PROF_START(_var) \
    struct timespec _var; \
    clock_gettime(CLOCK_MONOTONIC, &(_var))

#define PROF_ELAPSED_MS(_var) \
    ({ \
        struct timespec _prof_now; \
        clock_gettime(CLOCK_MONOTONIC, &_prof_now); \
        (long long)((_prof_now.tv_sec - (_var).tv_sec) * 1000LL + \
                    (_prof_now.tv_nsec - (_var).tv_nsec) / 1000000LL); \
    })

#define PROF_LOG(_var, _fmt, ...) \
    LV_LOG_INFO("[prof] " _fmt " took=%lld ms", \
                ##__VA_ARGS__, PROF_ELAPSED_MS(_var))

#define PROF_WARN_IF_OVER(_var, _threshold_ms, _fmt, ...) \
    do { \
        long long _ms = PROF_ELAPSED_MS(_var); \
        if (_ms >= (_threshold_ms)) { \
            LV_LOG_WARN("[prof] " _fmt " took=%lld ms (>%d ms)", \
                        ##__VA_ARGS__, _ms, (int)(_threshold_ms)); \
        } \
    } while (0)

#else

#define PROF_START(_var)                                  ((void)0)
#define PROF_ELAPSED_MS(_var)                             (0LL)
#define PROF_LOG(_var, _fmt, ...)                         ((void)0)
#define PROF_WARN_IF_OVER(_var, _threshold_ms, _fmt, ...) ((void)0)

#endif

#endif
