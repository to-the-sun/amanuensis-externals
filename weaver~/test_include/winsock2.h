#ifndef WINSOCK2_MOCK_H
#define WINSOCK2_MOCK_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>

#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1

#define closesocket(s) close(s)
#define WSAGetLastError() errno

#define WSAEWOULDBLOCK EWOULDBLOCK
#define WSAEINPROGRESS EINPROGRESS

typedef uint16_t WORD;

#define MAKEWORD(a, b) ((uint16_t)(((uint8_t)(a)) | ((uint16_t)((uint8_t)(b))) << 8))

struct sockaddr_in_mock {
    short sin_family;
    unsigned short sin_port;
    struct {
        union {
            struct { unsigned char s_b1, s_b2, s_b3, s_b4; } S_un_b;
            struct { unsigned short s_w1, s_w2; } S_un_w;
            uint32_t S_addr;
        } S_un;
    } sin_addr;
    char sin_zero[8];
};

#define sockaddr_in sockaddr_in_mock

static inline int ioctlsocket(SOCKET s, long cmd, u_long *argp) {
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return -1;
    if (*argp) {
        return fcntl(s, F_SETFL, flags | O_NONBLOCK);
    } else {
        return fcntl(s, F_SETFL, flags & ~O_NONBLOCK);
    }
}

typedef struct {
    int dummy;
} WSADATA;

static inline int WSAStartup(WORD wVersionRequested, WSADATA *lpWSAData) {
    return 0; // success
}

static inline int WSACleanup(void) {
    return 0; // success
}

#endif // WINSOCK2_MOCK_H
