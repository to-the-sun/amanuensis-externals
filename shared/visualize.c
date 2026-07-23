#include "visualize.h"
#include "ext.h"
#include "ext_obex.h"
#include "ext_systhread.h"
#include "ext_buffer.h"
#include "z_dsp.h"
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

#pragma comment(lib, "ws2_32.lib")

#define PORT_CRUCIBLE 9999
#define PORT_WEAVER   8999
#define SERVER "127.0.0.1"
#define MAX_QUEUE_SIZE 100

typedef struct {
    SOCKET sock;
    struct sockaddr_in addr;
    DWORD last_connect_attempt;
    t_systhread_mutex mutex; // Per-socket mutex for thread-safe access
} t_viz_socket;

typedef struct _viz_queue_item {
    t_viz_socket *vs;
    void *x;    // Reference to Max object
    char *type; // Resolved type string
    char *message;
    struct _viz_queue_item *next;
} t_viz_queue_item;

static t_viz_socket crucible_viz = { INVALID_SOCKET, {0}, 0, NULL };
static t_viz_socket weaver_viz = { INVALID_SOCKET, {0}, 0, NULL };
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

// Background thread function prototype
void *viz_worker_thread(void *arg);

int visualize_init() {
    if (ref_count == 0) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
            return 1;
        }
        viz_socket_init(&crucible_viz, PORT_CRUCIBLE);
        viz_socket_init(&weaver_viz, PORT_WEAVER);

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

        WSACleanup();
        ref_count = 0;
    }
}

static const char *get_event_name_from_message(const char *message) {
    if (!message) return "unknown";
    if (strstr(message, "\"event\":\"repopulate\"")) {
        return "repopulate";
    } else if (strstr(message, "\"event\":\"new_span\"")) {
        return "new_span";
    } else if (strstr(message, "\"event\":\"replace\"")) {
        return "replace";
    } else if (strstr(message, "\"event\":\"cleanup\"")) {
        return "cleanup";
    } else if (strstr(message, "\"event\":\"fill_bar\"")) {
        return "fill_bar";
    } else if (message[0] == '{' && strstr(message, "\"tracks\":")) {
        return "clear/tracks";
    }
    return "unknown";
}

// Structure layout mappings for objects dynamically checked
static void get_object_log_info(void *x, long *out_log, long *out_visualize, void **out_log_outlet) {
    if (!x) {
        if (out_log) *out_log = 0;
        if (out_visualize) *out_visualize = 0;
        if (out_log_outlet) *out_log_outlet = NULL;
        return;
    }
    t_symbol *classname = object_classname(x);
    if (classname == gensym("crucible") || classname == gensym("rebar_crucible_internal")) {
        struct min_crucible {
            t_object s_obj;
            t_dictionary *challenger_dict;
            t_symbol *last_track_id;
            t_symbol *incumbent_dict_name;
            void *outlet_data;
            void *outlet_fill;
            void *outlet_reach_int;
            void *log_outlet;
            t_buffer_ref *buffer_ref;
            long log;
            long consume;
            long defer;
            long async;
            void *worker;
            long visualize;
        } *obj = (struct min_crucible *)x;
        if (out_log) *out_log = obj->log;
        if (out_visualize) *out_visualize = obj->visualize;
        if (out_log_outlet) *out_log_outlet = obj->log_outlet;
    } else if (classname == gensym("smartloop~")) {
        struct min_smartloop {
            t_pxobject s_obj;
            t_symbol *dict_name;
            void *out_start;
            void *out_end;
            void *log_outlet;
            t_clock *clock;
            long interval;
            t_buffer_ref *buffer_ref;
            long bar_warn_sent;
            long log;
            long visualize;
        } *obj = (struct min_smartloop *)x;
        if (out_log) *out_log = obj->log;
        if (out_visualize) *out_visualize = obj->visualize;
        if (out_log_outlet) *out_log_outlet = obj->log_outlet;
    } else if (classname == gensym("weaver~")) {
        struct min_weaver {
            t_pxobject t_obj;
            t_symbol *poly_prefix;
            long log;
            long visualize;
            void *log_outlet;
        } *obj = (struct min_weaver *)x;
        if (out_log) *out_log = obj->log;
        if (out_visualize) *out_visualize = obj->visualize;
        if (out_log_outlet) *out_log_outlet = obj->log_outlet;
    } else if (classname == gensym("buildspans") || classname == gensym("rebar_buildspans_internal")) {
        struct min_buildspans {
            t_object s_obj;
            t_dictionary *building;
            t_dictionary *tracks_ended_in_current_event;
            long current_track;
            double current_offset;
            double loop_start;
            t_buffer_ref *buffer_ref;
            t_symbol *s_buffer_name;
            t_symbol *current_palette;
            void *span_outlet;
            void *track_outlet;
            void *out_bar_data;
            void *log_outlet;
            long log;
            long visualize;
        } *obj = (struct min_buildspans *)x;
        if (out_log) *out_log = obj->log;
        if (out_visualize) *out_visualize = obj->visualize;
        if (out_log_outlet) *out_log_outlet = obj->log_outlet;
    } else {
        if (out_log) *out_log = 0;
        if (out_visualize) *out_visualize = 0;
        if (out_log_outlet) *out_log_outlet = NULL;
    }
}

static void viz_log(void *x, const char *fmt, ...) {
    long log_enabled = 0;
    long visualize_enabled = 0;
    void *log_outlet = NULL;
    get_object_log_info(x, &log_enabled, &visualize_enabled, &log_outlet);

    if (log_enabled && log_outlet) {
        char buf[4096];
        char final_buf[4200];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, 4096, fmt, args);
        va_end(args);

        t_symbol *classname = object_classname(x);
        snprintf(final_buf, 4200, "%s: %s", classname ? classname->s_name : "visualize", buf);
        outlet_anything(log_outlet, gensym(final_buf), 0, NULL);
    }
}

// Internal helper for socket connection logic
// ASSUMES MUTEX FOR vs IS ALREADY HELD
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
                    object_error((t_object *)x, "visualize: TCP connect failed with error %d", err);
                }
                closesocket(vs->sock);
                vs->sock = INVALID_SOCKET;
            } else {
                if (x) {
                    viz_log(x, "visualize: TCP connection initiated (non-blocking, handshaking...)");
                }

                // Poll for handshake completion using select()
                fd_set write_fds, except_fds;
                struct timeval tv;
                tv.tv_sec = 0;
                tv.tv_usec = 500000; // 500ms timeout

                FD_ZERO(&write_fds);
                FD_ZERO(&except_fds);
                FD_SET(vs->sock, &write_fds);
                FD_SET(vs->sock, &except_fds);

                int sel_ret = select((int)vs->sock + 1, NULL, &write_fds, &except_fds, &tv);
                if (sel_ret > 0) {
                    if (FD_ISSET(vs->sock, &except_fds)) {
                        if (x) {
                            object_error((t_object *)x, "visualize: TCP handshake failed (exception set)");
                        }
                        closesocket(vs->sock);
                        vs->sock = INVALID_SOCKET;
                    } else if (FD_ISSET(vs->sock, &write_fds)) {
                        int valopt;
                        int lon = sizeof(int);
                        if (getsockopt(vs->sock, SOL_SOCKET, SO_ERROR, (char*)(&valopt), &lon) < 0) {
                            if (x) {
                                object_error((t_object *)x, "visualize: getsockopt failed during handshake");
                            }
                            closesocket(vs->sock);
                            vs->sock = INVALID_SOCKET;
                        } else if (valopt != 0) {
                            if (x) {
                                object_error((t_object *)x, "visualize: TCP handshake failed with socket error %d", valopt);
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
                        object_error((t_object *)x, "visualize: TCP handshake timed out (500ms)");
                    }
                    closesocket(vs->sock);
                    vs->sock = INVALID_SOCKET;
                } else {
                    if (x) {
                        object_error((t_object *)x, "visualize: select failed during TCP handshake");
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

// Internal helper for socket sending logic
// ASSUMES MUTEX FOR vs IS ALREADY HELD
static int perform_send(t_viz_socket *vs, void *x, const char *type, const char *message) {
    const char *ev = get_event_name_from_message(message);
    if (x) {
        viz_log(x, "visualize: calling ensure_connected for event '%s'...", ev);
    }
    ensure_connected(vs, x);
    if (vs->sock == INVALID_SOCKET) {
        if (x) {
            object_warn((t_object *)x, "visualize: perform_send aborting for event '%s', not connected to visualizer server", ev);
        }
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
                tv.tv_sec = 1; // Wait up to 1 second
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

    // If visualize is not enabled, do not send at all
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
    viz_log(x, "visualize: enqueuing %s packet (queue count: %d, type: '%s')", ev, queue_count, type_static);

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

int visualize_exchange(void *x, const char *message, char *response, size_t response_size) {
    if (!x || !message || !response || response_size == 0) return -1;

    long log_enabled = 0;
    long visualize_enabled = 0;
    void *log_outlet = NULL;
    get_object_log_info(x, &log_enabled, &visualize_enabled, &log_outlet);

    // If visualize is not enabled, do not send/exchange at all
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
