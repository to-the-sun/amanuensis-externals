#include "visualize.h"
#include "ext.h"
#include "ext_obex.h"
#include "ext_systhread.h"
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>

#pragma comment(lib, "ws2_32.lib")

#define PORT_CRUCIBLE 9999
#define PORT_WEAVER   8999
#define SERVER "127.0.0.1"
#define MAX_QUEUE_SIZE 100

typedef struct {
    SOCKET sock;
    struct sockaddr_in addr;
    DWORD last_connect_attempt;
    DWORD last_launch_attempt;
    char script_name[64];
    char script_path[MAX_PATH_CHARS];
    t_systhread_mutex mutex; // Per-socket mutex for thread-safe access
} t_viz_socket;

typedef struct _viz_queue_item {
    t_viz_socket *vs;
    void *x;    // Object context for logging/launching
    char *type; // Resolved type string
    char *message;
    struct _viz_queue_item *next;
} t_viz_queue_item;

static t_viz_socket crucible_viz = { INVALID_SOCKET };
static t_viz_socket weaver_viz = { INVALID_SOCKET };
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

static void viz_socket_init(t_viz_socket *vs, int port, const char *script_name, void *x) {
    memset((char *) &vs->addr, 0, sizeof(vs->addr));
    vs->addr.sin_family = AF_INET;
    vs->addr.sin_port = htons(port);
    vs->addr.sin_addr.S_un.S_addr = inet_addr(SERVER);
    vs->sock = INVALID_SOCKET;
    vs->last_connect_attempt = 0;
    vs->last_launch_attempt = 0;
    strncpy(vs->script_name, script_name, 64);
    vs->script_path[0] = '\0';
    systhread_mutex_new(&vs->mutex, 0);

    // Try to resolve absolute path for the script
    short pathid = 0;
    t_fourcc type = 0;
    char filename[MAX_FILENAME_CHARS];

    // 1. Try "shared/script_name"
    snprintf(filename, MAX_FILENAME_CHARS, "shared/%s", script_name);
    if (locatefile_extended(filename, &pathid, &type, NULL, 0) == 0) {
        path_toabsolutesystempath(pathid, filename, vs->script_path);
    }

    // 2. Try just "script_name"
    if (vs->script_path[0] == '\0') {
        strncpy(filename, script_name, MAX_FILENAME_CHARS);
        if (locatefile_extended(filename, &pathid, &type, NULL, 0) == 0) {
            path_toabsolutesystempath(pathid, filename, vs->script_path);
        }
    }

    // 3. Try relative to the object's path
    if (vs->script_path[0] == '\0' && x) {
        t_symbol *name = object_classname(x);
        char ext_filename[MAX_FILENAME_CHARS];
        snprintf(ext_filename, MAX_FILENAME_CHARS, "%s.mxe64", name->s_name);

        if (locatefile_extended(ext_filename, &pathid, &type, NULL, 0) == 0) {
            char ext_path[MAX_PATH_CHARS];
            path_toabsolutesystempath(pathid, ext_filename, ext_path);

            char *last_slash = strrchr(ext_path, '\\');
            if (!last_slash) last_slash = strrchr(ext_path, '/');
            if (last_slash) {
                *last_slash = '\0';
                // Try ../shared/script_name
                snprintf(vs->script_path, MAX_PATH_CHARS, "%s\\..\\shared\\%s", ext_path, script_name);
                if (GetFileAttributesA(vs->script_path) == INVALID_FILE_ATTRIBUTES) {
                    // Try shared/script_name
                    snprintf(vs->script_path, MAX_PATH_CHARS, "%s\\shared\\%s", ext_path, script_name);
                    if (GetFileAttributesA(vs->script_path) == INVALID_FILE_ATTRIBUTES) {
                         vs->script_path[0] = '\0';
                    }
                }
            }
        }
    }

    if (vs->script_path[0] != '\0') {
        // Normalize slashes to Windows style
        for (int i = 0; vs->script_path[i]; i++) {
            if (vs->script_path[i] == '/') vs->script_path[i] = '\\';
        }
        object_post(x, "visualize: found %s at %s", script_name, vs->script_path);
    } else {
        object_error(x, "visualize: could not locate %s", script_name);
    }
}

// Background thread function prototype
void *viz_worker_thread(void *arg);

int visualize_init(void *x) {
    if (ref_count == 0) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
            return 1;
        }
        viz_socket_init(&crucible_viz, PORT_CRUCIBLE, "visualizer.py", x);
        viz_socket_init(&weaver_viz, PORT_WEAVER, "debug_visualizer.py", x);

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

typedef struct _launch_ctx {
    t_viz_socket *vs;
    void *x;
} t_launch_ctx;

void visualize_launch_main_thread(void *x, t_symbol *s, short argc, t_atom *av) {
    t_launch_ctx *ctx = (t_launch_ctx *)x;
    if (!ctx) return;
    t_viz_socket *vs = ctx->vs;
    void *obj = ctx->x;

    if (!vs || vs->script_path[0] == '\0') {
        sysmem_freeptr(ctx);
        return;
    }

    object_post(obj, "visualize: attempting to launch %s", vs->script_name);
    object_post(obj, "visualize: script path: %s", vs->script_path);

    char cmd_args[MAX_PATH_CHARS + 16];
    snprintf(cmd_args, sizeof(cmd_args), "\"%s\"", vs->script_path);

    HINSTANCE res;

    // 1. Try 'python'
    res = ShellExecuteA(NULL, "open", "python", cmd_args, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)res <= 32) {
        object_error(obj, "visualize: failed with 'python' (%lld), trying 'pythonw'...", (long long)(INT_PTR)res);
        // 2. Try 'pythonw' (windowless interpreter)
        res = ShellExecuteA(NULL, "open", "pythonw", cmd_args, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)res <= 32) {
            object_error(obj, "visualize: failed with 'pythonw' (%lld), trying 'py'...", (long long)(INT_PTR)res);
            // 3. Try 'py'
            res = ShellExecuteA(NULL, "open", "py", cmd_args, NULL, SW_SHOWNORMAL);
            if ((INT_PTR)res <= 32) {
                object_error(obj, "visualize: failed with 'py' (%lld), trying direct execution...", (long long)(INT_PTR)res);
                // 4. Direct execution
                res = ShellExecuteA(NULL, "open", vs->script_path, NULL, NULL, SW_SHOWNORMAL);
                if ((INT_PTR)res <= 32) {
                    object_error(obj, "visualize: all launch attempts failed for %s (final error %lld)", vs->script_name, (long long)(INT_PTR)res);
                } else {
                    object_post(obj, "visualize: successfully launched %s via file association", vs->script_name);
                }
            } else {
                object_post(obj, "visualize: successfully launched %s via 'py'", vs->script_name);
            }
        } else {
            object_post(obj, "visualize: successfully launched %s via 'pythonw'", vs->script_name);
        }
    } else {
        object_post(obj, "visualize: successfully launched %s via 'python'", vs->script_name);
    }
    sysmem_freeptr(ctx);
}

// Internal helper for socket connection logic
// ASSUMES MUTEX FOR vs IS ALREADY HELD
static void ensure_connected(t_viz_socket *vs, void *x) {
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

                // Auto-launch if connection failed and we have a path
                if (vs->script_path[0] != '\0' && (now - vs->last_launch_attempt > 5000)) {
                    vs->last_launch_attempt = now;
                    object_post(x, "visualize: triggering deferred launch for %s", vs->script_name);

                    t_launch_ctx *ctx = (t_launch_ctx *)sysmem_newptr(sizeof(t_launch_ctx));
                    ctx->vs = vs;
                    ctx->x = x;
                    defer_low(ctx, (method)visualize_launch_main_thread, NULL, 0, NULL);
                }
            }
        }
    }
}

// Internal helper for socket sending logic
// ASSUMES MUTEX FOR vs IS ALREADY HELD
static int perform_send(t_viz_socket *vs, void *x, const char *type, const char *message) {
    ensure_connected(vs, x);
    if (vs->sock == INVALID_SOCKET) return -1;

    long buf_size = 524288;
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
        return -1;
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
                break;
            }
            closesocket(vs->sock);
            vs->sock = INVALID_SOCKET;
            break;
        }
        if (sent == 0) {
            closesocket(vs->sock);
            vs->sock = INVALID_SOCKET;
            break;
        }
        total_sent += sent;
    }

    sysmem_freeptr(buf);
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

    const char *type_static = NULL;
    t_viz_socket *vs = get_socket_for_object(x, &type_static);
    if (!vs) return;

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

    const char *type = NULL;
    t_viz_socket *vs = get_socket_for_object(x, &type);
    if (!vs) return -1;

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
