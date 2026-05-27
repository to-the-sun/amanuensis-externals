#include "visualize.h"
#include "ext.h"
#include "ext_obex.h"
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>

#pragma comment(lib, "ws2_32.lib")

#define PORT_CRUCIBLE 9999
#define PORT_WEAVER   8999
#define SERVER "127.0.0.1"

typedef struct {
    SOCKET sock;
    struct sockaddr_in addr;
    DWORD last_connect_attempt;
} t_viz_socket;

static t_viz_socket crucible_viz = { INVALID_SOCKET, {0}, 0 };
static t_viz_socket weaver_viz = { INVALID_SOCKET, {0}, 0 };
static int ref_count = 0;

static void viz_socket_init(t_viz_socket *vs, int port) {
    memset((char *) &vs->addr, 0, sizeof(vs->addr));
    vs->addr.sin_family = AF_INET;
    vs->addr.sin_port = htons(port);
    vs->addr.sin_addr.S_un.S_addr = inet_addr(SERVER);
    vs->sock = INVALID_SOCKET;
    vs->last_connect_attempt = 0;
}

int visualize_init() {
    if (ref_count == 0) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
            return 1;
        }
        viz_socket_init(&crucible_viz, PORT_CRUCIBLE);
        viz_socket_init(&weaver_viz, PORT_WEAVER);
    }
    ref_count++;
    return 0;
}

void visualize_cleanup() {
    ref_count--;
    if (ref_count <= 0) {
        if (crucible_viz.sock != INVALID_SOCKET) closesocket(crucible_viz.sock);
        if (weaver_viz.sock != INVALID_SOCKET) closesocket(weaver_viz.sock);
        crucible_viz.sock = INVALID_SOCKET;
        weaver_viz.sock = INVALID_SOCKET;
        WSACleanup();
        ref_count = 0;
    }
}

/**
 * Sends a JSON message to the visualization script on port 9999 (Crucible) or 8999 (Weaver/Building).
 *
 * PERFORMANCE & THREADING:
 * - This function is synchronous in terms of execution (it runs on the calling thread).
 * - It uses NON-BLOCKING TCP sockets (FIONBIO). If a send would block or the connection
 *   is not yet established, it returns immediately to avoid slowing down Max.
 * - It implements a 2-second cooldown for connection attempts to minimize overhead
 *   when the visualization script is not running.
 */
void visualize(void *x, const char *message) {
    if (!x || !message) return;

    t_symbol *classname = object_classname(x);
    t_viz_socket *vs = NULL;
    const char *type = "unknown";

    if (classname == gensym("crucible") || classname == gensym("rebar_crucible_internal")) {
        vs = &crucible_viz;
        type = "crucible";
    } else if (classname == gensym("weaver~")) {
        vs = &weaver_viz;
        type = "weaver";
    } else if (classname == gensym("buildspans") || classname == gensym("rebar_buildspans_internal")) {
        vs = &weaver_viz;
        type = "building";
    }

    if (!vs) return;

    if (vs->sock == INVALID_SOCKET) {
        DWORD now = GetTickCount();
        if (now - vs->last_connect_attempt < 2000) return;
        vs->last_connect_attempt = now;

        vs->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (vs->sock == INVALID_SOCKET) return;

        u_long mode = 1;
        if (ioctlsocket(vs->sock, FIONBIO, &mode) != 0) {
            closesocket(vs->sock);
            vs->sock = INVALID_SOCKET;
            return;
        }

        if (connect(vs->sock, (struct sockaddr *)&vs->addr, sizeof(vs->addr)) == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
                closesocket(vs->sock);
                vs->sock = INVALID_SOCKET;
                return;
            }
        }
        return;
    }

    long buf_size = 524288;
    char *buf = (char *)sysmem_newptr(buf_size);
    if (!buf) return;
    int n;

    if (message[0] == '{') {
        n = snprintf(buf, buf_size, "{\"type\":\"%s\",%s\n", type, message + 1);
    } else {
        n = snprintf(buf, buf_size, "%s\n", message);
    }

    if (n < 0 || n >= (int)buf_size) {
        sysmem_freeptr(buf);
        return;
    }

    int total_sent = 0;
    int len = n;
    while (total_sent < len) {
        int sent = send(vs->sock, buf + total_sent, len - total_sent, 0);
        if (sent == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAENOTCONN || err == WSAEINPROGRESS) {
                if (total_sent > 0) {
                    closesocket(vs->sock);
                    vs->sock = INVALID_SOCKET;
                }
                goto cleanup;
            }
            closesocket(vs->sock);
            vs->sock = INVALID_SOCKET;
            goto cleanup;
        }
        if (sent == 0) {
            closesocket(vs->sock);
            vs->sock = INVALID_SOCKET;
            goto cleanup;
        }
        total_sent += sent;
    }

cleanup:
    sysmem_freeptr(buf);
}
