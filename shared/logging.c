#include "logging.h"
#include <stdio.h>

void common_log(void *log_outlet, long log_enabled, const char *object_name, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vcommon_log(log_outlet, log_enabled, object_name, fmt, args);
    va_end(args);
}

void vcommon_log(void *log_outlet, long log_enabled, const char *object_name, const char *fmt, va_list args) {
    if (log_enabled && log_outlet) {
        char buf[1024];
        char final_buf[1100];
        vsnprintf(buf, 1024, fmt, args);
        snprintf(final_buf, 1100, "%s: %s", object_name, buf);
        outlet_anything(log_outlet, gensym(final_buf), 0, NULL);
    }
}
