#include "ext.h"
#include "ext_obex.h"
#include "ext_critical.h"
#include "ext_systhread.h"
#include "z_dsp.h"
#include "../shared/logging.h"
#include "../shared/visualize.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string.h>
#include <stdio.h>

/**
 * @file magenta~.c
 * @brief Max external for Magenta RealTime 2 synthesis bridge.
 */

#define MAGENTA_RING_BUFFER_SIZE 384000 // ~4 seconds of stereo at 48kHz (float32)
#define MAGENTA_MAX_CMD_LEN 1024

typedef enum {
    MAGENTA_CMD_NONE,
    MAGENTA_CMD_PROMPT,
    MAGENTA_CMD_MIDI,
    MAGENTA_CMD_MODEL,
    MAGENTA_CMD_OPEN
} t_magenta_cmd_type;

typedef struct _magenta_cmd {
    t_magenta_cmd_type type;
    char data[MAGENTA_MAX_CMD_LEN];
    struct _magenta_cmd *next;
} t_magenta_cmd;

typedef struct _magenta {
    t_pxobject m_obj;
    t_systhread thread;
    t_critical lock;
    int terminate;
    void *log_outlet;
    long log;

    // Networking
    t_symbol *address;
    long port;
    SOCKET sock;
    int connected;

    // Model state
    t_symbol *model;
    t_symbol *prompt;

    // Audio Buffer (Ring Buffer)
    float *audio_buffer;
    long buffer_head; // Written by thread
    long buffer_tail; // Read by DSP
    long buffer_size;

    // Command Queue
    t_magenta_cmd *cmd_head;
    t_magenta_cmd *cmd_tail;

    double sr;
} t_magenta;

void *magenta_new(t_symbol *s, long argc, t_atom *argv);
void magenta_free(t_magenta *x);
void magenta_assist(t_magenta *x, void *b, long m, long a, char *s);
void magenta_connect(t_magenta *x);
void magenta_disconnect(t_magenta *x);
void magenta_open(t_magenta *x);
void magenta_prompt(t_magenta *x, t_symbol *s);
void magenta_list(t_magenta *x, t_symbol *s, long argc, t_atom *argv);
void *magenta_thread_proc(t_magenta *x);
void magenta_dsp64(t_magenta *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
void magenta_perform64(t_magenta *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);
void magenta_log(t_magenta *x, const char *fmt, ...);
void magenta_push_cmd(t_magenta *x, t_magenta_cmd_type type, const char *data);

static t_class *magenta_class;

void magenta_log(t_magenta *x, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vcommon_log(x->log_outlet, x->log, "magenta~", fmt, args);
    va_end(args);
}

void ext_main(void *r) {
    common_symbols_init();
    t_class *c = class_new("magenta~", (method)magenta_new, (method)magenta_free, sizeof(t_magenta), 0L, A_GIMME, 0);

    class_addmethod(c, (method)magenta_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)magenta_assist, "assist", A_CANT, 0);
    class_addmethod(c, (method)magenta_connect, "connect", 0);
    class_addmethod(c, (method)magenta_disconnect, "disconnect", 0);
    class_addmethod(c, (method)magenta_open, "open", 0);
    class_addmethod(c, (method)magenta_prompt, "prompt", A_SYM, 0);
    class_addmethod(c, (method)magenta_list, "list", A_GIMME, 0);

    CLASS_ATTR_SYM(c, "address", 0, t_magenta, address);
    CLASS_ATTR_DEFAULT(c, "address", 0, "127.0.0.1");

    CLASS_ATTR_LONG(c, "port", 0, t_magenta, port);
    CLASS_ATTR_DEFAULT_SAVE(c, "port", 0, "9998");

    CLASS_ATTR_SYM(c, "model", 0, t_magenta, model);
    CLASS_ATTR_DEFAULT(c, "model", 0, "mrt2_small");

    CLASS_ATTR_LONG(c, "log", 0, t_magenta, log);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "onoff", "Enable Logging");
    CLASS_ATTR_DEFAULT(c, "log", 0, "0");

    class_dspinit(c);
    class_register(CLASS_BOX, c);
    magenta_class = c;
}

void *magenta_new(t_symbol *s, long argc, t_atom *argv) {
    t_magenta *x = (t_magenta *)object_alloc(magenta_class);

    if (x) {
        x->log = 0;
        x->terminate = 0;
        x->thread = NULL;
        x->connected = 0;
        x->sock = INVALID_SOCKET;
        x->address = gensym("127.0.0.1");
        x->port = 9998;
        x->model = gensym("mrt2_small");
        x->prompt = _sym_nothing;

        x->buffer_size = MAGENTA_RING_BUFFER_SIZE;
        x->audio_buffer = (float *)sysmem_newptr(x->buffer_size * sizeof(float));
        x->buffer_head = 0;
        x->buffer_tail = 0;

        x->cmd_head = NULL;
        x->cmd_tail = NULL;

        attr_args_process(x, argc, argv);

        x->log_outlet = outlet_new((t_object *)x, NULL);
        outlet_new((t_object *)x, "signal");
        outlet_new((t_object *)x, "signal");

        dsp_setup((t_pxobject *)x, 1);

        critical_new(&x->lock);
        magenta_log(x, "initialized");
    }
    return x;
}

void magenta_free(t_magenta *x) {
    dsp_free((t_pxobject *)x);
    magenta_disconnect(x);

    if (x->audio_buffer) sysmem_freeptr(x->audio_buffer);

    critical_enter(x->lock);
    t_magenta_cmd *cmd = x->cmd_head;
    while (cmd) {
        t_magenta_cmd *next = cmd->next;
        sysmem_freeptr(cmd);
        cmd = next;
    }
    critical_exit(x->lock);

    critical_free(x->lock);
}

void magenta_push_cmd(t_magenta *x, t_magenta_cmd_type type, const char *data) {
    t_magenta_cmd *cmd = (t_magenta_cmd *)sysmem_newptr(sizeof(t_magenta_cmd));
    if (cmd) {
        cmd->type = type;
        if (data) {
            strncpy(cmd->data, data, MAGENTA_MAX_CMD_LEN - 1);
            cmd->data[MAGENTA_MAX_CMD_LEN - 1] = '\0';
        } else {
            cmd->data[0] = '\0';
        }
        cmd->next = NULL;

        critical_enter(x->lock);
        if (x->cmd_tail) {
            x->cmd_tail->next = cmd;
            x->cmd_tail = cmd;
        } else {
            x->cmd_head = cmd;
            x->cmd_tail = cmd;
        }
        critical_exit(x->lock);
    }
}

void magenta_connect(t_magenta *x) {
    if (x->thread) {
        object_warn((t_object *)x, "already connected or connecting");
        return;
    }
    x->terminate = 0;
    systhread_create((method)magenta_thread_proc, x, 0, 0, 0, &x->thread);
}

void magenta_disconnect(t_magenta *x) {
    critical_enter(x->lock);
    x->terminate = 1;
    if (x->sock != INVALID_SOCKET) {
        closesocket(x->sock);
        x->sock = INVALID_SOCKET;
    }
    critical_exit(x->lock);

    if (x->thread) {
        unsigned int ret;
        systhread_join(x->thread, &ret);
        x->thread = NULL;
    }
    x->connected = 0;
    magenta_log(x, "disconnected");
}

void magenta_open(t_magenta *x) {
    magenta_push_cmd(x, MAGENTA_CMD_OPEN, NULL);
}

void magenta_prompt(t_magenta *x, t_symbol *s) {
    x->prompt = s;
    magenta_push_cmd(x, MAGENTA_CMD_PROMPT, s->s_name);
}

void magenta_list(t_magenta *x, t_symbol *s, long argc, t_atom *argv) {
    if (argc >= 2) {
        char buf[64];
        snprintf(buf, 64, "%ld,%ld,%ld", atom_getlong(argv), atom_getlong(argv+1), argc > 2 ? atom_getlong(argv+2) : 0);
        magenta_push_cmd(x, MAGENTA_CMD_MIDI, buf);
    }
}

void *magenta_thread_proc(t_magenta *x) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        object_error((t_object *)x, "magenta~: WSAStartup failed (Error %d)", WSAGetLastError());
        return NULL;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((u_short)x->port);

    if (inet_pton(AF_INET, x->address->s_name, &server_addr.sin_addr) <= 0) {
        object_error((t_object *)x, "magenta~: invalid address '%s'", x->address->s_name);
        WSACleanup();
        return NULL;
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        object_error((t_object *)x, "magenta~: socket creation failed (Error %d)", WSAGetLastError());
        WSACleanup();
        return NULL;
    }

    magenta_log(x, "attempting to connect to %s:%ld...", x->address->s_name, x->port);
    if (connect(s, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        object_error((t_object *)x, "magenta~: connection to %s:%ld failed (Error %d)", x->address->s_name, x->port, WSAGetLastError());
        closesocket(s);
        WSACleanup();
        return NULL;
    }

    critical_enter(x->lock);
    x->sock = s;
    x->connected = 1;
    x->buffer_head = 0;
    x->buffer_tail = 0;
    critical_exit(x->lock);

    magenta_log(x, "connected to bridge at %s:%ld", x->address->s_name, x->port);

    // Send initial model and prompt
    char init_model[MAGENTA_MAX_CMD_LEN];
    snprintf(init_model, MAGENTA_MAX_CMD_LEN, "{\"event\":\"model\",\"name\":\"%s\"}\n", x->model->s_name);
    send(s, init_model, (int)strlen(init_model), 0);

    if (x->prompt != _sym_nothing) {
        char init_prompt[MAGENTA_MAX_CMD_LEN];
        snprintf(init_prompt, MAGENTA_MAX_CMD_LEN, "{\"event\":\"prompt\",\"text\":\"%s\"}\n", x->prompt->s_name);
        send(s, init_prompt, (int)strlen(init_prompt), 0);
    }

    while (!x->terminate) {
        // 1. Send pending commands
        critical_enter(x->lock);
        t_magenta_cmd *cmd = x->cmd_head;
        x->cmd_head = NULL;
        x->cmd_tail = NULL;
        critical_exit(x->lock);

        while (cmd) {
            char json[MAGENTA_MAX_CMD_LEN + 128];
            if (cmd->type == MAGENTA_CMD_PROMPT) {
                snprintf(json, sizeof(json), "{\"event\":\"prompt\",\"text\":\"%s\"}\n", cmd->data);
            } else if (cmd->type == MAGENTA_CMD_MIDI) {
                snprintf(json, sizeof(json), "{\"event\":\"midi\",\"data\":[%s]}\n", cmd->data);
            } else if (cmd->type == MAGENTA_CMD_MODEL) {
                snprintf(json, sizeof(json), "{\"event\":\"model\",\"name\":\"%s\"}\n", cmd->data);
            } else if (cmd->type == MAGENTA_CMD_OPEN) {
                snprintf(json, sizeof(json), "{\"event\":\"open_gui\"}\n");
            }

            if (send(s, json, (int)strlen(json), 0) == SOCKET_ERROR) {
                magenta_log(x, "failed to send command (Error %d)", WSAGetLastError());
            }

            t_magenta_cmd *next = cmd->next;
            sysmem_freeptr(cmd);
            cmd = next;
        }

        // 2. Receive data [int32 length][data...]
        int32_t len_prefix;
        int r = recv(s, (char *)&len_prefix, 4, 0);
        if (r <= 0) {
            if (r == 0) magenta_log(x, "server closed connection");
            else magenta_log(x, "receive error (Error %d)", WSAGetLastError());
            break;
        }

        if (len_prefix > 0) {
            // Audio data
            int bytes_to_read = len_prefix;
            int actual_read_cap = (bytes_to_read > 1048576) ? 1048576 : bytes_to_read;

            char *buf = (char *)sysmem_newptr(actual_read_cap);
            int total_read = 0;
            while (total_read < bytes_to_read) {
                int to_recv = (bytes_to_read - total_read);
                if (total_read < actual_read_cap) {
                    if (to_recv > actual_read_cap - total_read) to_recv = actual_read_cap - total_read;
                    int n = recv(s, buf + total_read, to_recv, 0);
                    if (n <= 0) break;
                    total_read += n;
                } else {
                    char drain[4096];
                    if (to_recv > 4096) to_recv = 4096;
                    int n = recv(s, drain, to_recv, 0);
                    if (n <= 0) break;
                    total_read += n;
                }
            }

            if (total_read == bytes_to_read && buf) {
                float *audio = (float *)buf;
                int samples = actual_read_cap / sizeof(float);
                critical_enter(x->lock);
                for (int i = 0; i < samples; i++) {
                    x->audio_buffer[x->buffer_head] = audio[i];
                    x->buffer_head = (x->buffer_head + 1) % x->buffer_size;
                }
                critical_exit(x->lock);
            }
            if (buf) sysmem_freeptr(buf);
        } else if (len_prefix < 0) {
            // JSON status
            int bytes_to_read = -len_prefix;
            int actual_read_cap = (bytes_to_read > 8192) ? 8192 : bytes_to_read;

            char *buf = (char *)sysmem_newptr(actual_read_cap + 1);
            int total_read = 0;
            while (total_read < bytes_to_read) {
                int n_expected = (bytes_to_read - total_read);
                if (total_read < actual_read_cap) {
                    int to_recv = actual_read_cap - total_read;
                    if (to_recv > n_expected) to_recv = n_expected;
                    int n = recv(s, buf + total_read, to_recv, 0);
                    if (n <= 0) break;
                    total_read += n;
                } else {
                    char drain[1024];
                    int to_recv = (n_expected > 1024) ? 1024 : n_expected;
                    int n = recv(s, drain, to_recv, 0);
                    if (n <= 0) break;
                    total_read += n;
                }
            }
            if (total_read == bytes_to_read && buf) {
                buf[actual_read_cap] = '\0';
                magenta_log(x, "server status: %s", buf);
            }
            if (buf) sysmem_freeptr(buf);
        }
    }

    closesocket(s);
    WSACleanup();
    critical_enter(x->lock);
    x->connected = 0;
    x->sock = INVALID_SOCKET;
    critical_exit(x->lock);
    return NULL;
}

void magenta_assist(t_magenta *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "MIDI messages and Control");
    } else {
        switch (a) {
            case 0: sprintf(s, "Audio Output (L)"); break;
            case 1: sprintf(s, "Audio Output (R)"); break;
            case 2: sprintf(s, "Logging and Status"); break;
        }
    }
}

void magenta_dsp64(t_magenta *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags) {
    x->sr = samplerate;
    dsp_add64(dsp64, (t_object *)x, (t_perfroutine64)magenta_perform64, 0, NULL);
}

void magenta_perform64(t_magenta *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam) {
    double *out_l = outs[0];
    double *out_r = outs[1];

    if (critical_tryenter(x->lock) == MAX_ERR_NONE) {
        long available = (x->buffer_head - x->buffer_tail + x->buffer_size) % x->buffer_size;
        for (int i = 0; i < sampleframes; i++) {
            if (available >= 2) {
                out_l[i] = (double)x->audio_buffer[x->buffer_tail];
                x->buffer_tail = (x->buffer_tail + 1) % x->buffer_size;
                out_r[i] = (double)x->audio_buffer[x->buffer_tail];
                x->buffer_tail = (x->buffer_tail + 1) % x->buffer_size;
                available -= 2;
            } else {
                out_l[i] = 0.0;
                out_r[i] = 0.0;
            }
        }
        critical_exit(x->lock);
    } else {
        for (int i = 0; i < sampleframes; i++) {
            out_l[i] = 0.0;
            out_r[i] = 0.0;
        }
    }
}
