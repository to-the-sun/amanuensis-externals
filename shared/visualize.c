#include "visualize.h"
#include "ext.h"
#include "ext_obex.h"
#include "ext_systhread.h"
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

#pragma comment(lib, "ws2_32.lib")

#define PORT_CRUCIBLE 9999
#define PORT_WEAVER   8999
#define PORT_ANALYZE  9001
#define SERVER "127.0.0.1"
#define MAX_QUEUE_SIZE 100
#define MAX_DYNAMIC_SOCKETS 64

typedef struct {
    SOCKET sock;
    struct sockaddr_in addr;
    DWORD last_connect_attempt;
    t_systhread_mutex mutex; // Per-socket mutex for thread-safe access
} t_viz_socket;

typedef struct {
    int port;
    t_viz_socket vs;
    int in_use;
} t_dynamic_socket;

typedef struct _viz_queue_item {
    t_viz_socket *vs;
    void *x;    // Reference to Max object
    char *type; // Resolved type string
    char *message;
    struct _viz_queue_item *next;
} t_viz_queue_item;

static t_viz_socket crucible_viz = { INVALID_SOCKET, {0}, 0, NULL };
static t_viz_socket weaver_viz = { INVALID_SOCKET, {0}, 0, NULL };
static t_viz_socket analyze_viz = { INVALID_SOCKET, {0}, 0, NULL };

static t_dynamic_socket dynamic_sockets[MAX_DYNAMIC_SOCKETS];
static t_systhread_mutex dynamic_sockets_mutex = NULL;

#define SHARED_PORT_MAP_NAME "Local\\MaxAnalyzeVizSharedPorts"
#define SHARED_MUTEX_NAME    "Local\\MaxAnalyzeVizPortMutex"

typedef struct {
    int ports[MAX_DYNAMIC_SOCKETS];
} t_shared_port_map;

static HANDLE g_hSharedMap = NULL;
static t_shared_port_map *g_pSharedPortMap = NULL;
static HANDLE g_hSharedMutex = NULL;

static void shared_port_map_init(void) {
#if defined(WIN_VERSION) || defined(_WIN32)
    if (!g_hSharedMutex) {
        g_hSharedMutex = CreateMutexA(NULL, FALSE, SHARED_MUTEX_NAME);
    }
    if (!g_hSharedMap) {
        g_hSharedMap = CreateFileMappingA(
            INVALID_HANDLE_VALUE,
            NULL,
            PAGE_READWRITE,
            0,
            sizeof(t_shared_port_map),
            SHARED_PORT_MAP_NAME
        );
        if (g_hSharedMap) {
            g_pSharedPortMap = (t_shared_port_map *)MapViewOfFile(
                g_hSharedMap,
                FILE_MAP_ALL_ACCESS,
                0, 0,
                sizeof(t_shared_port_map)
            );
        }
    }
#endif
}

static void shared_port_map_lock(void) {
#if defined(WIN_VERSION) || defined(_WIN32)
    if (g_hSharedMutex) {
        WaitForSingleObject(g_hSharedMutex, INFINITE);
    }
#endif
}

static void shared_port_map_unlock(void) {
#if defined(WIN_VERSION) || defined(_WIN32)
    if (g_hSharedMutex) {
        ReleaseMutex(g_hSharedMutex);
    }
#endif
}

static int is_port_in_shared_map(int port) {
#if defined(WIN_VERSION) || defined(_WIN32)
    if (g_pSharedPortMap) {
        for (int i = 0; i < MAX_DYNAMIC_SOCKETS; i++) {
            if (g_pSharedPortMap->ports[i] == port) {
                return 1;
            }
        }
    }
#endif
    return 0;
}

static void add_port_to_shared_map(int port) {
#if defined(WIN_VERSION) || defined(_WIN32)
    if (g_pSharedPortMap) {
        for (int i = 0; i < MAX_DYNAMIC_SOCKETS; i++) {
            if (g_pSharedPortMap->ports[i] == 0) {
                g_pSharedPortMap->ports[i] = port;
                break;
            }
        }
    }
#endif
}

static void remove_port_from_shared_map(int port) {
#if defined(WIN_VERSION) || defined(_WIN32)
    if (g_pSharedPortMap) {
        for (int i = 0; i < MAX_DYNAMIC_SOCKETS; i++) {
            if (g_pSharedPortMap->ports[i] == port) {
                g_pSharedPortMap->ports[i] = 0;
                break;
            }
        }
    }
#endif
}

static int ref_count = 0;

static t_systhread viz_thread = NULL;
static t_systhread_mutex queue_mutex = NULL; // Mutex for queue operations
static t_systhread_cond viz_cond = NULL;
static t_viz_queue_item *queue_head = NULL;
static t_viz_queue_item *queue_tail = NULL;
static int queue_count = 0;
static int viz_exit_flag = 0;

static t_viz_socket *get_socket_for_object(void *x, const char **type_out) {
    t_symbol *classname = object_classname(x);
    if (classname == gensym("crucible") || classname == gensym("rebar_crucible_internal")) {
        if (type_out) *type_out = "crucible";
        return &crucible_viz;
    } else if (classname == gensym("smartloop~")) {
        if (type_out) *type_out = "smartloop";
        return &crucible_viz;
    } else if (classname == gensym("weaver~")) {
        if (type_out) *type_out = "weaver";
        return &weaver_viz;
    } else if (classname == gensym("buildspans") || classname == gensym("rebar_buildspans_internal")) {
        if (type_out) *type_out = "building";
        return &weaver_viz;
    }
    return NULL;
}

static void viz_socket_init(t_viz_socket *vs, int port) {
    memset((char *) &vs->addr, 0, sizeof(vs->addr));
    vs->addr.sin_family = AF_INET;
    vs->addr.sin_port = htons(port);
    vs->addr.sin_addr.S_un.S_addr = inet_addr(SERVER);
    vs->sock = INVALID_SOCKET;
    vs->last_connect_attempt = 0;
    systhread_mutex_new(&vs->mutex, 0);
}

void *viz_worker_thread(void *arg);

int visualize_init() {
    if (ref_count == 0) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
            return 1;
        }
        viz_socket_init(&crucible_viz, PORT_CRUCIBLE);
        viz_socket_init(&weaver_viz, PORT_WEAVER);
        viz_socket_init(&analyze_viz, PORT_ANALYZE);

        shared_port_map_init();

        systhread_mutex_new(&dynamic_sockets_mutex, 0);
        memset(dynamic_sockets, 0, sizeof(dynamic_sockets));

        systhread_mutex_new(&queue_mutex, 0);
        systhread_cond_new(&viz_cond, 0);
        viz_exit_flag = 0;
        systhread_create((method)viz_worker_thread, NULL, 0, 0, 0, &viz_thread);
    }
    ref_count++;
    return 0;
}

void visualize_cleanup() {
    ref_count--;
    if (ref_count <= 0) {
        systhread_mutex_lock(queue_mutex);
        viz_exit_flag = 1;
        systhread_cond_signal(viz_cond);
        systhread_mutex_unlock(queue_mutex);

        if (viz_thread) {
            unsigned int ret;
            systhread_join(viz_thread, &ret);
            viz_thread = NULL;
        }

        systhread_mutex_free(queue_mutex);
        systhread_cond_free(viz_cond);
        queue_mutex = NULL;
        viz_cond = NULL;

        if (dynamic_sockets_mutex) {
            systhread_mutex_lock(dynamic_sockets_mutex);
            for (int i = 0; i < MAX_DYNAMIC_SOCKETS; i++) {
                if (dynamic_sockets[i].in_use) {
                    systhread_mutex_lock(dynamic_sockets[i].vs.mutex);
                    if (dynamic_sockets[i].vs.sock != INVALID_SOCKET) {
                        closesocket(dynamic_sockets[i].vs.sock);
                        dynamic_sockets[i].vs.sock = INVALID_SOCKET;
                    }
                    systhread_mutex_unlock(dynamic_sockets[i].vs.mutex);
                    systhread_mutex_free(dynamic_sockets[i].vs.mutex);
                    dynamic_sockets[i].in_use = 0;
                }
            }
            systhread_mutex_unlock(dynamic_sockets_mutex);
            systhread_mutex_free(dynamic_sockets_mutex);
            dynamic_sockets_mutex = NULL;
        }

        systhread_mutex_lock(crucible_viz.mutex);
        if (crucible_viz.sock != INVALID_SOCKET) closesocket(crucible_viz.sock);
        crucible_viz.sock = INVALID_SOCKET;
        systhread_mutex_unlock(crucible_viz.mutex);
        systhread_mutex_free(crucible_viz.mutex);

        systhread_mutex_lock(weaver_viz.mutex);
        if (weaver_viz.sock != INVALID_SOCKET) closesocket(weaver_viz.sock);
        weaver_viz.sock = INVALID_SOCKET;
        systhread_mutex_unlock(weaver_viz.mutex);
        systhread_mutex_free(weaver_viz.mutex);

        systhread_mutex_lock(analyze_viz.mutex);
        if (analyze_viz.sock != INVALID_SOCKET) closesocket(analyze_viz.sock);
        analyze_viz.sock = INVALID_SOCKET;
        systhread_mutex_unlock(analyze_viz.mutex);
        systhread_mutex_free(analyze_viz.mutex);

#if defined(WIN_VERSION) || defined(_WIN32)
        if (g_pSharedPortMap) {
            UnmapViewOfFile(g_pSharedPortMap);
            g_pSharedPortMap = NULL;
        }
        if (g_hSharedMap) {
            CloseHandle(g_hSharedMap);
            g_hSharedMap = NULL;
        }
        if (g_hSharedMutex) {
            CloseHandle(g_hSharedMutex);
            g_hSharedMutex = NULL;
        }
#endif

        WSACleanup();
        ref_count = 0;
    }
}

int visualize_allocate_port(int start_port) {
    if (start_port <= 0) start_port = 9001;
    if (!dynamic_sockets_mutex) {
        return start_port;
    }

    shared_port_map_init();
    shared_port_map_lock();
    systhread_mutex_lock(dynamic_sockets_mutex);

    for (int port = start_port; port < start_port + 500; port++) {
        int already_used = 0;
        for (int i = 0; i < MAX_DYNAMIC_SOCKETS; i++) {
            if (dynamic_sockets[i].in_use && dynamic_sockets[i].port == port) {
                already_used = 1;
                break;
            }
        }
        if (already_used || is_port_in_shared_map(port)) continue;

        SOCKET test_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (test_sock != INVALID_SOCKET) {
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.S_un.S_addr = inet_addr(SERVER);

            int res = bind(test_sock, (struct sockaddr*)&addr, sizeof(addr));
            closesocket(test_sock);

            if (res == 0) {
                for (int i = 0; i < MAX_DYNAMIC_SOCKETS; i++) {
                    if (!dynamic_sockets[i].in_use) {
                        dynamic_sockets[i].port = port;
                        dynamic_sockets[i].in_use = 1;
                        viz_socket_init(&dynamic_sockets[i].vs, port);
                        add_port_to_shared_map(port);
                        systhread_mutex_unlock(dynamic_sockets_mutex);
                        shared_port_map_unlock();
                        return port;
                    }
                }
            }
        }
    }

    systhread_mutex_unlock(dynamic_sockets_mutex);
    shared_port_map_unlock();
    return start_port;
}

void visualize_close_port(int port) {
    if (port <= 0 || !dynamic_sockets_mutex) return;

    shared_port_map_lock();
    systhread_mutex_lock(dynamic_sockets_mutex);
    for (int i = 0; i < MAX_DYNAMIC_SOCKETS; i++) {
        if (dynamic_sockets[i].in_use && dynamic_sockets[i].port == port) {
            systhread_mutex_lock(dynamic_sockets[i].vs.mutex);
            if (dynamic_sockets[i].vs.sock != INVALID_SOCKET) {
                closesocket(dynamic_sockets[i].vs.sock);
                dynamic_sockets[i].vs.sock = INVALID_SOCKET;
            }
            systhread_mutex_unlock(dynamic_sockets[i].vs.mutex);
            systhread_mutex_free(dynamic_sockets[i].vs.mutex);

            dynamic_sockets[i].in_use = 0;
            dynamic_sockets[i].port = 0;
            remove_port_from_shared_map(port);
            break;
        }
    }
    systhread_mutex_unlock(dynamic_sockets_mutex);
    shared_port_map_unlock();
}

static const char *get_event_name_from_message(const char *message) {
    if (!message) return "unknown";
    if (strstr(message, "\"event\":\"repopulate\"")) {
        return "repopulate";
    } else if (strstr(message, "\"event\":\"new_span\"")) {
        return "new_span";
    } else if (strstr(message, "\"event\":\"replace\"")) {
        return "replace";
    } else if (strstr(message, "\"event\":\"fill_bar\"")) {
        return "fill_bar";
    } else if (strstr(message, "\"event\":\"clear\"")) {
        return "clear";
    } else if (message[0] == '{' && strstr(message, "\"tracks\":")) {
        return "clear/tracks";
    }
    return "unknown";
}

static void get_object_log_info(void *x, long *out_log, long *out_visualize, void **out_log_outlet) {
    if (!x) {
        if (out_log) *out_log = 0;
        if (out_visualize) *out_visualize = 0;
        if (out_log_outlet) *out_log_outlet = NULL;
        return;
    }

    if (out_log) *out_log = (long)object_attr_getlong(x, gensym("log"));
    if (out_visualize) *out_visualize = (long)object_attr_getlong(x, gensym("visualize"));

    if (out_log_outlet) {
        t_symbol *classname = object_classname(x);
        if (classname == gensym("crucible") || classname == gensym("rebar_crucible_internal")) {
            typedef struct _crucible_min_layout {
                t_object s_obj;
                t_dictionary *challenger_dict;
                t_symbol *last_track_id;
                t_symbol *incumbent_dict_name;
                void *outlet_data;
                void *outlet_rebar;
                void *outlet_reach_int;
                void *log_outlet;
            } t_crucible_min_layout;
            t_crucible_min_layout *cx = (t_crucible_min_layout *)x;
            *out_log_outlet = cx->log_outlet;
        } else {
            *out_log_outlet = NULL;
        }
    }
}

static void viz_log(void *x, const char *fmt, ...) {
    long log_enabled = 0;
    long visualize_enabled = 0;
    void *log_outlet = NULL;
    get_object_log_info(x, &log_enabled, &visualize_enabled, &log_outlet);

    if (log_enabled) {
        char buf[4096];
        char final_buf[4200];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, 4096, fmt, args);
        va_end(args);

        t_symbol *classname = object_classname(x);
        snprintf(final_buf, 4200, "%s: %s", classname ? classname->s_name : "visualize", buf);

        if (log_outlet) {
            outlet_anything(log_outlet, gensym(final_buf), 0, NULL);
        } else {
            object_post((t_object *)x, "%s", final_buf);
        }
    }
}

static void ensure_connected(t_viz_socket *vs, void *x) {
    if (vs->sock == INVALID_SOCKET) {
        if (x) {
            viz_log(x, "visualize: socket is closed, attempting connection to visualizer on 127.0.0.1:%d...", ntohs(vs->addr.sin_port));
        }
        DWORD now = GetTickCount();
        if (now - vs->last_connect_attempt < 2000) {
            if (x) {
                viz_log(x, "visualize: throttling connection attempt (too frequent)");
            }
            return;
        }
        vs->last_connect_attempt = now;

        vs->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (vs->sock == INVALID_SOCKET) {
            if (x) {
                object_error((t_object *)x, "visualize: socket creation failed");
            }
            return;
        }

        u_long mode = 1;
        if (ioctlsocket(vs->sock, FIONBIO, &mode) != 0) {
            if (x) {
                object_error((t_object *)x, "visualize: failed to set non-blocking mode");
            }
            closesocket(vs->sock);
            vs->sock = INVALID_SOCKET;
            return;
        }

        if (connect(vs->sock, (struct sockaddr *)&vs->addr, sizeof(vs->addr)) == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
                if (x) {
                    viz_log(x, "visualize: TCP connect failed with error %d", err);
                }
                closesocket(vs->sock);
                vs->sock = INVALID_SOCKET;
            } else {
                if (x) {
                    viz_log(x, "visualize: TCP connection initiated (non-blocking, handshaking...)");
                }

                fd_set write_fds, except_fds;
                struct timeval tv;
                tv.tv_sec = 0;
                tv.tv_usec = 50000; // 50ms timeout

                FD_ZERO(&write_fds);
                FD_ZERO(&except_fds);
                FD_SET(vs->sock, &write_fds);
                FD_SET(vs->sock, &except_fds);

                int sel_ret = select((int)vs->sock + 1, NULL, &write_fds, &except_fds, &tv);
                if (sel_ret > 0) {
                    if (FD_ISSET(vs->sock, &except_fds)) {
                        if (x) {
                            viz_log(x, "visualize: TCP handshake failed (exception set)");
                        }
                        closesocket(vs->sock);
                        vs->sock = INVALID_SOCKET;
                    } else if (FD_ISSET(vs->sock, &write_fds)) {
                        int valopt;
                        int lon = sizeof(int);
                        if (getsockopt(vs->sock, SOL_SOCKET, SO_ERROR, (char*)(&valopt), &lon) < 0) {
                            if (x) {
                                viz_log(x, "visualize: getsockopt failed during handshake");
                            }
                            closesocket(vs->sock);
                            vs->sock = INVALID_SOCKET;
                        } else if (valopt != 0) {
                            if (x) {
                                viz_log(x, "visualize: TCP handshake failed with socket error %d", valopt);
                            }
                            closesocket(vs->sock);
                            vs->sock = INVALID_SOCKET;
                        } else {
                            if (x) {
                                viz_log(x, "visualize: TCP handshake completed successfully via select!");
                            }
                        }
                    }
                } else if (sel_ret == 0) {
                    if (x) {
                        viz_log(x, "visualize: TCP handshake timed out (50ms)");
                    }
                    closesocket(vs->sock);
                    vs->sock = INVALID_SOCKET;
                } else {
                    if (x) {
                        viz_log(x, "visualize: select failed during TCP handshake");
                    }
                    closesocket(vs->sock);
                    vs->sock = INVALID_SOCKET;
                }
            }
        } else {
            if (x) {
                viz_log(x, "visualize: TCP socket connected immediately!");
            }
        }
    } else {
        if (x) {
            viz_log(x, "visualize: socket is already open and ready");
        }
    }
}

static int perform_send(t_viz_socket *vs, void *x, const char *type, const char *message) {
    const char *ev = get_event_name_from_message(message);
    if (x) {
        viz_log(x, "visualize: calling ensure_connected for event '%s'...", ev);
    }
    ensure_connected(vs, x);
    if (vs->sock == INVALID_SOCKET) {
        return -1;
    }

    long buf_size = strlen(message) + strlen(type) + 64;
    char *buf = (char *)sysmem_newptr(buf_size);
    if (!buf) return -1;

    int n;
    if (message[0] == '{') {
        n = snprintf(buf, buf_size, "{\"type\":\"%s\",%s\n", type, message + 1);
    } else {
        n = snprintf(buf, buf_size, "%s\n", message);
    }

    if (n <= 0 || n >= (int)buf_size) {
        sysmem_freeptr(buf);
        if (x) {
            object_error((t_object *)x, "visualize: packet formatting error or truncation for event '%s' (length: %d)", ev, n);
        }
        return -1;
    }

    if (x) {
        viz_log(x, "visualize: attempting socket send of %d bytes for event '%s' (type '%s')...", n, ev, type);
    }

    int total_sent = 0;
    int len = n;
    while (total_sent < len) {
        int sent = send(vs->sock, buf + total_sent, len - total_sent, 0);
        if (sent == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS) {
                if (x) {
                    viz_log(x, "visualize: send returned non-blocking delay code %d (sent %d/%d bytes) for event '%s', waiting for writability...", err, total_sent, len, ev);
                }

                fd_set write_fds;
                struct timeval tv;
                tv.tv_sec = 1;
                tv.tv_usec = 0;

                FD_ZERO(&write_fds);
                FD_SET(vs->sock, &write_fds);

                int sel_ret = select((int)vs->sock + 1, NULL, &write_fds, NULL, &tv);
                if (sel_ret > 0 && FD_ISSET(vs->sock, &write_fds)) {
                    if (x) {
                        viz_log(x, "visualize: socket became writable after WSAEWOULDBLOCK, retrying send...");
                    }
                    continue;
                } else {
                    if (x) {
                        object_error((t_object *)x, "visualize: select timed out or failed while waiting for writability (sent %d/%d bytes) for event '%s'", total_sent, len, ev);
                    }
                    closesocket(vs->sock);
                    vs->sock = INVALID_SOCKET;
                    break;
                }
            } else {
                if (x) {
                    object_error((t_object *)x, "visualize: send failed with socket error %d for event '%s'", err, ev);
                }
                closesocket(vs->sock);
                vs->sock = INVALID_SOCKET;
                break;
            }
        }
        if (sent == 0) {
            if (x) {
                object_warn((t_object *)x, "visualize: connection closed by remote visualizer server during event '%s'", ev);
            }
            closesocket(vs->sock);
            vs->sock = INVALID_SOCKET;
            break;
        }
        total_sent += sent;
    }

    sysmem_freeptr(buf);
    if (x) {
        viz_log(x, "visualize: perform_send complete for event '%s' (sent %d of %d bytes)", ev, total_sent, len);
    }
    return (total_sent == len) ? 0 : -1;
}

void *viz_worker_thread(void *arg) {
    systhread_set_name("visualize_worker");

    while (1) {
        t_viz_queue_item *item = NULL;

        systhread_mutex_lock(queue_mutex);
        while (queue_head == NULL && !viz_exit_flag) {
            systhread_cond_wait(viz_cond, queue_mutex);
        }

        if (viz_exit_flag && queue_head == NULL) {
            systhread_mutex_unlock(queue_mutex);
            break;
        }

        item = queue_head;
        if (item) {
            queue_head = item->next;
            if (queue_head == NULL) queue_tail = NULL;
            queue_count--;
        }
        systhread_mutex_unlock(queue_mutex);

        if (item) {
            systhread_mutex_lock(item->vs->mutex);
            perform_send(item->vs, item->x, item->type, item->message);
            systhread_mutex_unlock(item->vs->mutex);

            sysmem_freeptr(item->type);
            sysmem_freeptr(item->message);
            sysmem_freeptr(item);
        }
    }

    systhread_exit(0);
    return NULL;
}

void visualize(void *x, const char *message) {
    if (!x || !message || !queue_mutex) return;

    long log_enabled = 0;
    long visualize_enabled = 0;
    void *log_outlet = NULL;
    get_object_log_info(x, &log_enabled, &visualize_enabled, &log_outlet);

    if (!visualize_enabled) {
        return;
    }

    const char *type_static = NULL;
    t_viz_socket *vs = get_socket_for_object(x, &type_static);
    if (!vs) {
        object_warn((t_object *)x, "visualize: could not resolve socket for object");
        return;
    }

    const char *ev = get_event_name_from_message(message);

    systhread_mutex_lock(queue_mutex);
    if (queue_count >= MAX_QUEUE_SIZE) {
        object_error((t_object *)x, "visualize queue overflow (count: %d >= %d). Dropping packet.", queue_count, MAX_QUEUE_SIZE);
        systhread_mutex_unlock(queue_mutex);
        return;
    }

    t_viz_queue_item *item = (t_viz_queue_item *)sysmem_newptr(sizeof(t_viz_queue_item));
    if (!item) {
        systhread_mutex_unlock(queue_mutex);
        return;
    }

    item->vs = vs;
    item->x = x;
    item->type = (char *)sysmem_newptr(strlen(type_static) + 1);
    item->message = (char *)sysmem_newptr(strlen(message) + 1);

    if (!item->type || !item->message) {
        if (item->type) sysmem_freeptr(item->type);
        if (item->message) sysmem_freeptr(item->message);
        sysmem_freeptr(item);
        systhread_mutex_unlock(queue_mutex);
        return;
    }

    strcpy(item->type, type_static);
    strcpy(item->message, message);
    item->next = NULL;

    if (queue_tail) {
        queue_tail->next = item;
    } else {
        queue_head = item;
    }
    queue_tail = item;
    queue_count++;

    systhread_cond_signal(viz_cond);
    systhread_mutex_unlock(queue_mutex);
}

void visualize_to_port(void *x, int port, const char *type, const char *message) {
    if (!x || !message || !queue_mutex || port <= 0) return;

    long log_enabled = 0;
    long visualize_enabled = 0;
    void *log_outlet = NULL;
    get_object_log_info(x, &log_enabled, &visualize_enabled, &log_outlet);

    if (!visualize_enabled) {
        return;
    }

    t_viz_socket *vs = NULL;
    if (dynamic_sockets_mutex) {
        systhread_mutex_lock(dynamic_sockets_mutex);
        for (int i = 0; i < MAX_DYNAMIC_SOCKETS; i++) {
            if (dynamic_sockets[i].in_use && dynamic_sockets[i].port == port) {
                vs = &dynamic_sockets[i].vs;
                break;
            }
        }
        if (!vs) {
            for (int i = 0; i < MAX_DYNAMIC_SOCKETS; i++) {
                if (!dynamic_sockets[i].in_use) {
                    dynamic_sockets[i].port = port;
                    dynamic_sockets[i].in_use = 1;
                    viz_socket_init(&dynamic_sockets[i].vs, port);
                    vs = &dynamic_sockets[i].vs;
                    break;
                }
            }
        }
        systhread_mutex_unlock(dynamic_sockets_mutex);
    }

    if (!vs) {
        object_warn((t_object *)x, "visualize_to_port: could not resolve socket for port %d", port);
        return;
    }

    viz_log(x, "visualize_to_port: queuing packet of %ld bytes to port %d (type '%s')", (long)strlen(message), port, type);

    systhread_mutex_lock(queue_mutex);
    if (queue_count >= MAX_QUEUE_SIZE) {
        systhread_mutex_unlock(queue_mutex);
        return;
    }

    t_viz_queue_item *item = (t_viz_queue_item *)sysmem_newptr(sizeof(t_viz_queue_item));
    if (!item) {
        systhread_mutex_unlock(queue_mutex);
        return;
    }

    item->vs = vs;
    item->x = x;
    item->type = (char *)sysmem_newptr(strlen(type) + 1);
    item->message = (char *)sysmem_newptr(strlen(message) + 1);

    if (!item->type || !item->message) {
        if (item->type) sysmem_freeptr(item->type);
        if (item->message) sysmem_freeptr(item->message);
        sysmem_freeptr(item);
        systhread_mutex_unlock(queue_mutex);
        return;
    }

    strcpy(item->type, type);
    strcpy(item->message, message);
    item->next = NULL;

    if (queue_tail) {
        queue_tail->next = item;
    } else {
        queue_head = item;
    }
    queue_tail = item;
    queue_count++;

    systhread_cond_signal(viz_cond);
    systhread_mutex_unlock(queue_mutex);
}

int visualize_exchange(void *x, const char *message, char *response, size_t response_size) {
    if (!x || !message || !response || response_size == 0) return -1;

    long log_enabled = 0;
    long visualize_enabled = 0;
    void *log_outlet = NULL;
    get_object_log_info(x, &log_enabled, &visualize_enabled, &log_outlet);

    if (!visualize_enabled) {
        return -1;
    }

    const char *type = NULL;
    t_viz_socket *vs = get_socket_for_object(x, &type);
    if (!vs) {
        object_warn((t_object *)x, "visualize_exchange: could not resolve socket for object");
        return -1;
    }

    int received = -1;
    systhread_mutex_lock(vs->mutex);

    if (perform_send(vs, x, type, message) == 0) {
        fd_set read_fds;
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        FD_ZERO(&read_fds);
        FD_SET(vs->sock, &read_fds);

        int ret = select((int)vs->sock + 1, &read_fds, NULL, NULL, &tv);
        if (ret > 0) {
            received = recv(vs->sock, response, (int)response_size - 1, 0);
            if (received > 0) {
                response[received] = '\0';
                for (int i = received - 1; i >= 0 && (response[i] == '\n' || response[i] == '\r'); i--) {
                    response[i] = '\0';
                    received = i;
                }
            }
        }
    }

    systhread_mutex_unlock(vs->mutex);
    return received;
}
