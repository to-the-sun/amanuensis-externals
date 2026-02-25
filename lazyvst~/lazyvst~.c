#include <windows.h>
#include "ext.h"
#include "ext_obex.h"
#include "z_dsp.h"
#include "../shared/logging.h"
#include <stdarg.h>
#include <stdint.h>

#ifndef VSTCALLBACK
#define VSTCALLBACK __cdecl
#endif

// VST 2.4 Minimal Structures
typedef intptr_t VstIntPtr;
typedef int32_t VstInt32;

struct AEffect;

typedef VstIntPtr (VSTCALLBACK *audioMasterCallback) (struct AEffect* effect, VstInt32 opcode, VstInt32 index, VstIntPtr value, void* ptr, float opt);
typedef VstIntPtr (VSTCALLBACK *AEffectDispatcherProc) (struct AEffect* effect, VstInt32 opcode, VstInt32 index, VstIntPtr value, void* ptr, float opt);
typedef void (VSTCALLBACK *AEffectProcessProc) (struct AEffect* effect, float** inputs, float** outputs, VstInt32 sampleFrames);
typedef void (VSTCALLBACK *AEffectSetParameterProc) (struct AEffect* effect, VstInt32 index, float parameter);
typedef float (VSTCALLBACK *AEffectGetParameterProc) (struct AEffect* effect, VstInt32 index);

typedef struct AEffect
{
	VstInt32 magic;
	AEffectDispatcherProc dispatcher;
	AEffectProcessProc process;
	AEffectSetParameterProc setParameter;
	AEffectGetParameterProc getParameter;
	VstInt32 numPrograms;
	VstInt32 numParams;
	VstInt32 numInputs;
	VstInt32 numOutputs;
	VstInt32 flags;
	VstIntPtr resvd1;
	VstIntPtr resvd2;
	VstInt32 initialDelay;
	VstInt32 realQualities;
	VstInt32 offQualities;
	float ioRatio;
	void* object;
	void* user;
	VstInt32 uniqueID;
	VstInt32 version;
	void* processReplacing;
	char future[56];
} AEffect;


typedef AEffect* (VSTCALLBACK *PVSTPLUGINMAIN) (audioMasterCallback host);

// audioMaster Opcodes
#define audioMasterVersion 1

// AEffect Opcodes
#define effOpen 0
#define effClose 1

VstIntPtr VSTCALLBACK audioMaster(struct AEffect* effect, VstInt32 opcode, VstInt32 index, VstIntPtr value, void* ptr, float opt) {
    switch (opcode) {
        case audioMasterVersion:
            return 2400;
        default:
            return 0;
    }
}

typedef struct _lazyvst {
    t_pxobject x_obj;
    long log;
    void *log_outlet;
    HINSTANCE hLib;
    AEffect *effect;
    long inputs;  // Attribute (pins on box)
    long outputs; // Attribute (pins on box)
    int v_numInputs;  // Detected from VST
    int v_numOutputs; // Detected from VST
    t_symbol *last_path;
} t_lazyvst;

void *lazyvst_new(t_symbol *s, long argc, t_atom *argv);
void lazyvst_free(t_lazyvst *x);
void lazyvst_assist(t_lazyvst *x, void *b, long m, long a, char *s);
void lazyvst_plug(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv);
void lazyvst_repatch(t_lazyvst *x);
void lazyvst_dsp64(t_lazyvst *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
void lazyvst_perform64(t_lazyvst *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);

static t_class *lazyvst_class;

// Helper function to send verbose log messages with prefix
void lazyvst_log(t_lazyvst *x, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vcommon_log(x->log_outlet, x->log, "lazyvst~", fmt, args);
    va_end(args);
}

void ext_main(void *r) {
    t_class *c = class_new("lazyvst~", (method)lazyvst_new, (method)lazyvst_free, sizeof(t_lazyvst), 0L, A_GIMME, 0);

    class_addmethod(c, (method)lazyvst_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)lazyvst_assist, "assist", A_CANT, 0);
    class_addmethod(c, (method)lazyvst_plug, "plug", A_GIMME, 0);
    class_addmethod(c, (method)lazyvst_repatch, "repatch", 0);

    CLASS_ATTR_LONG(c, "log", 0, t_lazyvst, log);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "onoff", "Enable Logging");
    CLASS_ATTR_DEFAULT(c, "log", 0, "0");

    CLASS_ATTR_LONG(c, "inputs", 0, t_lazyvst, inputs);
    CLASS_ATTR_LABEL(c, "inputs", 0, "Number of Inputs");
    CLASS_ATTR_DEFAULT(c, "inputs", 0, "16");

    CLASS_ATTR_LONG(c, "outputs", 0, t_lazyvst, outputs);
    CLASS_ATTR_LABEL(c, "outputs", 0, "Number of Outputs");
    CLASS_ATTR_DEFAULT(c, "outputs", 0, "16");

    class_dspinit(c);
    class_register(CLASS_BOX, c);
    lazyvst_class = c;
}

void *lazyvst_new(t_symbol *s, long argc, t_atom *argv) {
    t_lazyvst *x = (t_lazyvst *)object_alloc(lazyvst_class);

    if (x) {
        x->log = 0;
        x->hLib = NULL;
        x->effect = NULL;
        x->v_numInputs = 0;
        x->v_numOutputs = 0;
        x->inputs = 16;
        x->outputs = 16;
        x->last_path = _sym_nothing;

        attr_args_process(x, argc, argv);

        dsp_setup((t_pxobject *)x, x->inputs);

        // Outlets (right to left)
        x->log_outlet = outlet_new((t_object *)x, NULL);
        for (int i = (int)x->outputs - 1; i >= 0; i--) {
            outlet_new((t_object *)x, "signal");
        }
    }
    return x;
}

void lazyvst_free(t_lazyvst *x) {
    dsp_free((t_pxobject *)x);
    if (x->effect && x->effect->dispatcher) {
        x->effect->dispatcher((struct AEffect*)x->effect, effClose, 0, 0, NULL, 0.0f);
    }
    if (x->hLib) {
        FreeLibrary(x->hLib);
        x->hLib = NULL;
    }
}

void lazyvst_plug(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv) {
    t_symbol *path = _sym_nothing;

    // Prioritize second argument if it exists to support [plug something "C:/path"]
    if (argc > 1 && atom_gettype(argv + 1) == A_SYM) {
        path = atom_getsym(argv + 1);
    } else if (argc > 0 && atom_gettype(argv) == A_SYM) {
        path = atom_getsym(argv);
    } else {
        object_error((t_object *)x, "plug: missing VST path");
        return;
    }

    if (x->hLib) {
        if (x->effect && x->effect->dispatcher) {
            x->effect->dispatcher((struct AEffect*)x->effect, effClose, 0, 0, NULL, 0.0f);
        }
        FreeLibrary(x->hLib);
        x->hLib = NULL;
        x->effect = NULL;
    }

    x->last_path = path;
    lazyvst_log(x, "plug: attempting to load %s", path->s_name);
    x->hLib = LoadLibraryA(path->s_name);

    if (x->hLib) {
        PVSTPLUGINMAIN pMain = (PVSTPLUGINMAIN)GetProcAddress(x->hLib, "VSTPluginMain");
        if (!pMain) pMain = (PVSTPLUGINMAIN)GetProcAddress(x->hLib, "main");

        if (pMain) {
            x->effect = pMain(audioMaster);
            if (x->effect && x->effect->magic == 0x56737450) { // 'VstP'
                if (x->effect->dispatcher) {
                    x->effect->dispatcher((struct AEffect*)x->effect, effOpen, 0, 0, NULL, 0.0f);
                }
                x->v_numInputs = x->effect->numInputs;
                x->v_numOutputs = x->effect->numOutputs;
                lazyvst_log(x, "plug: successfully loaded %s (%d in, %d out)", path->s_name, x->v_numInputs, x->v_numOutputs);

                if (x->v_numInputs != (int)x->inputs || x->v_numOutputs != (int)x->outputs) {
                    object_warn((t_object *)x, "I/O count mismatch: VST has %d/%d, object has %ld/%ld. Send 'repatch' to update.", x->v_numInputs, x->v_numOutputs, x->inputs, x->outputs);
                }
            } else {
                object_error((t_object *)x, "plug: library %s is not a valid VST plugin", path->s_name);
                FreeLibrary(x->hLib);
                x->hLib = NULL;
                x->effect = NULL;
            }
        } else {
            object_error((t_object *)x, "plug: could not find entry point in %s", path->s_name);
            FreeLibrary(x->hLib);
            x->hLib = NULL;
        }
    } else {
        object_error((t_object *)x, "plug: failed to load %s (Error code: %lu)", path->s_name, GetLastError());
    }
}

void lazyvst_repatch(t_lazyvst *x) {
    if (x->v_numInputs == 0 && x->v_numOutputs == 0) {
        object_error((t_object *)x, "repatch: no VST loaded or I/O counts unknown");
        return;
    }
    object_post((t_object *)x, "I/O ports of a Max object cannot be changed dynamically at runtime.");
    object_post((t_object *)x, "To update ports, recreate the object as [lazyvst~ @inputs %d @outputs %d]", x->v_numInputs, x->v_numOutputs);
}

void lazyvst_dsp64(t_lazyvst *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags) {
    dsp_add64(dsp64, (t_object *)x, (t_perfroutine64)lazyvst_perform64, 0, NULL);
}

void lazyvst_perform64(t_lazyvst *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam) {
    for (int chan = 0; chan < numouts; chan++) {
        for (int i = 0; i < sampleframes; i++) {
            outs[chan][i] = 0.0;
        }
    }
}

void lazyvst_assist(t_lazyvst *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        if (a == 0) sprintf(s, "(signal/symbol) Input 1 / Messages");
        else sprintf(s, "(signal) Input %ld", a + 1);
    } else {
        if (a == x->outputs) {
            sprintf(s, "Logging Outlet");
        } else if (a < x->outputs) {
            sprintf(s, "(signal) Output %ld", a + 1);
        }
    }
}
