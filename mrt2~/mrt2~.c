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
 * @file mrt2~.c
 * @brief Max external for MRT2 (Magenta RealTime 2) synthesis bridge.
 */

#define MRT2_RING_BUFFER_SIZE 384000 // ~4 seconds of stereo at 48kHz (float32)
#define MRT2_MAX_CMD_LEN 1024

typedef enum {
    MRT2_CMD_NONE,
    MRT2_CMD_PROMPT,
    MRT2_CMD_MIDI,
    MRT2_CMD_MODEL
} t_mrt2_cmd_type;

typedef struct _mrt2_cmd {
    t_mrt2_cmd_type type;
    char data[MRT2_MAX_CMD_LEN];
    struct _mrt2_cmd *next;
} t_mrt2_cmd;

typedef struct _mrt2 {
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
    t_mrt2_cmd *cmd_head;
    t_mrt2_cmd *cmd_tail;

    double sr;
} t_mrt2;

void *mrt2_new(t_symbol *s, long argc, t_atom *argv);
void mrt2_free(t_mrt2 *x);
void mrt2_assist(t_mrt2 *x, void *b, long m, long a, char *s);
void mrt2_connect(t_mrt2 *x);
void mrt2_disconnect(t_mrt2 *x);
void mrt2_prompt(t_mrt2 *x, t_symbol *s);
void mrt2_list(t_mrt2 *x, t_symbol *s, long argc, t_atom *argv);
void *mrt2_thread_proc(t_mrt2 *x);
void mrt2_dsp64(t_mrt2 *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
void mrt2_perform64(t_mrt2 *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);
void mrt2_log(t_mrt2 *x, const char *fmt, ...);
void mrt2_push_cmd(t_mrt2 *x, t_mrt2_cmd_type type, const char *data);

static t_class *mrt2_class;

void mrt2_log(t_mrt2 *x, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vcommon_log(x->log_outlet, x->log, "mrt2~", fmt, args);
    va_end(args);
}

void ext_main(void *r) {
    common_symbols_init();
    t_class *c = class_new("mrt2~", (method)mrt2_new, (method)mrt2_free, sizeof(t_mrt2), 0L, A_GIMME, 0);

    class_addmethod(c, (method)mrt2_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)mrt2_assist, "assist", A_CANT, 0);
    class_addmethod(c, (method)mrt2_connect, "connect", 0);
    class_addmethod(c, (method)mrt2_disconnect, "disconnect", 0);
    class_addmethod(c, (method)mrt2_prompt, "prompt", A_SYM, 0);
    class_addmethod(c, (method)mrt2_list, "list", A_GIMME, 0);

    CLASS_ATTR_SYM(c, "address", 0, t_mrt2, address);
    CLASS_ATTR_DEFAULT(c, "address", 0, "127.0.0.1");

    CLASS_ATTR_LONG(c, "port", 0, t_mrt2, port);
    CLASS_ATTR_DEFAULT_SAVE(c, "port", 0, "9998");

    CLASS_ATTR_SYM(c, "model", 0, t_mrt2, model);
    CLASS_ATTR_DEFAULT(c, "model", 0, "mrt2_small");

    CLASS_ATTR_LONG(c, "log", 0, t_mrt2, log);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "onoff", "Enable Logging");
    CLASS_ATTR_DEFAULT(c, "log", 0, "0");

    class_dspinit(c);
    class_register(CLASS_BOX, c);
    mrt2_class = c;
}

void *mrt2_new(t_symbol *s, long argc, t_atom *argv) {
    t_mrt2 *x = (t_mrt2 *)object_alloc(mrt2_class);

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

        x->buffer_size = MRT2_RING_BUFFER_SIZE;
        x->audio_buffer = (float *)sysmem_newptr(x->buffer_size * sizeof(float));
        x->buffer_head = 0;
        x->buffer_tail = 0;

        x->cmd_head = NULL;
        x->cmd_tail = NULL;

        attr_args_process(x, argc, argv);

        // Max outlets are created right-to-left
        x->log_outlet = outlet_new((t_object *)x, NULL); // Index 2
        outlet_new((t_object *)x, "signal"); // Index 1 (Right)
        outlet_new((t_object *)x, "signal"); // Index 0 (Left)

        dsp_setup((t_pxobject *)x, 1);

        critical_new(&x->lock);
        mrt2_log(x, "initialized");
    }
    return x;
}

void mrt2_free(t_mrt2 *x) {
    dsp_free((t_pxobject *)x);
    mrt2_disconnect(x);

    if (x->audio_buffer) sysmem_freeptr(x->audio_buffer);

    critical_enter(x->lock);
    t_mrt2_cmd *cmd = x->cmd_head;
    while (cmd) {
        t_mrt2_cmd *next = cmd->next;
        sysmem_freeptr(cmd);
        cmd = next;
    }
    critical_exit(x->lock);

    critical_free(x->lock);
}

void mrt2_push_cmd(t_mrt2 *x, t_mrt2_cmd_type type, const char *data) {
    t_mrt2_cmd *cmd = (t_mrt2_cmd *)sysmem_newptr(sizeof(t_mrt2_cmd));
    if (cmd) {
        cmd->type = type;
        if (data) {
            strncpy(cmd->data, data, MRT2_MAX_CMD_LEN - 1);
            cmd->data[MRT2_MAX_CMD_LEN - 1] = '\0';
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

void mrt2_connect(t_mrt2 *x) {
    if (x->thread) {
        object_warn((t_object *)x, "already connected or connecting");
        return;
    }
    x->terminate = 0;
    systhread_create((method)mrt2_thread_proc, x, 0, 0, 0, &x->thread);
}

void mrt2_disconnect(t_mrt2 *x) {
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
    mrt2_log(x, "disconnected");
}

void mrt2_prompt(t_mrt2 *x, t_symbol *s) {
    x->prompt = s;
    mrt2_push_cmd(x, MRT2_CMD_PROMPT, s->s_name);
}

void mrt2_list(t_mrt2 *x, t_symbol *s, long argc, t_atom *argv) {
    if (argc >= 2) {
        char buf[64];
        snprintf(buf, 64, "%ld,%ld,%ld", atom_getlong(argv), atom_getlong(argv+1), argc > 2 ? atom_getlong(argv+2) : 0);
        mrt2_push_cmd(x, MRT2_CMD_MIDI, buf);
    }
}

void *mrt2_thread_proc(t_mrt2 *x) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        mrt2_log(x, "WSAStartup failed");
        return NULL;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((u_short)x->port);
    inet_pton(AF_INET, x->address->s_name, &server_addr.sin_addr);

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        mrt2_log(x, "socket creation failed");
        WSACleanup();
        return NULL;
    }

    if (connect(s, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        mrt2_log(x, "connection to %s:%ld failed", x->address->s_name, x->port);
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

    mrt2_log(x, "connected to bridge at %s:%ld", x->address->s_name, x->port);

    // Send initial model and prompt
    char init_model[MRT2_MAX_CMD_LEN];
    snprintf(init_model, MRT2_MAX_CMD_LEN, "{\"event\":\"model\",\"name\":\"%s\"}\n", x->model->s_name);
    send(s, init_model, (int)strlen(init_model), 0);

    if (x->prompt != _sym_nothing) {
        char init_prompt[MRT2_MAX_CMD_LEN];
        snprintf(init_prompt, MRT2_MAX_CMD_LEN, "{\"event\":\"prompt\",\"text\":\"%s\"}\n", x->prompt->s_name);
        send(s, init_prompt, (int)strlen(init_prompt), 0);
    }

    while (!x->terminate) {
        // 1. Send pending commands
        critical_enter(x->lock);
        t_mrt2_cmd *cmd = x->cmd_head;
        x->cmd_head = NULL;
        x->cmd_tail = NULL;
        critical_exit(x->lock);

        while (cmd) {
            char json[MRT2_MAX_CMD_LEN + 128];
            if (cmd->type == MRT2_CMD_PROMPT) {
                snprintf(json, sizeof(json), "{\"event\":\"prompt\",\"text\":\"%s\"}\n", cmd->data);
            } else if (cmd->type == MRT2_CMD_MIDI) {
                snprintf(json, sizeof(json), "{\"event\":\"midi\",\"data\":[%s]}\n", cmd->data);
            } else if (cmd->type == MRT2_CMD_MODEL) {
                snprintf(json, sizeof(json), "{\"event\":\"model\",\"name\":\"%s\"}\n", cmd->data);
            }
            send(s, json, (int)strlen(json), 0);
            t_mrt2_cmd *next = cmd->next;
            sysmem_freeptr(cmd);
            cmd = next;
        }

        // 2. Receive data [int32 length][data...]
        int32_t len_prefix;
        int r = recv(s, (char *)&len_prefix, 4, 0);
        if (r <= 0) break;

        if (len_prefix > 0) {
            // Audio data
            int bytes_to_read = len_prefix;
            char *buf = (char *)sysmem_newptr(bytes_to_read);
            int total_read = 0;
            while (total_read < bytes_to_read) {
                int n = recv(s, buf + total_read, bytes_to_read - total_read, 0);
                if (n <= 0) break;
                total_read += n;
            }

            if (total_read == bytes_to_read) {
                float *audio = (float *)buf;
                int samples = total_read / sizeof(float);
                critical_enter(x->lock);
                for (int i = 0; i < samples; i++) {
                    x->audio_buffer[x->buffer_head] = audio[i];
                    x->buffer_head = (x->buffer_head + 1) % x->buffer_size;
                }
                critical_exit(x->lock);
            }
            sysmem_freeptr(buf);
        } else if (len_prefix < 0) {
            // JSON status
            int bytes_to_read = -len_prefix;
            char *buf = (char *)sysmem_newptr(bytes_to_read + 1);
            int total_read = 0;
            while (total_read < bytes_to_read) {
                int n = recv(s, buf + total_read, bytes_to_read - total_read, 0);
                if (n <= 0) break;
                total_read += n;
            }
            if (total_read == bytes_to_read) {
                buf[total_read] = '\0';
                mrt2_log(x, "status: %s", buf);
            }
            sysmem_freeptr(buf);
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

void mrt2_assist(t_mrt2 *x, void *b, long m, long a, char *s) {
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

void mrt2_dsp64(t_mrt2 *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags) {
    x->sr = samplerate;
    dsp_add64(dsp64, (t_object *)x, (t_perfroutine64)mrt2_perform64, 0, NULL);
}

void mrt2_perform64(t_mrt2 *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam) {
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
