#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "ext.h"
#include "ext_obex.h"
#include "ext_systhread.h"
#include "ext_critical.h"
#include "ext_buffer.h"
#include "z_dsp.h"

#define kEffectMagic 0x56737450

#pragma pack(push, 8)

typedef struct VstTimeInfo {
    double samplePos;
    double sampleRate;
    double nanoSeconds;
    double ppqPos;
    double tempo;
    double barStartPos;
    double cycleStartPos;
    double cycleEndPos;
    int32_t timeSigNumerator;
    int32_t timeSigDenominator;
    int32_t samplesToNextClock;
    int32_t flags;
} VstTimeInfo;

struct AEffect;

typedef intptr_t (VstHostCallback)(struct AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
typedef intptr_t (VstDispatcherProc)(struct AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
typedef void (VstProcessProc)(struct AEffect* effect, float** inputs, float** outputs, int32_t sampleframes);
typedef void (VstProcessDoubleProc)(struct AEffect* effect, double** inputs, double** outputs, int32_t sampleframes);

typedef struct AEffect {
    int32_t magic;
    VstDispatcherProc* dispatcher;
    void* process;
    void* setParameter;
    void* getParameter;
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
    void* processReplacing;
    void* processDoubleReplacing;
    char future[56];
} AEffect;

typedef struct ERect {
    int16_t top;
    int16_t left;
    int16_t bottom;
    int16_t right;
} ERect;

#pragma pack(pop)

typedef AEffect* (VstPluginMainProc)(VstHostCallback* audioMaster);

enum {
    effOpen = 0,
    effClose,
    effSetProgram = 2,
    effGetProgram = 3,
    effGetParam = 4,
    effSetParam = 5,
    effSetSampleRate = 10,
    effSetBlockSize = 11,
    effMainsChanged = 12,
    effEditGetRect = 13,
    effEditOpen = 14,
    effEditClose = 15,
    effEditIdle = 19,
    effSetChunk = 24,
    effGetChunk = 23,
    effBeginSetProgram = 67,
    effEndSetProgram = 68,
    effGetParamName = 8,
    effGetParamLabel = 9,
    effGetParamDisplay = 10,
    effGetEffectName = 45,
    effGetProductString = 48
};

enum {
    effFlagsHasEditor = 1 << 0,
    effFlagsCanReplacing = 1 << 4,
    effFlagsProgramChunks = 1 << 5,
    effFlagsCanDoubleReplacing = 1 << 12
};

enum {
    audioMasterVersion = 1,
    audioMasterIdle = 3,
    audioMasterGetTime = 7,
    audioMasterSizeWindow = 11,
    audioMasterGetSampleRate = 13,
    audioMasterGetBlockSize = 14,
    audioMasterGetInputLatency = 15,
    audioMasterGetOutputLatency = 16,
    audioMasterGetCurrentProcessLevel = 23,
    audioMasterGetAutomationState = 26,
    audioMasterGetVendorString = 32,
    audioMasterGetProductString = 33,
    audioMasterGetVendorVersion = 34,
    audioMasterCanDo = 37,
    audioMasterGetLanguage = 38,
    audioMasterGetDirectory = 41,
    audioMasterUpdateDisplay = 42
};

struct _lazyvst;
typedef struct _lazyvst t_lazyvst;

typedef struct _lazyvst {
    t_pxobject x_obj;
    HMODULE h_module;
    t_systhread thread;
    char path[2048];
    AEffect* effect;
    HWND hwnd;
    t_qelem *q_instantiate;
    VstPluginMainProc* main_ptr;
    long num_inputs;    // Max object inlet count
    long num_outputs;   // Max object outlet count
    long vst_inputs;    // Plugin's actual input count
    long vst_outputs;   // Plugin's actual output count

    float** f_ins;
    float** f_outs;
    double** d_ins;
    double** d_outs;
    float* f_in_data;
    float* f_out_data;
    long f_buffer_size;

    long f_ins_alloc;
    long f_outs_alloc;
    long d_ins_alloc;
    long d_outs_alloc;

    float* f_silence;
    double* d_silence;

    double cur_sr;
    long cur_ms;
    long debug_host;
    long loading;
    t_critical lock;

    VstTimeInfo time_info;
    t_symbol *b_src_name;
    t_symbol *b_dest_name;
    double b_start_ms;
    double b_end_ms;
    long b_busy;
    t_qelem *q_bounce;
    void *outlet_bounce;
} t_lazyvst;

intptr_t hostCallback(struct AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt) {
    t_lazyvst *x = (effect) ? (t_lazyvst*)effect->user : NULL;

    if (x) {
        x->time_info.sampleRate = (x->cur_sr > 0) ? x->cur_sr : 44100.0;
        x->time_info.nanoSeconds = 0.0;
        x->time_info.ppqPos = 0.0;
        x->time_info.tempo = 120.0;
        x->time_info.barStartPos = 0.0;
        x->time_info.cycleStartPos = 0.0;
        x->time_info.cycleEndPos = 0.0;
        x->time_info.timeSigNumerator = 4;
        x->time_info.timeSigDenominator = 4;
        x->time_info.samplesToNextClock = 0;
        x->time_info.flags = 1 | 2 | 1024 | 2048; // TransportChanged | TransportPlaying | TimeSigValid | TempoValid
    }

    // Comprehensive host call tracing
    if (x && x->debug_host) {
        if (opcode != audioMasterGetTime && opcode != audioMasterIdle) {
            post("lazyvst~: Plugin calling host: op=%d, idx=%d, val=%ld, ptr=%p, opt=%f", opcode, index, (long)value, ptr, opt);
        }
    }

    switch (opcode) {
        case audioMasterVersion:
            return 2400;
        case audioMasterIdle:
            return 0;
        case audioMasterGetTime:
            return (intptr_t)(x ? &x->time_info : NULL);
        case audioMasterGetSampleRate:
            return (intptr_t)((x && x->cur_sr > 0) ? x->cur_sr : 44100.0);
        case audioMasterGetBlockSize:
            return (intptr_t)((x && x->cur_ms > 0) ? x->cur_ms : 512);
        case audioMasterGetCurrentProcessLevel:
            return 0; // Unknown
        case audioMasterGetAutomationState:
            return 0; // Off
        case audioMasterGetVendorString:
            if (ptr) strcpy((char*)ptr, "Cycling '74");
            return 1;
        case audioMasterGetProductString:
            if (ptr) strcpy((char*)ptr, "Max");
            return 1;
        case audioMasterGetVendorVersion:
            return 1000;
        case audioMasterGetLanguage:
            return 1; // English
        case audioMasterCanDo:
            if (ptr) {
                const char* canDo = (const char*)ptr;
                if (strcmp(canDo, "sendVstEvents") == 0 ||
                    strcmp(canDo, "sendVstMidiEvent") == 0 ||
                    strcmp(canDo, "supplyIdle") == 0 ||
                    strcmp(canDo, "sizeWindow") == 0 ||
                    strcmp(canDo, "sendVstTimeInfo") == 0 ||
                    strcmp(canDo, "reportConnectionChanges") == 0 ||
                    strcmp(canDo, "acceptIOChanges") == 0) {
                    return 1;
                }
            }
            return 0;
        case audioMasterUpdateDisplay:
            return 0;
        case audioMasterGetDirectory:
            if (x && ptr) {
                // Return directory of the VST DLL
                char dir[2048];
                strncpy(dir, x->path, 2048);
                char *last_slash = strrchr(dir, '/');
                if (!last_slash) last_slash = strrchr(dir, '\\');
                if (last_slash) *last_slash = '\0';
                strcpy((char*)ptr, dir);
                return (intptr_t)ptr;
            }
            return 0;
        default:
            return 0;
    }
}

void *lazyvst_new(t_symbol *s, long argc, t_atom *argv);
void lazyvst_free(t_lazyvst *x);
void *lazyvst_worker(t_lazyvst *x);
void lazyvst_open(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv);
void lazyvst_do_open(t_lazyvst *x);
void lazyvst_do_instantiate(t_lazyvst *x);
long lazyvst_get_counts(t_lazyvst *x);
void lazyvst_dsp64(t_lazyvst *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
void lazyvst_perform64(t_lazyvst *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);
void lazyvst_assist(t_lazyvst *x, void *b, long m, long a, char *s);
void lazyvst_snapshot(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv);
void lazyvst_do_snapshot(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv);
void lazyvst_plug(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv);
void lazyvst_open_plug_dialog(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv);
void lazyvst_do_plug(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv);
void lazyvst_bang(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv);
void lazyvst_anything(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv);
void lazyvst_vst(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv);
void lazyvst_getchunk(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv);
void lazyvst_vstinfo(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv);
void lazyvst_params(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv);
void lazyvst_list(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv);
void lazyvst_vstdebug(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv);
void lazyvst_bounce(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv);
void *lazyvst_bounce_worker(t_lazyvst *x);
void lazyvst_bounce_done(t_lazyvst *x);
unsigned char *lazyvst_base64_decode(const char *in, size_t *out_len);

static t_class *lazyvst_class;

LRESULT CALLBACK VstWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    t_lazyvst *x = (t_lazyvst *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_NCCREATE: {
            LPCREATESTRUCT lpcs = (LPCREATESTRUCT)lParam;
            SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)lpcs->lpCreateParams);
            return DefWindowProc(hwnd, msg, wParam, lParam);
        }
        case WM_CLOSE:
            if (x && x->effect) {
                x->effect->dispatcher(x->effect, effEditClose, 0, 0, NULL, 0.0f);
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (x) {
                KillTimer(hwnd, 1);
                x->hwnd = NULL;
            }
            return 0;
        case WM_TIMER:
            if (x && x->effect) {
                x->effect->dispatcher(x->effect, effEditIdle, 0, 0, NULL, 0.0f);
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void lazyvst_register_window_class() {
    WNDCLASSEX wcex;
    if (GetClassInfoEx(GetModuleHandle(NULL), "LazyVstWin", &wcex)) return;

    memset(&wcex, 0, sizeof(WNDCLASSEX));
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wcex.lpfnWndProc = VstWndProc;
    wcex.hInstance = GetModuleHandle(NULL);
    wcex.lpszClassName = "LazyVstWin";
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassEx(&wcex);
}

void lazyvst_open(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv) {
    if (x->loading) {
        defer_low(x, (method)lazyvst_open, s, argc, argv);
        return;
    }
    defer_low(x, (method)lazyvst_do_open, NULL, 0, NULL);
}

void ext_main(void *r) {
    common_symbols_init();
    t_class *c = class_new("lazyvst~", (method)lazyvst_new, (method)lazyvst_free, sizeof(t_lazyvst), 0L, A_GIMME, 0);

    class_addmethod(c, (method)lazyvst_open, "open", A_GIMME, 0);
    class_addmethod(c, (method)lazyvst_snapshot, "snapshot", A_GIMME, 0);
    class_addmethod(c, (method)lazyvst_plug, "plug", A_GIMME, 0);
    class_addmethod(c, (method)lazyvst_vst, "vst", A_GIMME, 0);
    class_addmethod(c, (method)lazyvst_getchunk, "getchunk", A_GIMME, 0);
    class_addmethod(c, (method)lazyvst_vstinfo, "vstinfo", A_GIMME, 0);
    class_addmethod(c, (method)lazyvst_params, "params", A_GIMME, 0);
    class_addmethod(c, (method)lazyvst_list, "list", A_GIMME, 0);
    class_addmethod(c, (method)lazyvst_vstdebug, "vstdebug", A_GIMME, 0);
    class_addmethod(c, (method)lazyvst_bounce, "bounce", A_GIMME, 0);
    class_addmethod(c, (method)lazyvst_bang, "bang", A_GIMME, 0);
    class_addmethod(c, (method)lazyvst_anything, "anything", A_GIMME, 0);
    class_addmethod(c, (method)lazyvst_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)lazyvst_assist, "assist", A_CANT, 0);

    class_dspinit(c);
    class_register(CLASS_BOX, c);
    lazyvst_class = c;

    lazyvst_register_window_class();
}

void lazyvst_bang(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv) {
    if (x->loading) {
        defer_low(x, (method)lazyvst_bang, s, argc, argv);
        return;
    }
    post("lazyvst~: bang received");
}

void lazyvst_anything(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv) {
    if (x->loading) {
        defer_low(x, (method)lazyvst_anything, s, argc, argv);
        return;
    }
    post("lazyvst~: UNHANDLED message received: %s (argc=%ld)", s->s_name, argc);
}

void lazyvst_vst(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv) {
    if (x->loading) {
        defer_low(x, (method)lazyvst_vst, s, argc, argv);
        return;
    }
    if (!x->effect) return;
    if (argc < 1) return;

    int32_t opcode = (int32_t)atom_getlong(argv);
    int32_t index = (argc > 1) ? (int32_t)atom_getlong(argv + 1) : 0;
    intptr_t value = (argc > 2) ? (intptr_t)atom_getlong(argv + 2) : 0;

    post("lazyvst~: Manual VST dispatch: op=%d, idx=%d, val=%ld", opcode, index, (long)value);
    intptr_t ret = x->effect->dispatcher(x->effect, opcode, index, value, NULL, 0.0f);
    post("lazyvst~: Result: %ld", (long)ret);
}

void lazyvst_getchunk(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv) {
    if (x->loading) {
        defer_low(x, (method)lazyvst_getchunk, s, argc, argv);
        return;
    }
    if (!x->effect) return;

    int32_t isbank = (argc > 0) ? (int32_t)atom_getlong(argv) : 0;
    void *ptr = NULL;
    intptr_t size = x->effect->dispatcher(x->effect, effGetChunk, isbank, 0, &ptr, 0.0f);
    post("lazyvst~: Current chunk (isbank=%ld): size=%ld, ptr=%p", isbank, (long)size, ptr);
    if (ptr && size > 0) {
        char dump[64];
        char *dptr = dump;
        unsigned char *bptr = (unsigned char *)ptr;
        for (int k = 0; k < 16 && k < (int)size; k++) {
            dptr += sprintf(dptr, "%02X ", bptr[k]);
        }
        post("lazyvst~: Chunk head: %s", dump);
    }
}

void lazyvst_vstinfo(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv) {
    if (x->loading) {
        defer_low(x, (method)lazyvst_vstinfo, s, argc, argv);
        return;
    }
    post("lazyvst~: INFO");
    post("  SR: %.1f", x->cur_sr);
    post("  MS: %ld", x->cur_ms);
    if (x->effect) {
        post("  UniqueID: 0x%08X", x->effect->uniqueID);
        post("  Version: %d", x->effect->version);
    }
}

void lazyvst_params(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv) {
    if (x->loading) {
        defer_low(x, (method)lazyvst_params, s, argc, argv);
        return;
    }
    if (!x->effect) return;
    post("lazyvst~: Listing parameters (%d total):", x->effect->numParams);
    for (int i = 0; i < x->effect->numParams; i++) {
        char name[256] = {0};
        char display[256] = {0};
        char label[256] = {0};
        x->effect->dispatcher(x->effect, effGetParamName, i, 0, name, 0.0f);
        x->effect->dispatcher(x->effect, effGetParamDisplay, i, 0, display, 0.0f);
        x->effect->dispatcher(x->effect, effGetParamLabel, i, 0, label, 0.0f);
        float val = x->effect->getParameter ? ((float (*)(struct AEffect*, int32_t))x->effect->getParameter)(x->effect, i) : 0.0f;
        post("  [%d] %s: %s %s (raw=%f)", i + 1, name, display, label, val);
    }
}

void lazyvst_list(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv) {
    if (x->loading) {
        defer_low(x, (method)lazyvst_list, s, argc, argv);
        return;
    }
    if (!x->effect) return;
    if (argc < 2) return;

    int32_t param_idx = (int32_t)atom_getlong(argv) - 1; // 1-indexed to 0-indexed
    float param_val = atom_getfloat(argv + 1);

    if (param_idx >= 0 && param_idx < x->effect->numParams) {
        if (x->effect->setParameter) {
            ((void (*)(AEffect*, int32_t, float))x->effect->setParameter)(x->effect, param_idx, param_val);
        }
    }
}

void lazyvst_vstdebug(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv) {
    if (x->loading) {
        defer_low(x, (method)lazyvst_vstdebug, s, argc, argv);
        return;
    }
    long state = (argc > 0) ? atom_getlong(argv) : 0;
    x->debug_host = state;
    post("lazyvst~: Host call debugging %s.", state ? "ENABLED" : "DISABLED");
}

void lazyvst_bounce(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv) {
    if (x->loading) {
        defer_low(x, (method)lazyvst_bounce, s, argc, argv);
        return;
    }

    if (!x->effect) {
        error("lazyvst~: cannot bounce, no plugin loaded");
        return;
    }

    if (x->b_busy) {
        error("lazyvst~: bounce already in progress");
        return;
    }

    if (argc < 2) {
        error("lazyvst~: bounce requires source and destination buffer names");
        return;
    }

    t_symbol *src = atom_getsym(argv);
    t_symbol *dest = atom_getsym(argv + 1);
    double start_ms = (argc > 2) ? atom_getfloat(argv + 2) : 0.0;
    double end_ms = (argc > 3) ? atom_getfloat(argv + 3) : -1.0;

    // Verify buffers exist
    t_buffer_ref *src_ref = buffer_ref_new((t_object *)x, src);
    t_buffer_ref *dest_ref = buffer_ref_new((t_object *)x, dest);

    if (!buffer_ref_getobject(src_ref)) {
        error("lazyvst~: source buffer '%s' not found", src->s_name);
        object_free(src_ref);
        object_free(dest_ref);
        return;
    }

    if (!buffer_ref_getobject(dest_ref)) {
        error("lazyvst~: destination buffer '%s' not found", dest->s_name);
        object_free(src_ref);
        object_free(dest_ref);
        return;
    }

    object_free(src_ref);
    object_free(dest_ref);

    x->b_src_name = src;
    x->b_dest_name = dest;
    x->b_start_ms = start_ms;
    x->b_end_ms = end_ms;
    x->b_busy = 1;

    post("lazyvst~: beginning offline bounce from '%s' to '%s'...", src->s_name, dest->s_name);
    systhread_create((method)lazyvst_bounce_worker, x, 0, 0, 0, &x->thread);
}

void *lazyvst_bounce_worker(t_lazyvst *x) {
    t_buffer_ref *src_ref = buffer_ref_new((t_object *)x, x->b_src_name);
    t_buffer_ref *dest_ref = buffer_ref_new((t_object *)x, x->b_dest_name);
    t_buffer_obj *src_obj = buffer_ref_getobject(src_ref);
    t_buffer_obj *dest_obj = buffer_ref_getobject(dest_ref);
    float *src_samples = NULL;
    float *dest_samples = NULL;
    float **vst_f_ins = NULL;
    float **vst_f_outs = NULL;

    if (!src_obj || !dest_obj) {
        error("lazyvst~: bounce failed, buffer(s) no longer valid");
        goto cleanup;
    }

    double sr = buffer_getsamplerate(src_obj);
    if (sr <= 0) sr = 44100.0;
    long long total_frames = buffer_getframecount(src_obj);
    long src_chans = (long)buffer_getchannelcount(src_obj);
    long dest_chans = (long)buffer_getchannelcount(dest_obj);
    long long dest_frames = buffer_getframecount(dest_obj);

    long long start_frame = (long long)round(x->b_start_ms * sr / 1000.0);
    long long end_frame = (x->b_end_ms < 0) ? total_frames : (long long)round(x->b_end_ms * sr / 1000.0);

    if (start_frame < 0) start_frame = 0;
    if (end_frame > total_frames) end_frame = total_frames;
    if (start_frame >= end_frame) {
        error("lazyvst~: bounce failed, invalid frame range");
        goto cleanup;
    }

    long long frames_to_process = end_frame - start_frame;
    if (frames_to_process > dest_frames) {
        error("lazyvst~: bounce failed, destination buffer too small (needs %lld frames, has %lld)", frames_to_process, dest_frames);
        goto cleanup;
    }

    src_samples = buffer_locksamples(src_obj);
    dest_samples = buffer_locksamples(dest_obj);

    if (!src_samples || !dest_samples) {
        error("lazyvst~: bounce failed, could not lock buffer samples");
        goto cleanup;
    }

    critical_enter(x->lock);
    long block_size = 1024;
    x->effect->dispatcher(x->effect, effSetSampleRate, 0, 0, NULL, (float)sr);
    x->effect->dispatcher(x->effect, effSetBlockSize, 0, block_size, NULL, 0.0f);
    x->effect->dispatcher(x->effect, effMainsChanged, 0, 1, NULL, 0.0f);

    vst_f_ins = (x->vst_inputs > 0) ? (float**)sysmem_newptr(sizeof(float*) * x->vst_inputs) : NULL;
    vst_f_outs = (x->vst_outputs > 0) ? (float**)sysmem_newptr(sizeof(float*) * x->vst_outputs) : NULL;

    if (x->vst_inputs > 0 && !vst_f_ins) { error("lazyvst~: bounce failed, memory allocation error"); goto cleanup; }
    if (x->vst_outputs > 0 && !vst_f_outs) { error("lazyvst~: bounce failed, memory allocation error"); goto cleanup; }

    if (vst_f_ins) {
        for (int i = 0; i < x->vst_inputs; i++) {
            vst_f_ins[i] = (float*)sysmem_newptrclear(sizeof(float) * block_size);
            if (!vst_f_ins[i]) { error("lazyvst~: bounce failed, memory allocation error"); goto cleanup; }
        }
    }
    if (vst_f_outs) {
        for (int i = 0; i < x->vst_outputs; i++) {
            vst_f_outs[i] = (float*)sysmem_newptrclear(sizeof(float) * block_size);
            if (!vst_f_outs[i]) { error("lazyvst~: bounce failed, memory allocation error"); goto cleanup; }
        }
    }

    x->time_info.sampleRate = sr;
    x->time_info.tempo = 120.0;
    x->time_info.timeSigNumerator = 4;
    x->time_info.timeSigDenominator = 4;
    x->time_info.flags = 1 | 2 | 1024 | 2048; // TransportChanged | TransportPlaying | TimeSigValid | TempoValid

    for (long long f = 0; f < frames_to_process; f += block_size) {
        long cur_block = (long)((f + block_size > frames_to_process) ? (frames_to_process - f) : block_size);

        // Fill inputs
        if (vst_f_ins) {
            for (int c = 0; c < x->vst_inputs; c++) {
                if (c < src_chans) {
                    for (int i = 0; i < cur_block; i++) {
                        vst_f_ins[c][i] = src_samples[(start_frame + f + i) * src_chans + c];
                    }
                } else {
                    memset(vst_f_ins[c], 0, sizeof(float) * cur_block);
                }
            }
        }

        // Process
        x->time_info.samplePos = (double)(start_frame + f);
        x->time_info.ppqPos = x->time_info.samplePos / sr * (x->time_info.tempo / 60.0);
        x->time_info.nanoSeconds = x->time_info.samplePos / sr * 1000000000.0;

        if (x->effect->flags & effFlagsCanReplacing) {
            ((VstProcessProc*)x->effect->processReplacing)(x->effect, vst_f_ins, vst_f_outs, (int32_t)cur_block);
        }

        // Fill outputs - destination always starts at 0
        if (vst_f_outs) {
            for (int c = 0; c < dest_chans; c++) {
                if (c < x->vst_outputs) {
                    for (int i = 0; i < cur_block; i++) {
                        dest_samples[(f + i) * dest_chans + c] = vst_f_outs[c][i];
                    }
                }
            }
        }
    }

    x->effect->dispatcher(x->effect, effMainsChanged, 0, 0, NULL, 0.0f);

    if (vst_f_ins) {
        for (int i = 0; i < x->vst_inputs; i++) if (vst_f_ins[i]) sysmem_freeptr(vst_f_ins[i]);
        sysmem_freeptr(vst_f_ins);
        vst_f_ins = NULL;
    }
    if (vst_f_outs) {
        for (int i = 0; i < x->vst_outputs; i++) if (vst_f_outs[i]) sysmem_freeptr(vst_f_outs[i]);
        sysmem_freeptr(vst_f_outs);
        vst_f_outs = NULL;
    }

    critical_exit(x->lock);

cleanup:
    if (vst_f_ins) {
        for (int i = 0; i < x->vst_inputs; i++) if (vst_f_ins[i]) sysmem_freeptr(vst_f_ins[i]);
        sysmem_freeptr(vst_f_ins);
    }
    if (vst_f_outs) {
        for (int i = 0; i < x->vst_outputs; i++) if (vst_f_outs[i]) sysmem_freeptr(vst_f_outs[i]);
        sysmem_freeptr(vst_f_outs);
    }

    if (src_obj && src_samples) buffer_unlocksamples(src_obj);
    if (dest_obj) {
        if (dest_samples) {
            buffer_setdirty(dest_obj);
            buffer_unlocksamples(dest_obj);
        }
    }

    if (src_ref) object_free(src_ref);
    if (dest_ref) object_free(dest_ref);

    qelem_set(x->q_bounce);
    return NULL;
}

void lazyvst_bounce_done(t_lazyvst *x) {
    if (x->thread) {
        systhread_join(x->thread, NULL);
        x->thread = NULL;
    }
    x->b_busy = 0;
    outlet_bang(x->outlet_bounce);
    post("lazyvst~: offline bounce complete.");
}

void lazyvst_snapshot(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv) {
    if (x->loading) {
        defer_low(x, (method)lazyvst_snapshot, s, argc, argv);
        return;
    }
    t_symbol *path = NULL;
    if (argc > 0 && argv[0].a_type == A_SYM) {
        path = atom_getsym(argv);
    } else if (s != gensym("snapshot")) {
        path = s;
    }

    if (path) {
        post("lazyvst~: Received snapshot message for: %s", path->s_name);
        defer_low(x, (method)lazyvst_do_snapshot, path, 0, NULL);
    } else {
        post("lazyvst~: snapshot message received but no path provided (argc=%ld, s=%s)", argc, s ? s->s_name : "NULL");
    }
}

void lazyvst_plug(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv) {
    if (x->loading) {
        defer_low(x, (method)lazyvst_plug, s, argc, argv);
        return;
    }
    t_symbol *path = NULL;
    if (argc > 0 && argv[0].a_type == A_SYM) {
        path = atom_getsym(argv);
    } else if (s != gensym("plug")) {
        path = s;
    }

    x->loading = 1;
    if (path) {
        post("lazyvst~: Received plug message for: %s", path->s_name);
        defer_low(x, (method)lazyvst_do_plug, path, 0, NULL);
    } else {
        defer_low(x, (method)lazyvst_open_plug_dialog, NULL, 0, NULL);
    }
}

void lazyvst_open_plug_dialog(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv) {
    char filename[MAX_FILENAME_CHARS];
    short path_id;
    t_fourcc type = 0;
    char fullpath[MAX_PATH_CHARS];

    filename[0] = '\0';
    open_promptset("Select VST Plugin");
    if (open_dialog(filename, &path_id, &type, NULL, 0) == 0) {
        if (path_toabsolutesystempath(path_id, filename, fullpath) == MAX_ERR_NONE) {
            t_symbol *path_sym = gensym(fullpath);
            post("lazyvst~: Dialog selected: %s", fullpath);
            lazyvst_do_plug(x, path_sym, 0, NULL);
            return;
        }
    }
    x->loading = 0;
}

void lazyvst_do_plug(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv) {
    if (x->thread) {
        systhread_join(x->thread, NULL);
        x->thread = NULL;
    }

    critical_enter(x->lock);
    if (x->effect) {
        x->effect->dispatcher(x->effect, effMainsChanged, 0, 0, NULL, 0.0f);
        if (x->hwnd) {
            x->effect->dispatcher(x->effect, effEditClose, 0, 0, NULL, 0.0f);
            DestroyWindow(x->hwnd);
            x->hwnd = NULL;
        }
        x->effect->dispatcher(x->effect, effClose, 0, 0, NULL, 0.0f);
        x->effect = NULL;
    }

    if (x->h_module) {
        FreeLibrary(x->h_module);
        x->h_module = NULL;
    }
    x->main_ptr = NULL;
    critical_exit(x->lock);

    short path_id = 0;
    char filename[MAX_FILENAME_CHARS];
    char fullpath[2048];

    if (path_frompathname(s->s_name, &path_id, filename)) {
        // Fallback to raw string if path_frompathname fails
        strncpy(x->path, s->s_name, 2048);
    } else {
        if (path_toabsolutesystempath(path_id, filename, fullpath)) {
            strncpy(x->path, s->s_name, 2048);
        } else {
            strncpy(x->path, fullpath, 2048);
        }
    }
    x->path[2047] = '\0';

    const char *fname = strrchr(x->path, '/');
    if (!fname) fname = strrchr(x->path, '\\');
    if (fname) fname++;
    else fname = x->path;

    WIN32_FILE_ATTRIBUTE_DATA fileInfo;
    if (GetFileAttributesExA(x->path, GetFileExInfoStandard, &fileInfo)) {
        if (lazyvst_get_counts(x)) {
            post("lazyvst~: began loading %s", fname);
            systhread_create((method)lazyvst_worker, x, 0, 0, 0, &x->thread);
        }
    } else {
        error("lazyvst~: plugin %s not found", fname);
    }
}

void lazyvst_do_open(t_lazyvst *x) {
    if (!x->effect) {
        post("lazyvst~: VST not loaded yet.");
        return;
    }

    if (!(x->effect->flags & effFlagsHasEditor)) {
        post("lazyvst~: VST does not have an editor.");
        return;
    }

    if (x->hwnd) {
        ShowWindow(x->hwnd, SW_SHOW);
        SetForegroundWindow(x->hwnd);
        return;
    }

    ERect* rect = NULL;
    x->effect->dispatcher(x->effect, effEditGetRect, 0, 0, &rect, 0.0f);

    if (rect) {
        int width = rect->right - rect->left;
        int height = rect->bottom - rect->top;

        x->hwnd = CreateWindowEx(
            0,
            "LazyVstWin",
            "VST GUI",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            CW_USEDEFAULT, CW_USEDEFAULT,
            width + 16, height + 39, // Add some space for title bar and borders
            NULL,
            NULL,
            GetModuleHandle(NULL),
            (LPVOID)x
        );

        if (x->hwnd) {
            x->effect->dispatcher(x->effect, effEditOpen, 0, 0, x->hwnd, 0.0f);
            ShowWindow(x->hwnd, SW_SHOW);
            SetForegroundWindow(x->hwnd);
            SetTimer(x->hwnd, 1, 20, NULL);
        } else {
            post("lazyvst~: Failed to create window.");
        }
    } else {
        post("lazyvst~: Failed to get editor rect.");
    }
}

void *lazyvst_worker(t_lazyvst *x) {
    const char *path = x->path;

    x->h_module = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (x->h_module) {
        x->main_ptr = (VstPluginMainProc*)GetProcAddress(x->h_module, "VSTPluginMain");
        if (!x->main_ptr) {
            x->main_ptr = (VstPluginMainProc*)GetProcAddress(x->h_module, "main");
        }

        if (x->main_ptr) {
            qelem_set(x->q_instantiate);
        }
    }
    return NULL;
}

long lazyvst_get_counts(t_lazyvst *x) {
    long success = 0;
    x->num_inputs = 0;
    x->num_outputs = 0;

    if (x->path[0] == '\0') return 0;

    HMODULE h_mod = LoadLibraryExA(x->path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (h_mod) {
        VstPluginMainProc* main_p = (VstPluginMainProc*)GetProcAddress(h_mod, "VSTPluginMain");
        if (!main_p) {
            main_p = (VstPluginMainProc*)GetProcAddress(h_mod, "main");
        }

        if (main_p) {
            AEffect* effect = main_p((VstHostCallback*)hostCallback);
            if (effect && effect->magic == kEffectMagic) {
                x->num_inputs = (long)effect->numInputs;
                x->num_outputs = (long)effect->numOutputs;
                effect->dispatcher(effect, effClose, 0, 0, NULL, 0.0f);
                success = 1;
            }
        }
        FreeLibrary(h_mod);
    }
    return success;
}

void lazyvst_dsp64(t_lazyvst *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags) {
    x->cur_sr = samplerate;
    x->cur_ms = maxvectorsize;

    if (maxvectorsize > x->f_buffer_size) {
        if (x->f_in_data) sysmem_freeptr(x->f_in_data);
        if (x->f_out_data) sysmem_freeptr(x->f_out_data);
        if (x->f_silence) sysmem_freeptr(x->f_silence);
        if (x->d_silence) sysmem_freeptr(x->d_silence);

        if (x->num_inputs > 0) {
            x->f_in_data = (float*)sysmem_newptr(sizeof(float) * maxvectorsize * x->num_inputs);
        } else {
            x->f_in_data = NULL;
        }

        if (x->num_outputs > 0) {
            x->f_out_data = (float*)sysmem_newptr(sizeof(float) * maxvectorsize * x->num_outputs);
        } else {
            x->f_out_data = NULL;
        }
        x->f_silence = (float*)sysmem_newptr(sizeof(float) * maxvectorsize);
        memset(x->f_silence, 0, sizeof(float) * maxvectorsize);
        x->d_silence = (double*)sysmem_newptr(sizeof(double) * maxvectorsize);
        memset(x->d_silence, 0, sizeof(double) * maxvectorsize);
        x->f_buffer_size = maxvectorsize;
    }

    critical_enter(x->lock);
    if (x->effect) {
        x->effect->dispatcher(x->effect, effSetSampleRate, 0, 0, NULL, (float)samplerate);
        x->effect->dispatcher(x->effect, effSetBlockSize, 0, maxvectorsize, NULL, 0.0f);
        x->effect->dispatcher(x->effect, effMainsChanged, 0, 1, NULL, 0.0f);
    }
    critical_exit(x->lock);

    dsp_add64(dsp64, (t_object *)x, (t_perfroutine64)lazyvst_perform64, 0, NULL);
}

void lazyvst_perform64(t_lazyvst *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam) {
    if (critical_tryenter(x->lock) != MAX_ERR_NONE) {
        // Throughput for 4x4
        for (int i = 0; i < numouts; i++) {
            if (outs[i]) {
                if (i < 4 && i < numins && ins[i]) {
                    memcpy(outs[i], ins[i], sizeof(double) * sampleframes);
                } else {
                    memset(outs[i], 0, sizeof(double) * sampleframes);
                }
            }
        }
        return;
    }

    if (!x->effect) {
        // Throughput for 4x4
        for (int i = 0; i < numouts; i++) {
            if (outs[i]) {
                if (i < 4 && i < numins && ins[i]) {
                    memcpy(outs[i], ins[i], sizeof(double) * sampleframes);
                } else {
                    memset(outs[i], 0, sizeof(double) * sampleframes);
                }
            }
        }
        critical_exit(x->lock);
        return;
    }

    if (x->effect->flags & effFlagsCanDoubleReplacing) {
        if (x->d_ins && x->d_outs) {
            for (int i = 0; i < x->vst_inputs; i++) {
                if (i < numins && ins[i]) {
                    x->d_ins[i] = ins[i];
                } else {
                    x->d_ins[i] = x->d_silence;
                }
            }
            for (int i = 0; i < x->vst_outputs; i++) {
                if (i < numouts && outs[i]) {
                    x->d_outs[i] = outs[i];
                } else {
                    // Plugin wants to write to extra output, but we don't have enough outlets
                    // Use silence buffer as a dummy sink to prevent crashes
                    x->d_outs[i] = x->d_silence;
                }
            }
            ((VstProcessDoubleProc*)x->effect->processDoubleReplacing)(x->effect, x->d_ins, x->d_outs, (int32_t)sampleframes);

            // Zero any remaining Max outlets that the VST didn't cover
            for (int i = x->vst_outputs; i < numouts; i++) {
                if (outs[i]) {
                    memset(outs[i], 0, sizeof(double) * sampleframes);
                }
            }
        }
    } else if (x->effect->flags & effFlagsCanReplacing) {
        if (x->f_ins && x->f_outs && x->f_in_data && x->f_out_data) {
            for (int i = 0; i < x->vst_inputs; i++) {
                if (i < x->num_inputs) {
                    x->f_ins[i] = x->f_in_data + (i * x->f_buffer_size);
                    if (i < numins && ins[i]) {
                        for (int j = 0; j < sampleframes; j++) {
                            x->f_ins[i][j] = (float)ins[i][j];
                        }
                    } else {
                        memset(x->f_ins[i], 0, sizeof(float) * sampleframes);
                    }
                } else {
                    x->f_ins[i] = x->f_silence;
                }
            }

            for (int i = 0; i < x->vst_outputs; i++) {
                if (i < x->num_outputs) {
                    x->f_outs[i] = x->f_out_data + (i * x->f_buffer_size);
                } else {
                    x->f_outs[i] = x->f_silence;
                }
            }

            ((VstProcessProc*)x->effect->processReplacing)(x->effect, x->f_ins, x->f_outs, (int32_t)sampleframes);

            for (int i = 0; i < x->vst_outputs; i++) {
                if (i < x->num_outputs && i < numouts && outs[i]) {
                    for (int j = 0; j < sampleframes; j++) {
                        outs[i][j] = (double)x->f_outs[i][j];
                    }
                }
            }

            // Zero any remaining Max outlets
            for (int i = x->vst_outputs; i < numouts; i++) {
                if (outs[i]) {
                    memset(outs[i], 0, sizeof(double) * sampleframes);
                }
            }
        }
    } else {
        for (int i = 0; i < numouts; i++) {
            if (outs[i]) {
                memset(outs[i], 0, sizeof(double) * sampleframes);
            }
        }
    }
    critical_exit(x->lock);
}

void lazyvst_assist(t_lazyvst *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        if (a == 0) {
            sprintf(s, "Inlet %ld (signal/messages): VST Input %ld (min. 4 inlets), bounce [src] [dest] [start] [end]", a + 1, a + 1);
        } else {
            sprintf(s, "Inlet %ld (signal): VST Input %ld", a + 1, a + 1);
        }
    } else {
        if (a < x->num_outputs) {
            sprintf(s, "Outlet %ld (signal): VST Output %ld (min. 4 outlets)", a + 1, a + 1);
        } else {
            sprintf(s, "Outlet %ld (bang): offline bounce complete", a + 1);
        }
    }
}


unsigned char *lazyvst_base64_decode(const char *in, size_t *out_len) {
    static const int B64index[256] = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1,  0, 63,
        52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
        -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, 63,
        -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
        41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
    };

    const char *p = in;
    size_t expected_len = 0;
    const char *dot = strchr(in, '.');
    if (dot) {
        char prefix[32];
        size_t plen = dot - in;
        if (plen < 31) {
            strncpy(prefix, in, plen);
            prefix[plen] = '\0';
            expected_len = (size_t)atol(prefix);
            if (expected_len > 0) p = dot + 1;
        }
    }

    size_t in_len = strlen(p);
    if (in_len == 0) return NULL;

    size_t max_out_len = expected_len ? expected_len : (in_len * 3) / 4 + 2;
    unsigned char *out = (unsigned char *)sysmem_newptr(max_out_len);
    if (!out) return NULL;

    size_t j = 0;
    uint32_t buffer = 0;
    int bits = 0;

    for (size_t i = 0; i < in_len; i++) {
        int val = B64index[(unsigned char)p[i]];
        if (val != -1) {
            buffer = (buffer << 6) | val;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                if (expected_len == 0 || j < expected_len) {
                    out[j++] = (buffer >> bits) & 0xFF;
                }
            }
        } else if (p[i] == '=') {
            break;
        }
    }

    *out_len = j;
    return out;
}

void lazyvst_do_snapshot(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv) {
    post("lazyvst~: Executing deferred snapshot for %s", s->s_name);
    if (!x->effect) {
        post("lazyvst~: ERROR - No VST plugin loaded to apply snapshot.");
        return;
    }

    if (!(x->effect->flags & effFlagsProgramChunks)) {
        post("lazyvst~: ERROR - VST does not support state chunks.");
        return;
    }

    short path = 0;
    char filename[MAX_FILENAME_CHARS];
    t_max_err err;

    if (path_frompathname(s->s_name, &path, filename)) {
        post("lazyvst~: ERROR - Could not resolve snapshot path: %s", s->s_name);
        return;
    }
    post("lazyvst~: Snapshot file resolved to ID %d, Name %s", path, filename);

    t_dictionary *d = NULL;
    err = dictionary_read(filename, path, &d);
    if (err || !d) {
        post("lazyvst~: ERROR - Could not read snapshot file (Error %d): %s", err, filename);
        return;
    }
    post("lazyvst~: Snapshot dictionary read successfully.");

    t_dictionary *snapshot_dict = NULL;
    if (dictionary_getdictionary(d, gensym("snapshot"), (t_object **)&snapshot_dict) || !snapshot_dict) {
        post("lazyvst~: ERROR - No 'snapshot' sub-dictionary found.");

        long numkeys = 0;
        t_symbol **keys = NULL;
        dictionary_getkeys(d, &numkeys, &keys);
        if (keys) {
            post("lazyvst~: Top-level keys found:");
            for (int i = 0; i < numkeys; i++) post("  %s", keys[i]->s_name);
            dictionary_freekeys(d, numkeys, keys);
        }

        object_free(d);
        return;
    }

    const char *blob_str = NULL;
    if (dictionary_getstring(snapshot_dict, gensym("blob"), &blob_str) || !blob_str) {
        post("lazyvst~: ERROR - No 'blob' string found in snapshot.");
        object_free(d);
        return;
    }

    t_atom_long is_bank = 0;
    dictionary_getlong(snapshot_dict, gensym("isbank"), &is_bank);

    t_atom_long saved_id = 0;
    if (dictionary_getlong(snapshot_dict, gensym("pluginsaveduniqueid"), &saved_id) == MAX_ERR_NONE) {
        if (saved_id != 0 && saved_id != (t_atom_long)x->effect->uniqueID) {
            post("lazyvst~: WARNING - Snapshot uniqueID (0x%08llX) does not match plugin uniqueID (0x%08X)", saved_id, x->effect->uniqueID);
        }
    }

    size_t decoded_size = 0;
    unsigned char *decoded_ptr = lazyvst_base64_decode(blob_str, &decoded_size);

    if (decoded_ptr && decoded_size > 0) {
        // Hex dump diagnostics (first 16 bytes)
        char dump[64];
        char *dptr = dump;
        for (int k = 0; k < 16 && k < (int)decoded_size; k++) {
            dptr += sprintf(dptr, "%02X ", decoded_ptr[k]);
        }
        post("lazyvst~: Decoded state head: %s", dump);

        // VST effSetChunk: index 0 = bank, 1 = program
        intptr_t chunk_index = is_bank ? 0 : 1;

        post("lazyvst~: Applying state (index=%ld, size=%zu bytes)...", (long)chunk_index, decoded_size);

        // Comprehensive VST state restoration sequence
        x->effect->dispatcher(x->effect, effMainsChanged, 0, 0, NULL, 0.0f);

        intptr_t cur_prog = x->effect->dispatcher(x->effect, effGetProgram, 0, 0, NULL, 0.0f);
        x->effect->dispatcher(x->effect, effBeginSetProgram, 0, 0, NULL, 0.0f);

        // Some plugins require effSetProgram *before* effSetChunk
        x->effect->dispatcher(x->effect, effSetProgram, 0, cur_prog, NULL, 0.0f);

        intptr_t ret = x->effect->dispatcher(x->effect, effSetChunk, (int32_t)chunk_index, (intptr_t)decoded_size, decoded_ptr, 0.0f);

        x->effect->dispatcher(x->effect, effEndSetProgram, 0, 0, NULL, 0.0f);

        // Re-set program again after chunk to ensure update
        x->effect->dispatcher(x->effect, effSetProgram, 0, cur_prog, NULL, 0.0f);

        x->effect->dispatcher(x->effect, effMainsChanged, 0, 1, NULL, 0.0f);

        post("lazyvst~: VST state applied (ret=%ld).", (long)ret);

        // Fallback for plugins that ignore program chunks or require bank chunks
        if (ret == 0) {
            intptr_t other_index = (chunk_index == 1) ? 0 : 1;
            post("lazyvst~: dispatcher returned 0. Trying index %ld restoration...", (long)other_index);
            intptr_t ret2 = x->effect->dispatcher(x->effect, effSetChunk, (int32_t)other_index, (intptr_t)decoded_size, decoded_ptr, 0.0f);
            post("lazyvst~: Fallback restore ret=%ld", (long)ret2);
            // Refresh again after fallback
            x->effect->dispatcher(x->effect, effSetProgram, 0, cur_prog, NULL, 0.0f);
        }

        // If editor is open, signal idle multiple times to ensure GUI update
        if (x->hwnd) {
            for (int i=0; i<10; i++) x->effect->dispatcher(x->effect, effEditIdle, 0, 0, NULL, 0.0f);
        }
    } else {
        post("lazyvst~: ERROR - Failed to decode snapshot blob.");
    }

    if (d) object_free(d);
    if (decoded_ptr) sysmem_freeptr(decoded_ptr);
}

void lazyvst_do_instantiate(t_lazyvst *x) {
    if (x->main_ptr) {
        AEffect* effect = x->main_ptr((VstHostCallback*)hostCallback);
        if (effect && effect->magic == kEffectMagic) {
            effect->user = x;

            critical_enter(x->lock);
            x->effect = effect;
            x->vst_inputs = (long)effect->numInputs;
            x->vst_outputs = (long)effect->numOutputs;

            if (x->vst_inputs > x->f_ins_alloc) {
                if (x->f_ins) sysmem_freeptr(x->f_ins);
                x->f_ins = (float**)sysmem_newptr(sizeof(float*) * x->vst_inputs);
                x->f_ins_alloc = x->vst_inputs;
            }
            if (x->vst_inputs > x->d_ins_alloc) {
                if (x->d_ins) sysmem_freeptr(x->d_ins);
                x->d_ins = (double**)sysmem_newptr(sizeof(double*) * x->vst_inputs);
                x->d_ins_alloc = x->vst_inputs;
            }
            if (x->vst_outputs > x->f_outs_alloc) {
                if (x->f_outs) sysmem_freeptr(x->f_outs);
                x->f_outs = (float**)sysmem_newptr(sizeof(float*) * x->vst_outputs);
                x->f_outs_alloc = x->vst_outputs;
            }
            if (x->vst_outputs > x->d_outs_alloc) {
                if (x->d_outs) sysmem_freeptr(x->d_outs);
                x->d_outs = (double**)sysmem_newptr(sizeof(double*) * x->vst_outputs);
                x->d_outs_alloc = x->vst_outputs;
            }
            critical_exit(x->lock);

            effect->dispatcher(effect, effOpen, 0, 0, NULL, 0.0f);

            if (x->cur_sr > 0) {
                effect->dispatcher(effect, effSetSampleRate, 0, 0, NULL, (float)x->cur_sr);
                effect->dispatcher(effect, effSetBlockSize, 0, x->cur_ms, NULL, 0.0f);
                effect->dispatcher(effect, effMainsChanged, 0, 1, NULL, 0.0f);
            }

            char effectName[256];
            memset(effectName, 0, 256);
            effect->dispatcher(effect, effGetEffectName, 0, 0, effectName, 0.0f);

            if (effectName[0] == '\0') {
                // Fallback to Product String
                effect->dispatcher(effect, effGetProductString, 0, 0, effectName, 0.0f);
            }

            const char *filename = strrchr(x->path, '/');
            if (!filename) filename = strrchr(x->path, '\\');
            if (filename) filename++;
            else filename = x->path;

            const char *pluginName = (effectName[0] != '\0') ? effectName : filename;

            post("lazyvst~: %s finished loading with %d inlets, %d outlets, %d parameters and %d presets.",
                pluginName, (int)x->vst_inputs, (int)x->vst_outputs, effect->numParams, effect->numPrograms);
        }
    }
    x->loading = 0;
}

void *lazyvst_new(t_symbol *s, long argc, t_atom *argv) {
    t_lazyvst *x = (t_lazyvst *)object_alloc(lazyvst_class);

    if (x) {
        critical_new(&x->lock);
        x->h_module = NULL;
        x->thread = NULL;
        x->path[0] = '\0';
        x->effect = NULL;
        x->hwnd = NULL;
        x->num_inputs = 4; // Default to 4
        x->num_outputs = 4; // Default to 4
        x->vst_inputs = 0;
        x->vst_outputs = 0;
        x->f_ins = NULL;
        x->f_outs = NULL;
        x->d_ins = NULL;
        x->d_outs = NULL;
        x->f_in_data = NULL;
        x->f_out_data = NULL;
        x->f_buffer_size = 0;
        x->f_ins_alloc = 0;
        x->f_outs_alloc = 0;
        x->d_ins_alloc = 0;
        x->d_outs_alloc = 0;
        x->f_silence = NULL;
        x->d_silence = NULL;
        x->cur_sr = 0;
        x->cur_ms = 0;
        x->debug_host = 0;
        x->loading = 0;
        x->q_instantiate = qelem_new(x, (method)lazyvst_do_instantiate);
        x->q_bounce = qelem_new(x, (method)lazyvst_bounce_done);
        x->b_busy = 0;
        memset(&x->time_info, 0, sizeof(VstTimeInfo));

        if (argc > 0 && atom_gettype(argv) == A_SYM) {
            t_symbol *path_sym = atom_getsym(argv);
            strncpy(x->path, path_sym->s_name, 2048);
            x->path[2047] = '\0';

            const char *filename = strrchr(x->path, '/');
            if (!filename) filename = strrchr(x->path, '\\');
            if (filename) filename++;
            else filename = x->path;

            WIN32_FILE_ATTRIBUTE_DATA fileInfo;
            if (GetFileAttributesExA(x->path, GetFileExInfoStandard, &fileInfo)) {
                if (lazyvst_get_counts(x)) {
                    post("lazyvst~: began loading %s", filename);
                    if (x->num_inputs < 4) x->num_inputs = 4;
                    if (x->num_outputs < 4) x->num_outputs = 4;
                    x->loading = 1;
                    systhread_create((method)lazyvst_worker, x, 0, 0, 0, &x->thread);
                } else {
                    x->num_inputs = 4;
                    x->num_outputs = 4;
                }
            } else {
                error("lazyvst~: plugin %s not found", filename);
                x->num_inputs = 4;
                x->num_outputs = 4;
            }
        } else {
            x->num_inputs = 4;
            x->num_outputs = 4;
        }

        if (x->num_inputs > 0) {
            x->f_ins = (float**)sysmem_newptr(sizeof(float*) * x->num_inputs);
            x->f_ins_alloc = x->num_inputs;
            x->d_ins = (double**)sysmem_newptr(sizeof(double*) * x->num_inputs);
            x->d_ins_alloc = x->num_inputs;
        }
        if (x->num_outputs > 0) {
            x->f_outs = (float**)sysmem_newptr(sizeof(float*) * x->num_outputs);
            x->f_outs_alloc = x->num_outputs;
            x->d_outs = (double**)sysmem_newptr(sizeof(double*) * x->num_outputs);
            x->d_outs_alloc = x->num_outputs;
        }

        dsp_setup((t_pxobject *)x, x->num_inputs);

        for (long i = 0; i < x->num_outputs; i++) {
            outlet_new((t_object *)x, "signal");
        }
        x->outlet_bounce = outlet_new((t_object *)x, NULL);
    }
    return x;
}

void lazyvst_free(t_lazyvst *x) {
    dsp_free((t_pxobject *)x);

    if (x->q_instantiate) {
        qelem_free(x->q_instantiate);
        x->q_instantiate = NULL;
    }

    if (x->q_bounce) {
        qelem_free(x->q_bounce);
        x->q_bounce = NULL;
    }

    if (x->thread) {
        systhread_join(x->thread, NULL);
        x->thread = NULL;
    }

    if (x->effect) {
        x->effect->dispatcher(x->effect, effMainsChanged, 0, 0, NULL, 0.0f);
        if (x->hwnd) {
            x->effect->dispatcher(x->effect, effEditClose, 0, 0, NULL, 0.0f);
            DestroyWindow(x->hwnd);
            x->hwnd = NULL;
        }
        x->effect->dispatcher(x->effect, effClose, 0, 0, NULL, 0.0f);
        x->effect = NULL;
    }

    if (x->f_ins) sysmem_freeptr(x->f_ins);
    if (x->f_outs) sysmem_freeptr(x->f_outs);
    if (x->d_ins) sysmem_freeptr(x->d_ins);
    if (x->d_outs) sysmem_freeptr(x->d_outs);
    if (x->f_in_data) sysmem_freeptr(x->f_in_data);
    if (x->f_out_data) sysmem_freeptr(x->f_out_data);
    if (x->f_silence) sysmem_freeptr(x->f_silence);
    if (x->d_silence) sysmem_freeptr(x->d_silence);

    if (x->lock) critical_free(x->lock);

    if (x->h_module) {
        FreeLibrary(x->h_module);
        x->h_module = NULL;
    }
}
