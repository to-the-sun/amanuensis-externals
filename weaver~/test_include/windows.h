#ifndef WINDOWS_MOCK_H
#define WINDOWS_MOCK_H

#include <stdint.h>
#include <sys/time.h>

typedef uint32_t DWORD;

static inline DWORD GetTickCount(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (DWORD)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

#endif // WINDOWS_MOCK_H
