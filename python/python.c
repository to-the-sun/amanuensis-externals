#include "ext.h"
#include "ext_obex.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <process.h>

/**
 * @file python.c
 * @brief Max external that bridges to a Python script.
 */

typedef struct _python {
    t_object m_obj;
    void *m_outlet;

    t_symbol *script_path;
    long port;
    SOCKET sock;
    int connected;
    HANDLE child_process;
} t_python;

void *python_new(t_symbol *s, long argc, t_atom *argv);
void python_free(t_python *x);
void python_assist(t_python *x, void *b, long m, long a, char *s);
void python_anything(t_python *x, t_symbol *s, long argc, t_atom *argv);

static t_class *python_class;

void ext_main(void *r) {
    common_symbols_init();
    t_class *c = class_new("python", (method)python_new, (method)python_free, sizeof(t_python), 0L, A_GIMME, 0);

    class_addmethod(c, (method)python_anything, "anything", A_GIMME, 0);
    class_addmethod(c, (method)python_assist, "assist", A_CANT, 0);

    class_register(CLASS_BOX, c);
    python_class = c;
}

static void escape_json_string(const char *in, char *out, size_t out_size) {
    size_t i = 0, j = 0;
    while (in[i] && j < out_size - 4) {
        if (in[i] == '"') {
            out[j++] = '\\';
            out[j++] = '"';
        } else if (in[i] == '\\') {
            out[j++] = '\\';
            out[j++] = '\\';
        } else if (in[i] == '\n') {
            out[j++] = '\\';
            out[j++] = 'n';
        } else {
            out[j++] = in[i];
        }
        i++;
    }
    out[j] = '\0';
}

void *python_new(t_symbol *s, long argc, t_atom *argv) {
    t_python *x = (t_python *)object_alloc(python_class);

    if (x) {
        x->script_path = _sym_nothing;
        x->port = 0;
        x->sock = INVALID_SOCKET;
        x->connected = 0;
        x->child_process = NULL;

        if (argc > 0 && atom_gettype(argv) == A_SYM) {
            x->script_path = atom_getsym(argv);
        } else {
            object_error((t_object *)x, "script path required as first argument");
        }

        x->m_outlet = outlet_new((t_object *)x, NULL);

        if (x->script_path != _sym_nothing) {
            // Setup networking
            WSADATA wsa;
            WSAStartup(MAKEWORD(2,2), &wsa);

            // Find a free port
            SOCKET temp_sock = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in addr;
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = 0;
            int addr_len = sizeof(addr);
            bind(temp_sock, (struct sockaddr *)&addr, addr_len);
            getsockname(temp_sock, (struct sockaddr *)&addr, &addr_len);
            x->port = ntohs(addr.sin_port);
            closesocket(temp_sock);

            // Resolve paths
            char bridge_path[MAX_PATH_CHARS];
            char bridge_filename[MAX_FILENAME_CHARS] = "python_bridge.py";
            short pathid = 0;
            t_fourcc type = 0;

            // Try to locate python_bridge.py
            if (locatefile_extended(bridge_filename, &pathid, &type, NULL, 0) == 0) {
                path_toabsolutesystempath(pathid, bridge_filename, bridge_path);
            } else {
                strncpy(bridge_path, bridge_filename, MAX_PATH_CHARS);
            }

            char script_fullpath[MAX_PATH_CHARS];
            strncpy(script_fullpath, x->script_path->s_name, MAX_PATH_CHARS);
            if (locatefile_extended(script_fullpath, &pathid, &type, NULL, 0) == 0) {
                path_toabsolutesystempath(pathid, x->script_path->s_name, script_fullpath);
            }

            // Start python bridge
            char cmd[8192];
            // Basic sanitization: avoid double quotes in paths
            if (strchr(bridge_path, '"') || strchr(script_fullpath, '"')) {
                object_error((t_object *)x, "quotes not allowed in script paths");
                return x;
            }

            snprintf(cmd, sizeof(cmd), "python \"%s\" \"%s\" %ld", bridge_path, script_fullpath, x->port);

            STARTUPINFO si;
            PROCESS_INFORMATION pi;
            ZeroMemory(&si, sizeof(si));
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            ZeroMemory(&pi, sizeof(pi));

            if (CreateProcess(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
                x->child_process = pi.hProcess;
                CloseHandle(pi.hThread);

                // Try to connect (non-blockingish)
                int attempts = 0;
                while (attempts < 30) {
                    Sleep(200);
                    x->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                    struct sockaddr_in server_addr;
                    server_addr.sin_family = AF_INET;
                    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
                    server_addr.sin_port = htons((u_short)x->port);

                    if (connect(x->sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) != SOCKET_ERROR) {
                        x->connected = 1;
                        break;
                    }
                    closesocket(x->sock);
                    attempts++;
                }

                if (!x->connected) {
                    object_error((t_object *)x, "could not connect to python bridge on port %ld", x->port);
                }
            } else {
                object_error((t_object *)x, "failed to start python bridge process. command: %s", cmd);
            }
        }
    }
    return x;
}

void python_free(t_python *x) {
    if (x->connected) {
        closesocket(x->sock);
    }
    if (x->child_process) {
        TerminateProcess(x->child_process, 0);
        CloseHandle(x->child_process);
    }
    WSACleanup();
}

void python_anything(t_python *x, t_symbol *s, long argc, t_atom *argv) {
    if (!x->connected) {
        object_error((t_object *)x, "not connected to python bridge");
        return;
    }

    // Construct JSON payload
    char payload[16384];
    int pos = snprintf(payload, sizeof(payload), "{\"func\": \"%s\", \"args\": [", s->s_name);

    for (int i = 0; i < argc; i++) {
        if (pos >= sizeof(payload) - 1024) break; // Leave some room
        if (i > 0) pos += snprintf(payload + pos, sizeof(payload) - pos, ", ");

        switch (atom_gettype(argv + i)) {
            case A_LONG:
                pos += snprintf(payload + pos, sizeof(payload) - pos, "%ld", atom_getlong(argv + i));
                break;
            case A_FLOAT:
                pos += snprintf(payload + pos, sizeof(payload) - pos, "%f", atom_getfloat(argv + i));
                break;
            case A_SYM: {
                char escaped[1024];
                escape_json_string(atom_getsym(argv + i)->s_name, escaped, sizeof(escaped));
                pos += snprintf(payload + pos, sizeof(payload) - pos, "\"%s\"", escaped);
                break;
            }
            default:
                pos += snprintf(payload + pos, sizeof(payload) - pos, "null");
                break;
        }
    }
    if (pos < sizeof(payload) - 2)
        pos += snprintf(payload + pos, sizeof(payload) - pos, "]}");

    // Send: [int32 length][payload]
    int32_t len = pos;
    if (len >= sizeof(payload)) len = sizeof(payload) - 1;
    int32_t net_len = htonl(len);
    if (send(x->sock, (char *)&net_len, 4, 0) == SOCKET_ERROR) goto err;
    if (send(x->sock, payload, len, 0) == SOCKET_ERROR) goto err;

    // Receive response length
    int32_t resp_len_net;
    if (recv(x->sock, (char *)&resp_len_net, 4, 0) <= 0) goto err;
    int32_t resp_len = ntohl(resp_len_net);
    if (resp_len < 0 || resp_len > 1024*1024) goto err; // sanity check

    // Receive response payload
    char *resp_buf = (char *)sysmem_newptr(resp_len + 1);
    int received = 0;
    while (received < resp_len) {
        int n = recv(x->sock, resp_buf + received, resp_len - received, 0);
        if (n <= 0) break;
        received += n;
    }
    resp_buf[resp_len] = '\0';

    // Parse JSON response
    if (strstr(resp_buf, "\"status\": \"ok\"")) {
        char *result_start = strstr(resp_buf, "\"result\": [");
        if (result_start) {
            result_start += 11;
            char *result_end = strrchr(result_start, ']');
            if (result_end) {
                *result_end = '\0';

                t_atom atoms[256];
                int count = 0;
                char *p = result_start;
                while (*p && count < 256) {
                    while (*p == ' ' || *p == ',') p++;
                    if (!*p) break;

                    if (*p == '"') {
                        p++;
                        char *start = p;
                        while (*p && *p != '"') {
                            if (*p == '\\' && *(p+1) == '"') p += 2;
                            else p++;
                        }
                        if (*p) {
                            *p = '\0';
                            // Unescaping would be nice but for now we just take it as is
                            atom_setsym(atoms + count, gensym(start));
                            p++;
                        }
                    } else {
                        char *start = p;
                        while (*p && *p != ',' && *p != ' ' && *p != ']') p++;
                        char saved = *p;
                        *p = '\0';
                        if (strchr(start, '.')) {
                            atom_setfloat(atoms + count, (float)atof(start));
                        } else if (strcmp(start, "null") == 0) {
                            atom_setsym(atoms + count, _sym_nothing);
                        } else if (strcmp(start, "true") == 0) {
                            atom_setlong(atoms + count, 1);
                        } else if (strcmp(start, "false") == 0) {
                            atom_setlong(atoms + count, 0);
                        } else {
                            atom_setlong(atoms + count, atol(start));
                        }
                        *p = saved;
                    }
                    count++;
                }

                if (count > 0) {
                    if (atom_gettype(atoms) == A_SYM) {
                        outlet_anything(x->m_outlet, atom_getsym(atoms), count - 1, atoms + 1);
                    } else if (count == 1) {
                        if (atom_gettype(atoms) == A_LONG) outlet_int(x->m_outlet, atom_getlong(atoms));
                        else if (atom_gettype(atoms) == A_FLOAT) outlet_float(x->m_outlet, atom_getfloat(atoms));
                    } else {
                        outlet_list(x->m_outlet, NULL, count, atoms);
                    }
                }
            }
        }
    } else {
        char *msg_start = strstr(resp_buf, "\"message\": \"");
        if (msg_start) {
            msg_start += 12;
            char *msg_end = strchr(msg_start, '"');
            if (msg_end) *msg_end = '\0';
            object_error((t_object *)x, "Python error: %s", msg_start);
        }
    }

    sysmem_freeptr(resp_buf);
    return;

err:
    object_error((t_object *)x, "bridge communication error");
    x->connected = 0;
    if (x->sock != INVALID_SOCKET) {
        closesocket(x->sock);
        x->sock = INVALID_SOCKET;
    }
}

void python_assist(t_python *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "Function calls (list: selector=func, atoms=args)");
    } else {
        sprintf(s, "Python return value");
    }
}
