#include "ext.h"
#include "ext_obex.h"
#include "ext_critical.h"
#include "z_dsp.h"
#include <string.h>
#include <stdlib.h>

typedef struct _mc_block {
    t_pxobject obj;
    void *proxy;
    long proxy_id;

    long input_num_chans;

    t_critical lock;
    long *blocked_channels;
    long num_blocked;
    long allocated_blocked;
} t_mc_block;

void *mc_block_new(t_symbol *s, long argc, t_atom *argv);
void mc_block_free(t_mc_block *x);
void mc_block_assist(t_mc_block *x, void *b, long m, long a, char *s);
void mc_block_list(t_mc_block *x, t_symbol *s, long argc, t_atom *argv);
void mc_block_int(t_mc_block *x, long n);
void mc_block_float(t_mc_block *x, double f);
void mc_block_anything(t_mc_block *x, t_symbol *s, long argc, t_atom *argv);
void mc_block_clear(t_mc_block *x);
long mc_block_inputchanged(t_mc_block *x, long index, long count);
long mc_block_multichanneloutputs(t_mc_block *x, long outlet_index);
void mc_block_dsp64(t_mc_block *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
void mc_block_perform64(t_mc_block *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);

static t_class *mc_block_class = NULL;

void ext_main(void *r) {
    common_symbols_init();

    t_class *c = class_new("mc.block~", (method)mc_block_new, (method)mc_block_free, sizeof(t_mc_block), 0L, A_GIMME, 0);

    class_addmethod(c, (method)mc_block_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)mc_block_assist, "assist", A_CANT, 0);
    class_addmethod(c, (method)mc_block_list, "list", A_GIMME, 0);
    class_addmethod(c, (method)mc_block_int, "int", A_LONG, 0);
    class_addmethod(c, (method)mc_block_float, "float", A_FLOAT, 0);
    class_addmethod(c, (method)mc_block_anything, "anything", A_GIMME, 0);
    class_addmethod(c, (method)mc_block_clear, "clear", 0);
    class_addmethod(c, (method)mc_block_inputchanged, "inputchanged", A_CANT, 0);
    class_addmethod(c, (method)mc_block_multichanneloutputs, "multichanneloutputs", A_CANT, 0);

    class_dspinit(c);
    class_register(CLASS_BOX, c);
    mc_block_class = c;
}

static void mc_block_update_blocked_channels(t_mc_block *x, t_symbol *s, long argc, t_atom *argv) {
    critical_enter(x->lock);
    x->num_blocked = 0;

    long total_count = argc;
    if (s && s != _sym_nothing && s != gensym("list") && s != gensym("anything")) {
        total_count++;
    }

    if (total_count > 0) {
        if (x->allocated_blocked < total_count) {
            long *new_arr = (long *)realloc(x->blocked_channels, sizeof(long) * total_count);
            if (new_arr) {
                x->blocked_channels = new_arr;
                x->allocated_blocked = total_count;
            }
        }

        if (x->blocked_channels && x->allocated_blocked >= total_count) {
            if (s && s != _sym_nothing && s != gensym("list") && s != gensym("anything")) {
                char *endptr = NULL;
                long chan = strtol(s->s_name, &endptr, 10);
                if (endptr && *endptr == '\0' && chan > 0) {
                    x->blocked_channels[x->num_blocked++] = chan;
                }
            }

            for (long i = 0; i < argc; i++) {
                long chan = 0;
                if (atom_gettype(argv + i) == A_LONG) {
                    chan = atom_getlong(argv + i);
                } else if (atom_gettype(argv + i) == A_FLOAT) {
                    chan = (long)atom_getfloat(argv + i);
                } else if (atom_gettype(argv + i) == A_SYM) {
                    t_symbol *sym = atom_getsym(argv + i);
                    if (sym) {
                        char *endptr = NULL;
                        chan = strtol(sym->s_name, &endptr, 10);
                        if (!endptr || *endptr != '\0') {
                            chan = 0;
                        }
                    }
                }
                if (chan > 0) {
                    x->blocked_channels[x->num_blocked++] = chan;
                }
            }
        }
    }
    critical_exit(x->lock);
}

void *mc_block_new(t_symbol *s, long argc, t_atom *argv) {
    t_mc_block *x = (t_mc_block *)object_alloc(mc_block_class);

    if (x) {
        dsp_setup((t_pxobject *)x, 1);
        x->obj.z_misc |= Z_MC_INLETS;

        x->proxy = proxy_new((t_object *)x, 1, &x->proxy_id);

        outlet_new((t_object *)x, "multichannelsignal");

        x->input_num_chans = 1;

        critical_new(&x->lock);
        x->blocked_channels = NULL;
        x->num_blocked = 0;
        x->allocated_blocked = 0;

        if (argc > 0) {
            mc_block_update_blocked_channels(x, NULL, argc, argv);
        }
    }
    return x;
}

void mc_block_free(t_mc_block *x) {
    dsp_free((t_pxobject *)x);

    if (x->proxy) {
        object_free(x->proxy);
    }

    if (x->blocked_channels) {
        free(x->blocked_channels);
    }

    critical_free(x->lock);
}

void mc_block_list(t_mc_block *x, t_symbol *s, long argc, t_atom *argv) {
    long inlet = proxy_getinlet((t_object *)x);
    if (inlet == 1) {
        mc_block_update_blocked_channels(x, s, argc, argv);
    }
}

void mc_block_int(t_mc_block *x, long n) {
    long inlet = proxy_getinlet((t_object *)x);
    if (inlet == 1) {
        t_atom a;
        atom_setlong(&a, n);
        mc_block_update_blocked_channels(x, NULL, 1, &a);
    }
}

void mc_block_float(t_mc_block *x, double f) {
    long inlet = proxy_getinlet((t_object *)x);
    if (inlet == 1) {
        t_atom a;
        atom_setlong(&a, (long)f);
        mc_block_update_blocked_channels(x, NULL, 1, &a);
    }
}

void mc_block_anything(t_mc_block *x, t_symbol *s, long argc, t_atom *argv) {
    long inlet = proxy_getinlet((t_object *)x);
    if (inlet == 1) {
        mc_block_update_blocked_channels(x, s, argc, argv);
    }
}

void mc_block_clear(t_mc_block *x) {
    critical_enter(x->lock);
    x->num_blocked = 0;
    critical_exit(x->lock);
}

long mc_block_inputchanged(t_mc_block *x, long index, long count) {
    if (index == 0) {
        if (count != x->input_num_chans) {
            x->input_num_chans = count;
            return true;
        }
    }
    return false;
}

long mc_block_multichanneloutputs(t_mc_block *x, long outlet_index) {
    if (outlet_index == 0) {
        return x->input_num_chans > 0 ? x->input_num_chans : 1;
    }
    return 1;
}

void mc_block_assist(t_mc_block *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        switch (a) {
            case 0: sprintf(s, "(multichannelsignal) Audio Input"); break;
            case 1: sprintf(s, "(list/int) Channel Numbers to Block (1-based)"); break;
        }
    } else {
        switch (a) {
            case 0: sprintf(s, "(multichannelsignal) Mirrored Audio Output"); break;
        }
    }
}

void mc_block_dsp64(t_mc_block *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags) {
    long num_chans = (long)(intptr_t)object_method(dsp64, gensym("getnuminputchannels"), x, 0);
    if (num_chans < 1) {
        num_chans = 1;
    }
    x->input_num_chans = num_chans;
    object_method(dsp64, gensym("setnumoutputchannels"), x, 0, num_chans);
    dsp_add64(dsp64, (t_object *)x, (t_perfroutine64)mc_block_perform64, 0, NULL);
}

void mc_block_perform64(t_mc_block *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam) {
    long n_chans = numins < numouts ? numins : numouts;

    critical_enter(x->lock);

    for (long c = 0; c < n_chans; c++) {
        long chan_num = c + 1;
        int is_blocked = 0;

        for (long b = 0; b < x->num_blocked; b++) {
            if (x->blocked_channels[b] == chan_num) {
                is_blocked = 1;
                break;
            }
        }

        double *in = ins[c];
        double *out = outs[c];

        if (is_blocked) {
            memset(out, 0, sampleframes * sizeof(double));
        } else {
            if (in != out) {
                memcpy(out, in, sampleframes * sizeof(double));
            }
        }
    }

    for (long c = n_chans; c < numouts; c++) {
        memset(outs[c], 0, sampleframes * sizeof(double));
    }

    critical_exit(x->lock);
}
