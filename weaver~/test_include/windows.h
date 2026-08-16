#ifndef WINDOWS_MOCK_H
#define WINDOWS_MOCK_H

#include <stdint.h>
#include <stddef.h>
#include <sys/time.h>

typedef uint32_t DWORD;
typedef void *HANDLE;

#ifndef FALSE
#define FALSE 0
#endif

#ifndef INVALID_HANDLE_VALUE
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#endif

#ifndef PAGE_READWRITE
#define PAGE_READWRITE 0x04
#endif

#ifndef FILE_MAP_ALL_ACCESS
#define FILE_MAP_ALL_ACCESS 0xF001F
#endif

#ifndef INFINITE
#define INFINITE 0xFFFFFFFF
#endif

static inline DWORD GetTickCount(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (DWORD)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

static inline HANDLE CreateMutexA(void *a, int b, const char *c) { return (HANDLE)1; }
static inline HANDLE CreateFileMappingA(HANDLE a, void *b, DWORD c, DWORD d, DWORD e, const char *f) { return (HANDLE)1; }
static inline void *MapViewOfFile(HANDLE a, DWORD b, DWORD c, DWORD d, size_t e) {
    static char dummy_map[4096];
    return dummy_map;
}
static inline void UnmapViewOfFile(const void *p) {}
static inline void CloseHandle(HANDLE h) {}
static inline DWORD WaitForSingleObject(HANDLE h, DWORD ms) { return 0; }
static inline int ReleaseMutex(HANDLE h) { return 1; }

#endif // WINDOWS_MOCK_H
