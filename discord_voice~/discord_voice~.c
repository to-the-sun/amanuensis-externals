#include "ext.h"
#include "ext_obex.h"
#include "ext_critical.h"
#include "z_dsp.h"
#include "ext_systhread.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string.h>
#include <stdio.h>

/**
 * @file discord_voice~.c
 * @brief Max external for sending audio to Discord voice channels.
 *
 * This object is a skeleton for implementing Discord's voice protocol within Max.
 */

typedef struct _discord_voice {
    t_pxobject d_obj;
    t_systhread thread;
    t_critical lock;
    int terminate;
    void *log_outlet;
} t_discord_voice;

void *discord_voice_new(t_symbol *s, long argc, t_atom *argv);
void discord_voice_free(t_discord_voice *x);
void discord_voice_assist(t_discord_voice *x, void *b, long m, long a, char *s);
void discord_voice_dsp64(t_discord_voice *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
void discord_voice_perform64(t_discord_voice *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);

static t_class *discord_voice_class;

void ext_main(void *r) {
    t_class *c = class_new("discord_voice~", (method)discord_voice_new, (method)discord_voice_free, sizeof(t_discord_voice), 0L, A_GIMME, 0);

    class_addmethod(c, (method)discord_voice_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)discord_voice_assist, "assist", A_CANT, 0);

    class_dspinit(c);
    class_register(CLASS_BOX, c);
    discord_voice_class = c;
}

void *discord_voice_new(t_symbol *s, long argc, t_atom *argv) {
    t_discord_voice *x = (t_discord_voice *)object_alloc(discord_voice_class);

    if (x) {
        dsp_setup((t_pxobject *)x, 2); // 2 signal inlets (L/R)
        x->log_outlet = outlet_new((t_object *)x, NULL);

        critical_new(&x->lock);
        x->terminate = 0;

        object_post((t_object *)x, "discord_voice~: initialized");
    }
    return x;
}

void discord_voice_free(t_discord_voice *x) {
    dsp_free((t_pxobject *)x);
    critical_free(x->lock);
}

void discord_voice_assist(t_discord_voice *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        switch (a) {
            case 0: sprintf(s, "Inlet 1 (signal): Left audio input / messages"); break;
            case 1: sprintf(s, "Inlet 2 (signal): Right audio input"); break;
        }
    } else {
        sprintf(s, "Outlet 1 (anything): Logging and status");
    }
}

void discord_voice_dsp64(t_discord_voice *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags) {
    dsp_add64(dsp64, (t_object *)x, (t_perfroutine64)discord_voice_perform64, 0, NULL);
}

void discord_voice_perform64(t_discord_voice *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam) {
    // Collect audio for processing/encoding in worker thread
    // For now, this is just a pass-through/no-op placeholder
}
