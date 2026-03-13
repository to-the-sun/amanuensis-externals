#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "ext.h"
#include "ext_obex.h"
#include "ext_systhread.h"
#include "z_dsp.h"

#define kEffectMagic 0x56737450

#pragma pack(push, 8)

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
    effSetSampleRate = 10,
    effSetBlockSize = 11,
    effMainsChanged = 12,
    effEditGetRect = 13,
    effEditOpen = 14,
    effEditClose = 15,
    effEditIdle = 19,
    effSetChunk = 24,
    effGetChunk = 23,
    effGetEffectName = 45,
    effGetProductString = 48
};

enum {
    effFlagsHasEditor = 1 << 0,
    effFlagsCanReplacing = 1 << 4,
    effFlagsProgramChunks = 1 << 5,
    effFlagsCanDoubleReplacing = 1 << 12
};

intptr_t hostCallback(AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt) {
    if (opcode == 1) { // audioMasterVersion
        return 2400;
    }
    return 0;
}

typedef struct _lazyvst {
    t_pxobject x_obj;
    HMODULE h_module;
    t_systhread thread;
    char path[2048];
    AEffect* effect;
    HWND hwnd;
    t_qelem *q_instantiate;
    VstPluginMainProc* main_ptr;
    long num_inputs;
    long num_outputs;

    float** f_ins;
    float** f_outs;
    float* f_in_data;
    float* f_out_data;
    long f_buffer_size;

    double cur_sr;
    long cur_ms;
} t_lazyvst;

void *lazyvst_new(t_symbol *s, long argc, t_atom *argv);
void lazyvst_free(t_lazyvst *x);
void *lazyvst_worker(t_lazyvst *x);
void lazyvst_open(t_lazyvst *x);
void lazyvst_do_open(t_lazyvst *x);
void lazyvst_do_instantiate(t_lazyvst *x);
void lazyvst_get_counts(t_lazyvst *x);
void lazyvst_dsp64(t_lazyvst *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
void lazyvst_perform64(t_lazyvst *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);
void lazyvst_assist(t_lazyvst *x, void *b, long m, long a, char *s);
void lazyvst_snapshot(t_lazyvst *x, t_symbol *s);
void lazyvst_do_snapshot(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv);
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

void ext_main(void *r) {
    t_class *c = class_new("lazyvst~", (method)lazyvst_new, (method)lazyvst_free, sizeof(t_lazyvst), 0L, A_GIMME, 0);

    class_addmethod(c, (method)lazyvst_open, "open", 0);
    class_addmethod(c, (method)lazyvst_snapshot, "snapshot", A_SYM, 0);
    class_addmethod(c, (method)lazyvst_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)lazyvst_assist, "assist", A_CANT, 0);

    class_dspinit(c);
    class_register(CLASS_BOX, c);
    lazyvst_class = c;

    lazyvst_register_window_class();
}

void lazyvst_open(t_lazyvst *x) {
    defer_low(x, (method)lazyvst_do_open, NULL, 0, NULL);
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

    WIN32_FILE_ATTRIBUTE_DATA fileInfo;
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &fileInfo)) {
        LARGE_INTEGER size;
        size.LowPart = fileInfo.nFileSizeLow;
        size.HighPart = fileInfo.nFileSizeHigh;

        post("lazyvst~: Identified file: %s", path);
        post("lazyvst~: File size: %lld bytes", size.QuadPart);
        post("lazyvst~: Attributes: 0x%lx", fileInfo.dwFileAttributes);

        x->h_module = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (x->h_module) {
            post("lazyvst~: Successfully loaded library at address %p", x->h_module);

            x->main_ptr = (VstPluginMainProc*)GetProcAddress(x->h_module, "VSTPluginMain");
            if (!x->main_ptr) {
                x->main_ptr = (VstPluginMainProc*)GetProcAddress(x->h_module, "main");
            }

            if (x->main_ptr) {
                post("lazyvst~: VST entry point found.");
                qelem_set(x->q_instantiate);
            } else {
                post("lazyvst~: VST entry point NOT FOUND.");
            }
        } else {
            post("lazyvst~: Failed to load library: %s (Error %lu)", path, GetLastError());
        }
    } else {
        post("lazyvst~: Could not identify file: %s (Error %lu)", path, GetLastError());
    }
    return NULL;
}

void lazyvst_get_counts(t_lazyvst *x) {
    x->num_inputs = 0;
    x->num_outputs = 0;

    if (x->path[0] == '\0') return;

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
            }
        }
        FreeLibrary(h_mod);
    }
}

void lazyvst_dsp64(t_lazyvst *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags) {
    x->cur_sr = samplerate;
    x->cur_ms = maxvectorsize;

    if (maxvectorsize > x->f_buffer_size) {
        if (x->f_in_data) sysmem_freeptr(x->f_in_data);
        if (x->f_out_data) sysmem_freeptr(x->f_out_data);

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
        x->f_buffer_size = maxvectorsize;
    }

    if (x->effect) {
        x->effect->dispatcher(x->effect, effSetSampleRate, 0, 0, NULL, (float)samplerate);
        x->effect->dispatcher(x->effect, effSetBlockSize, 0, maxvectorsize, NULL, 0.0f);
        x->effect->dispatcher(x->effect, effMainsChanged, 0, 1, NULL, 0.0f);
    }

    dsp_add64(dsp64, (t_object *)x, (t_perfroutine64)lazyvst_perform64, 0, NULL);
}

void lazyvst_perform64(t_lazyvst *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam) {
    if (!x->effect) {
        for (int i = 0; i < numouts; i++) {
            if (outs[i]) {
                memset(outs[i], 0, sizeof(double) * sampleframes);
            }
        }
        return;
    }

    if (x->effect->flags & effFlagsCanDoubleReplacing) {
        ((VstProcessDoubleProc*)x->effect->processDoubleReplacing)(x->effect, ins, outs, (int32_t)sampleframes);
    } else if (x->effect->flags & effFlagsCanReplacing) {
        if (x->f_ins && x->f_outs && x->f_in_data && x->f_out_data) {
            for (int i = 0; i < x->num_inputs; i++) {
                x->f_ins[i] = x->f_in_data + (i * x->f_buffer_size);
                if (i < numins && ins[i]) {
                    for (int j = 0; j < sampleframes; j++) {
                        x->f_ins[i][j] = (float)ins[i][j];
                    }
                } else {
                    memset(x->f_ins[i], 0, sizeof(float) * sampleframes);
                }
            }

            for (int i = 0; i < x->num_outputs; i++) {
                x->f_outs[i] = x->f_out_data + (i * x->f_buffer_size);
            }

            ((VstProcessProc*)x->effect->processReplacing)(x->effect, x->f_ins, x->f_outs, (int32_t)sampleframes);

            for (int i = 0; i < x->num_outputs; i++) {
                if (i < numouts && outs[i]) {
                    for (int j = 0; j < sampleframes; j++) {
                        outs[i][j] = (double)x->f_outs[i][j];
                    }
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
}

void lazyvst_assist(t_lazyvst *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        if (a == 0) {
            sprintf(s, "Inlet %ld (signal/messages): VST Input %ld. Messages: open, snapshot [path]", a + 1, a + 1);
        } else {
            sprintf(s, "Inlet %ld (signal): VST Input %ld", a + 1, a + 1);
        }
    } else {
        sprintf(s, "Outlet %ld (signal): VST Output %ld", a + 1, a + 1);
    }
}

void lazyvst_snapshot(t_lazyvst *x, t_symbol *s) {
    defer_low(x, (method)lazyvst_do_snapshot, s, 0, NULL);
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
    const char *dot = strchr(in, '.');
    if (dot) {
        // Check if prefix is numeric
        int is_numeric = 1;
        for (const char *q = in; q < dot; q++) {
            if (*q < '0' || *q > '9') {
                is_numeric = 0;
                break;
            }
        }
        if (is_numeric) p = dot + 1;
    }

    size_t in_len = strlen(p);
    if (in_len == 0) return NULL;

    size_t max_out_len = (in_len * 3) / 4 + 2;
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
                out[j++] = (buffer >> bits) & 0xFF;
            }
        } else if (p[i] == '=') {
            break;
        }
    }

    *out_len = j;
    return out;
}

void lazyvst_do_snapshot(t_lazyvst *x, t_symbol *s, long argc, t_atom *argv) {
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

    t_dictionary *d = NULL;
    err = dictionary_read(filename, path, &d);
    if (err || !d) {
        post("lazyvst~: ERROR - Could not read snapshot file (Error %d): %s", err, filename);
        return;
    }

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

    size_t decoded_size = 0;
    unsigned char *decoded_ptr = lazyvst_base64_decode(blob_str, &decoded_size);

    if (decoded_ptr && decoded_size > 0) {
        // VST effSetChunk: index 0 = bank, 1 = program
        intptr_t index = is_bank ? 0 : 1;
        x->effect->dispatcher(x->effect, effSetChunk, (int32_t)index, (intptr_t)decoded_size, decoded_ptr, 0.0f);
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
            post("lazyvst~: VST instance created successfully on main thread.");
            effect->user = x;
            x->effect = effect;

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

            if (effectName[0] != '\0') {
                post("lazyvst~: VST Name: %s", effectName);
            } else {
                post("lazyvst~: VST Name: [Unknown]");
            }

            post("lazyvst~: Programs: %d", effect->numPrograms);
            post("lazyvst~: Parameters: %d", effect->numParams);
            post("lazyvst~: Inputs: %d", effect->numInputs);
            post("lazyvst~: Outputs: %d", effect->numOutputs);
            post("lazyvst~: Initial Delay: %d", effect->initialDelay);
            post("lazyvst~: Unique ID: 0x%08X", effect->uniqueID);
            post("lazyvst~: VST Version: %d", effect->version);
            post("lazyvst~: Flags: 0x%08X", effect->flags);
        } else if (effect) {
            post("lazyvst~: Failed magic number check. Expected 0x%lx, Observed 0x%lx.", kEffectMagic, effect->magic);
        } else {
            post("lazyvst~: Entry point returned NULL.");
        }
    }
}

void *lazyvst_new(t_symbol *s, long argc, t_atom *argv) {
    t_lazyvst *x = (t_lazyvst *)object_alloc(lazyvst_class);

    if (x) {
        x->h_module = NULL;
        x->thread = NULL;
        x->path[0] = '\0';
        x->effect = NULL;
        x->hwnd = NULL;
        x->num_inputs = 0;
        x->num_outputs = 0;
        x->f_ins = NULL;
        x->f_outs = NULL;
        x->f_in_data = NULL;
        x->f_out_data = NULL;
        x->f_buffer_size = 0;
        x->cur_sr = 0;
        x->cur_ms = 0;
        x->q_instantiate = qelem_new(x, (method)lazyvst_do_instantiate);

        if (argc > 0 && atom_gettype(argv) == A_SYM) {
            t_symbol *path_sym = atom_getsym(argv);
            strncpy(x->path, path_sym->s_name, 2048);
            x->path[2047] = '\0';

            lazyvst_get_counts(x);

            if (x->num_inputs > 0) {
                x->f_ins = (float**)sysmem_newptr(sizeof(float*) * x->num_inputs);
            }
            if (x->num_outputs > 0) {
                x->f_outs = (float**)sysmem_newptr(sizeof(float*) * x->num_outputs);
            }

            dsp_setup((t_pxobject *)x, x->num_inputs);

            for (long i = 0; i < x->num_outputs; i++) {
                outlet_new((t_object *)x, "signal");
            }

            systhread_create((method)lazyvst_worker, x, 0, 0, 0, &x->thread);
        } else {
            dsp_setup((t_pxobject *)x, 0);
            post("lazyvst~: No valid path argument provided.");
        }
    }
    return x;
}

void lazyvst_free(t_lazyvst *x) {
    dsp_free((t_pxobject *)x);

    if (x->q_instantiate) {
        qelem_free(x->q_instantiate);
        x->q_instantiate = NULL;
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
    if (x->f_in_data) sysmem_freeptr(x->f_in_data);
    if (x->f_out_data) sysmem_freeptr(x->f_out_data);

    if (x->h_module) {
        FreeLibrary(x->h_module);
        x->h_module = NULL;
    }
}
