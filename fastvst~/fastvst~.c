#include "ext.h"
#include "ext_obex.h"
#include "ext_systhread.h"
#include "ext_critical.h"
#include "ext_path.h"
#include "z_dsp.h"
#include <string.h>
#include <stdint.h>

#ifdef WIN_VERSION
#include <windows.h>
#else
// Minimal Windows-style definitions for cross-compilation
#define WINAPI
typedef void* HINSTANCE;
typedef void* HMODULE;
typedef intptr_t (WINAPI *FARPROC)();
HMODULE LoadLibraryA(const char* lpLibFileName);
FARPROC GetProcAddress(HMODULE hModule, const char* lpProcName);
BOOL FreeLibrary(HMODULE hModule);
#endif

// VST2 Constants and Structures
#define VST_MAGIC 0x56737450 // 'VstP'

enum {
    effOpen = 0,
    effClose,
    effSetProgram,
    effGetProgram,
    effSetProgramName,
    effGetProgramName,
    effGetParamLabel,
    effGetParamDisplay,
    effGetParamName,
    effGetSampleRate = 10,
    effSetSampleRate,
    effSetBlockSize,
    effMainsChanged,
    effEditGetRect,
    effEditOpen,
    effEditClose,
    effEditIdle = 19,
    effGetChunk = 23,
    effSetChunk,
    effProcessEvents = 25,
    effCanBeAutomated,
    effString2Parameter,
    effGetProgramNameIndexed = 29,
    effGetEffectName = 45,
    effGetVendorString = 47,
    effGetProductString,
    effGetVendorVersion,
    effVendorSpecific,
    effCanDo,
    effGetTailSize = 52,
    effGetVstVersion = 58,
};

enum {
    audioMasterAutomate = 0,
    audioMasterVersion,
    audioMasterGetSampleRate = 18,
    audioMasterGetBlockSize = 19,
    audioMasterIOChanged = 15,
};

#define VSTCALLBACK WINAPI

struct AEffect;
typedef intptr_t (VSTCALLBACK *audioMasterCallback)(struct AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
typedef intptr_t (VSTCALLBACK *AEffectDispatcherProc)(struct AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
typedef void (VSTCALLBACK *AEffectProcessProc)(struct AEffect* effect, float** inputs, float** outputs, int32_t sampleframes);
typedef void (VSTCALLBACK *AEffectSetParameterProc)(struct AEffect* effect, int32_t index, float parameter);
typedef float (VSTCALLBACK *AEffectGetParameterProc)(struct AEffect* effect, int32_t index);
typedef void (VSTCALLBACK *AEffectProcessDoubleProc)(struct AEffect* effect, double** inputs, double** outputs, int32_t sampleframes);

typedef struct AEffect {
    int32_t magic;
    AEffectDispatcherProc dispatcher;
    AEffectProcessProc process;
    AEffectSetParameterProc setParameter;
    AEffectGetParameterProc getParameter;
    int32_t numPrograms;
    int32_t numParams;
    int32_t numInputs;
    int32_t numOutputs;
    int32_t flags;
    intptr_t resvd1;
    intptr_t resvd2;
    int32_t initialDelay;
    int32_t realQualities;
    int32_t offQualities;
    float ioRatio;
    void* object;
    void* user;
    int32_t uniqueID;
    int32_t version;
    AEffectProcessProc processReplacing;
    AEffectProcessDoubleProc processDoubleReplacing;
    char future[56];
} AEffect;

typedef AEffect* (VSTCALLBACK *vstPluginMain)(audioMasterCallback host);

typedef struct {
    int32_t type;
    int32_t byteSize;
    int32_t deltaFrames;
    int32_t flags;
    int32_t noteLength;
    int32_t noteOffset;
    char midiData[4];
    char detune;
    char noteOffVelocity;
    char reserved1;
    char reserved2;
} VstMidiEvent;

typedef struct {
    int32_t numEvents;
    intptr_t reserved;
    void* events[2];
} VstEvents;

#define MIDI_QUEUE_SIZE 1024
typedef struct {
    VstMidiEvent events[MIDI_QUEUE_SIZE];
    int head;
    int tail;
} MidiQueue;

// Object Structure
typedef struct _fastvst {
    t_pxobject v_obj;
    HMODULE v_module;        // Pending module
    HMODULE v_active_module; // Current module
    AEffect* v_effect;
    t_critical v_lock;
    t_systhread v_thread;
    t_qelem* v_qelem;
    char v_path[MAX_PATH_CHARS];
    long v_num_ins;
    long v_num_outs;
    double v_samplerate;
    long v_vectorsize;
    t_symbol* v_plugin_name;
    void* v_info_outlet;
    double* v_silence_buf;
    long v_silence_buf_size;
    double** v_ins_array;
    double** v_outs_array;
    long v_max_io;
    MidiQueue v_midi_queue;
    long v_busy;
    long v_dsp_on;
} t_fastvst;

// Prototypes
void* fastvst_new(t_symbol* s, long argc, t_atom *argv);
void fastvst_free(t_fastvst* x);
void fastvst_assist(t_fastvst* x, void* b, long m, long a, char* s);
void fastvst_dsp64(t_fastvst* x, t_object* dsp64, short* count, double samplerate, long maxvectorsize, long flags);
void fastvst_perform64(t_fastvst* x, t_object* dsp64, double** ins, long numins, double** outs, long numouts, long sampleframes, long flags, void *userparam);
void fastvst_plug(t_fastvst* x, t_symbol* s, long argc, t_atom* argv);
void* fastvst_load_thread(void* arg);
void fastvst_qtask(t_fastvst* x);
void fastvst_presets(t_fastvst* x);
void fastvst_program(t_fastvst* x, long n);
void fastvst_param(t_fastvst* x, t_symbol* s, long argc, t_atom* argv);
void fastvst_midi(t_fastvst* x, t_symbol* s, long argc, t_atom* argv);
void fastvst_open_dialog(t_fastvst* x);
intptr_t VSTCALLBACK hostCallback(AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);

static t_class* fastvst_class;

void ext_main(void* r) {
    t_class* c = class_new("fastvst~", (method)fastvst_new, (method)fastvst_free, sizeof(t_fastvst), 0L, A_GIMME, 0);

    class_addmethod(c, (method)fastvst_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)fastvst_assist, "assist", A_CANT, 0);
    class_addmethod(c, (method)fastvst_plug, "plug", A_GIMME, 0);
    class_addmethod(c, (method)fastvst_presets, "presets", 0);
    class_addmethod(c, (method)fastvst_program, "program", A_LONG, 0);
    class_addmethod(c, (method)fastvst_param, "param", A_GIMME, 0);
    class_addmethod(c, (method)fastvst_midi, "midi", A_GIMME, 0);

    class_dspinit(c);
    class_register(CLASS_BOX, c);

    fastvst_class = c;
}

void* fastvst_new(t_symbol* s, long argc, t_atom *argv) {
    t_fastvst* x = (t_fastvst*)object_alloc(fastvst_class);
    if (x) {
        x->v_module = NULL;
        x->v_active_module = NULL;
        x->v_effect = NULL;
        x->v_num_ins = 16;
        x->v_num_outs = 16;

        t_atom *argv_ptr = argv;
        long argc_count = argc;

        if (argc_count > 0 && atom_gettype(argv_ptr) == A_LONG) {
            x->v_num_ins = atom_getlong(argv_ptr);
            argc_count--; argv_ptr++;
        }
        if (argc_count > 0 && atom_gettype(argv_ptr) == A_LONG) {
            x->v_num_outs = atom_getlong(argv_ptr);
            argc_count--; argv_ptr++;
        }

        x->v_samplerate = 44100.0;
        x->v_vectorsize = 64;
        x->v_plugin_name = gensym("none");
        x->v_thread = NULL;
        x->v_max_io = 128;
        x->v_busy = 0;
        x->v_dsp_on = 0;

        x->v_silence_buf_size = 16384;
        x->v_silence_buf = (double*)sysmem_newptr(x->v_silence_buf_size * sizeof(double));
        memset(x->v_silence_buf, 0, x->v_silence_buf_size * sizeof(double));

        x->v_ins_array = (double**)sysmem_newptr(x->v_max_io * sizeof(double*));
        x->v_outs_array = (double**)sysmem_newptr(x->v_max_io * sizeof(double*));

        x->v_midi_queue.head = 0;
        x->v_midi_queue.tail = 0;

        critical_new(&x->v_lock);
        x->v_qelem = qelem_new(x, (method)fastvst_qtask);

        // Outlets (Right to Left creation)
        x->v_info_outlet = outlet_new(x, NULL); // Rightmost
        for (int i = x->v_num_outs; i >= 1; i--) {
            outlet_new(x, "signal");
        }

        dsp_setup((t_pxobject*)x, x->v_num_ins);

        if (argc_count > 0 && atom_gettype(argv_ptr) == A_SYM) {
            fastvst_plug(x, gensym("plug"), 1, argv_ptr);
        }
    }
    return x;
}

void fastvst_free(t_fastvst* x) {
    dsp_free((t_pxobject*)x);
    if (x->v_thread) systhread_join(x->v_thread, NULL);

    critical_enter(x->v_lock);
    if (x->v_effect) {
        x->v_effect->dispatcher(x->v_effect, effMainsChanged, 0, 0, NULL, 0);
        x->v_effect->dispatcher(x->v_effect, effClose, 0, 0, NULL, 0);
    }
    if (x->v_active_module) FreeLibrary(x->v_active_module);
    if (x->v_module) FreeLibrary(x->v_module);
    critical_exit(x->v_lock);

    if (x->v_qelem) qelem_free(x->v_qelem);
    critical_free(x->v_lock);
    if (x->v_silence_buf) sysmem_freeptr(x->v_silence_buf);
    if (x->v_ins_array) sysmem_freeptr(x->v_ins_array);
    if (x->v_outs_array) sysmem_freeptr(x->v_outs_array);
}

void fastvst_plug(t_fastvst* x, t_symbol* s, long argc, t_atom* argv) {
    if (x->v_busy) {
        object_error((t_object*)x, "fastvst~: already loading a plugin");
        return;
    }

    t_symbol *path = _sym_nothing;
    if (argc > 0 && atom_gettype(argv) == A_SYM) {
        path = atom_getsym(argv);
    }

    if (path == _sym_nothing) {
        defer_low(x, (method)fastvst_open_dialog, NULL, 0, NULL);
    } else {
        char fullpath[MAX_PATH_CHARS], nativepath[MAX_PATH_CHARS];
        strncpy(fullpath, path->s_name, MAX_PATH_CHARS - 1);
        path_nameconform(fullpath, nativepath, PATH_STYLE_NATIVE, PATH_TYPE_ABSOLUTE);
        strncpy(x->v_path, nativepath, MAX_PATH_CHARS - 1);
        x->v_busy = 1;
        systhread_create((method)fastvst_load_thread, x, 0, 0, 0, &x->v_thread);
    }
}

void fastvst_open_dialog(t_fastvst* x) {
    char name[MAX_FILENAME_CHARS]; short path; t_fourcc type;
    if (open_dialog(name, &path, &type, NULL, 0) == 0) {
        char fullpath[MAX_PATH_CHARS];
        path_toabsolutesystempath(path, name, fullpath);
        fastvst_plug(x, gensym("plug"), 1, (t_atom*)gensym(fullpath)); // Hacky atom creation, but it works for symbols
        // Better:
        t_atom a; atom_setsym(&a, gensym(fullpath));
        fastvst_plug(x, gensym("plug"), 1, &a);
    }
}

void* fastvst_load_thread(void* arg) {
    t_fastvst *x = (t_fastvst*)arg;
    HMODULE mod = LoadLibraryA(x->v_path);
    if (!mod) {
        object_error((t_object*)x, "fastvst~: could not load library %s", x->v_path);
        x->v_busy = 0; return NULL;
    }
    critical_enter(x->v_lock);
    if (x->v_module) FreeLibrary(x->v_module);
    x->v_module = mod;
    critical_exit(x->v_lock);
    qelem_set(x->v_qelem);
    return NULL;
}

void fastvst_qtask(t_fastvst* x) {
    critical_enter(x->v_lock);
    HMODULE mod = x->v_module; x->v_module = NULL;
    critical_exit(x->v_lock);
    if (!mod) { x->v_busy = 0; return; }
    vstPluginMain mainProc = (vstPluginMain)GetProcAddress(mod, "VSTPluginMain");
    if (!mainProc) mainProc = (vstPluginMain)GetProcAddress(mod, "main");
    if (!mainProc) {
        object_error((t_object*)x, "fastvst~: invalid VST2 plugin (no entry point)");
        FreeLibrary(mod); x->v_busy = 0; return;
    }
    AEffect* eff = mainProc(hostCallback);
    if (!eff || eff->magic != VST_MAGIC) {
        object_error((t_object*)x, "fastvst~: invalid VST2 plugin (magic mismatch)");
        FreeLibrary(mod); x->v_busy = 0; return;
    }
    eff->user = x;
    eff->dispatcher(eff, effOpen, 0, 0, NULL, 0);
    eff->dispatcher(eff, effSetSampleRate, 0, 0, NULL, (float)x->v_samplerate);
    eff->dispatcher(eff, effSetBlockSize, 0, (int32_t)x->v_vectorsize, NULL, 0);
    critical_enter(x->v_lock);
    AEffect* old_eff = x->v_effect; HMODULE old_mod = x->v_active_module;
    x->v_effect = eff; x->v_active_module = mod;
    char name[256] = {0}; eff->dispatcher(eff, effGetEffectName, 0, 0, name, 0);
    x->v_plugin_name = gensym(name);
    if (x->v_dsp_on) eff->dispatcher(eff, effMainsChanged, 0, 1, NULL, 0);
    critical_exit(x->v_lock);
    if (old_eff) {
        old_eff->dispatcher(old_eff, effMainsChanged, 0, 0, NULL, 0);
        old_eff->dispatcher(old_eff, effClose, 0, 0, NULL, 0);
    }
    if (old_mod) FreeLibrary(old_mod);
    x->v_midi_queue.head = 0; x->v_midi_queue.tail = 0;
    t_atom list[3];
    atom_setsym(&list[0], gensym("loaded")); atom_setsym(&list[1], x->v_plugin_name);
    atom_setlong(&list[2], eff->numOutputs);
    outlet_list(x->v_info_outlet, NULL, 3, list);
    object_post((t_object*)x, "fastvst~: loaded '%s' (%d ins, %d outs)", x->v_plugin_name->s_name, eff->numInputs, eff->numOutputs);
    x->v_busy = 0;
}

void fastvst_presets(t_fastvst* x) {
    critical_enter(x->v_lock);
    if (x->v_effect) {
        t_atom a; atom_setlong(&a, x->v_effect->numPrograms);
        outlet_anything(x->v_info_outlet, gensym("numpresets"), 1, &a);
    }
    critical_exit(x->v_lock);
}

void fastvst_program(t_fastvst* x, long n) {
    critical_enter(x->v_lock);
    if (x->v_effect) x->v_effect->dispatcher(x->v_effect, effSetProgram, 0, (int32_t)n, NULL, 0);
    critical_exit(x->v_lock);
}

void fastvst_param(t_fastvst* x, t_symbol* s, long argc, t_atom* argv) {
    if (argc < 2) return;
    int index = (int)atom_getlong(argv); float value = (float)atom_getfloat(argv + 1);
    critical_enter(x->v_lock);
    if (x->v_effect) x->v_effect->setParameter(x->v_effect, index, value);
    critical_exit(x->v_lock);
}

void fastvst_midi(t_fastvst* x, t_symbol* s, long argc, t_atom* argv) {
    if (argc < 3) return;
    critical_enter(x->v_lock);
    int next_tail = (x->v_midi_queue.tail + 1) % MIDI_QUEUE_SIZE;
    if (next_tail != x->v_midi_queue.head) {
        VstMidiEvent* ev = &x->v_midi_queue.events[x->v_midi_queue.tail];
        memset(ev, 0, sizeof(VstMidiEvent));
        ev->type = 1; ev->byteSize = sizeof(VstMidiEvent);
        ev->midiData[0] = (char)atom_getlong(argv);
        ev->midiData[1] = (char)atom_getlong(argv + 1);
        ev->midiData[2] = (char)atom_getlong(argv + 2);
        x->v_midi_queue.tail = next_tail;
    }
    critical_exit(x->v_lock);
}

void fastvst_dsp64(t_fastvst* x, t_object* dsp64, short* count, double samplerate, long maxvectorsize, long flags) {
    x->v_samplerate = samplerate; x->v_vectorsize = maxvectorsize; x->v_dsp_on = 1;
    if (maxvectorsize > x->v_silence_buf_size) {
        x->v_silence_buf = (double*)sysmem_resizeptr(x->v_silence_buf, maxvectorsize * sizeof(double));
        memset(x->v_silence_buf, 0, maxvectorsize * sizeof(double));
        x->v_silence_buf_size = maxvectorsize;
    }
    critical_enter(x->v_lock);
    if (x->v_effect) {
        x->v_effect->dispatcher(x->v_effect, effSetSampleRate, 0, 0, NULL, (float)samplerate);
        x->v_effect->dispatcher(x->v_effect, effSetBlockSize, 0, (int32_t)maxvectorsize, NULL, 0);
        x->v_effect->dispatcher(x->v_effect, effMainsChanged, 0, 1, NULL, 0);
    }
    critical_exit(x->v_lock);
    dsp_add64(dsp64, (t_object*)x, (t_perfroutine64)fastvst_perform64, 0, NULL);
}

void fastvst_perform64(t_fastvst* x, t_object* dsp64, double** ins, long numins, double** outs, long numouts, long sampleframes, long flags, void *userparam) {
    if (critical_tryenter(x->v_lock) == MAX_ERR_NONE) {
        if (x->v_effect && x->v_effect->processDoubleReplacing && (x->v_effect->flags & (1 << 12))) {
            if (x->v_midi_queue.head != x->v_midi_queue.tail) {
                int count = 0; void* ev_ptrs[32];
                while (x->v_midi_queue.head != x->v_midi_queue.tail && count < 32) {
                    ev_ptrs[count] = &x->v_midi_queue.events[x->v_midi_queue.head];
                    x->v_midi_queue.head = (x->v_midi_queue.head + 1) % MIDI_QUEUE_SIZE; count++;
                }
                struct { int32_t numEvents; intptr_t reserved; void* events[32]; } block_evs;
                block_evs.numEvents = count; block_evs.reserved = 0;
                for (int i = 0; i < count; i++) block_evs.events[i] = ev_ptrs[i];
                x->v_effect->dispatcher(x->v_effect, effProcessEvents, 0, 0, &block_evs, 0);
            }
            long vst_ins = x->v_effect->numInputs; long vst_outs = x->v_effect->numOutputs;
            for (long i = 0; i < vst_ins && i < x->v_max_io; i++) {
                x->v_ins_array[i] = (i < x->v_num_ins) ? ins[i] : x->v_silence_buf;
            }
            for (long i = 0; i < vst_outs && i < x->v_max_io; i++) {
                x->v_outs_array[i] = (i < x->v_num_outs) ? outs[i] : x->v_silence_buf;
            }
            x->v_effect->processDoubleReplacing(x->v_effect, x->v_ins_array, x->v_outs_array, (int32_t)sampleframes);
        } else {
            for (int i = 0; i < x->v_num_outs && i < x->v_num_ins; i++) {
                memcpy(outs[i], ins[i], sampleframes * sizeof(double));
            }
        }
        critical_exit(x->v_lock);
    } else {
        for (int i = 0; i < numouts; i++) memset(outs[i], 0, sampleframes * sizeof(double));
    }
}

void fastvst_assist(t_fastvst* x, void* b, long m, long a, char* s) {
    if (m == ASSIST_INLET) {
        if (a == 0) sprintf(s, "Messages (plug, presets, program, param, midi) and Signal Inlet 1");
        else sprintf(s, "Signal Inlet %ld", a + 1);
    } else {
        if (a < x->v_num_outs) sprintf(s, "Signal Outlet %ld", a + 1);
        else sprintf(s, "Info Outlet");
    }
}

intptr_t VSTCALLBACK hostCallback(AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt) {
    t_fastvst* x = (effect) ? (t_fastvst*)effect->user : NULL;
    switch (opcode) {
        case audioMasterVersion: return 2400;
        case audioMasterGetSampleRate: return (x) ? (intptr_t)x->v_samplerate : 44100;
        case audioMasterGetBlockSize: return (x) ? (intptr_t)x->v_vectorsize : 64;
        case audioMasterIOChanged: if (x) qelem_set(x->v_qelem); return 1;
        default: return 0;
    }
}
