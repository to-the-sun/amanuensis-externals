#include "visualize.h"
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>

#pragma comment(lib, "ws2_32.lib")

#define TCP_PORT 9999
#define SERVER "127.0.0.1"

static SOCKET sock = INVALID_SOCKET;
static struct sockaddr_in server_addr;
static int ref_count = 0;
static DWORD last_connect_attempt = 0;

int visualize_init() {
    if (ref_count == 0) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
            return 1;
        }

        memset((char *) &server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(TCP_PORT);
        server_addr.sin_addr.S_un.S_addr = inet_addr(SERVER);

        sock = INVALID_SOCKET;
        last_connect_attempt = 0;
    }
    ref_count++;
    return 0;
}

void visualize_cleanup() {
    ref_count--;
    if (ref_count <= 0) {
        if (sock != INVALID_SOCKET) {
            closesocket(sock);
            sock = INVALID_SOCKET;
        }
        WSACleanup();
        ref_count = 0;
    }
}

/**
 * Sends a JSON message to the visualization script on port 9999.
 *
 * PERFORMANCE & THREADING:
 * - This function is synchronous in terms of execution (it runs on the calling thread).
 * - It uses NON-BLOCKING TCP sockets (FIONBIO). If a send would block or the connection
 *   is not yet established, it returns immediately to avoid slowing down Max.
 * - It implements a 2-second cooldown for connection attempts to minimize overhead
 *   when the visualization script is not running.
 * - While it uses non-blocking I/O, it does NOT operate on its own background thread.
 */
void visualize(const char *message) {
    if (sock == INVALID_SOCKET) {
        DWORD now = GetTickCount();
        if (now - last_connect_attempt < 2000) {
            return; // Cooldown: try connecting every 2 seconds
        }
        last_connect_attempt = now;

        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) return;

        // Set non-blocking mode
        u_long mode = 1;
        if (ioctlsocket(sock, FIONBIO, &mode) != 0) {
            closesocket(sock);
            sock = INVALID_SOCKET;
            return;
        }

        // Try to connect (non-blocking)
        if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
                closesocket(sock);
                sock = INVALID_SOCKET;
                return;
            }
        }
        // In non-blocking mode, connect usually returns WSAEWOULDBLOCK.
        // We'll attempt to send in subsequent calls; send() will fail with WSAENOTCONN
        // until the connection is established.
        return;
    }

    char buf[65536];
    int n = snprintf(buf, sizeof(buf), "%s\n", message);
    if (n < 0 || n >= (int)sizeof(buf)) {
        return;
    }

    int total_sent = 0;
    int len = n;
    while (total_sent < len) {
        int sent = send(sock, buf + total_sent, len - total_sent, 0);
        if (sent == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAENOTCONN || err == WSAEINPROGRESS) {
                // Not connected yet or buffer full.
                // If we've already sent part of the message, we MUST close to avoid
                // mangling the JSON stream on the next attempt.
                if (total_sent > 0) {
                    closesocket(sock);
                    sock = INVALID_SOCKET;
                }
                return;
            }
            // Hard error (e.g., connection reset)
            closesocket(sock);
            sock = INVALID_SOCKET;
            return;
        }
        if (sent == 0) {
            // Connection closed by peer
            closesocket(sock);
            sock = INVALID_SOCKET;
            return;
        }
        total_sent += sent;
    }
}
