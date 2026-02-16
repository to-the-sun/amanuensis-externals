#ifndef LOGGING_H
#define LOGGING_H

#include "ext.h"
#include <stdarg.h>

/**
 * Generic logging function for Max objects
 */
void common_log(void *log_outlet, long log_enabled, const char *object_name, const char *fmt, ...);

/**
 * Generic logging function for Max objects (va_list version)
 */
void vcommon_log(void *log_outlet, long log_enabled, const char *object_name, const char *fmt, va_list args);

#endif // LOGGING_H
