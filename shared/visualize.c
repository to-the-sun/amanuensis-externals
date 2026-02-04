#include "visualize.h"
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>

#pragma comment(lib, "ws2_32.lib")

#define TCP_PORT 9999
#define SERVER "127.0.0.1"

static SOCKET sock = INVALID_SOCKET;
static struct sockaddr_in server_addr;

int visualize_init() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        return 1;
    }

    memset((char *) &server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(TCP_PORT);
    server_addr.sin_addr.S_un.S_addr = inet_addr(SERVER);
    
    return 0;
}

void visualize_cleanup() {
    if (sock != INVALID_SOCKET) {
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
    WSACleanup();
}

void visualize(const char *message) {
    if (sock == INVALID_SOCKET) {
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) return;

        if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
            closesocket(sock);
            sock = INVALID_SOCKET;
            return;
        }
    }

    char buf[8192];
    int n = snprintf(buf, sizeof(buf), "%s\n", message);
    if (n < 0 || n >= (int)sizeof(buf)) {
        // Message too long or error, skip for now
        return;
    }

    int total_sent = 0;
    int len = n;
    while (total_sent < len) {
        int sent = send(sock, buf + total_sent, len - total_sent, 0);
        if (sent == SOCKET_ERROR) {
            closesocket(sock);
            sock = INVALID_SOCKET;
            return;
        }
        total_sent += sent;
    }
}
