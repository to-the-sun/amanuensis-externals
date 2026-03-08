#include <windows.h>
#include <stdint.h>
#include "ext.h"
#include "ext_obex.h"
#include "ext_systhread.h"
#include "ext_critical.h"
#include "../shared/logging.h"

#define kEffectMagic 0x56737450
#define LAZYVST_LOG_QUEUE_SIZE 64

#pragma pack(push, 8)

struct AEffect;

typedef intptr_t (VstHostCallback)(struct AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
typedef intptr_t (VstDispatcherProc)(struct AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);

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

#pragma pack(pop)

typedef AEffect* (VstPluginMainProc)(VstHostCallback* audioMaster);

enum {
    effOpen = 0,
    effClose,
    effGetEffectName = 45,
    effGetProductString = 48
};

intptr_t hostCallback(AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt) {
    if (opcode == 1) { // audioMasterVersion
        return 2400;
    }
    return 0;
}

typedef struct _lazyvst {
    t_object x_obj;
    HMODULE h_module;
    char path[MAX_PATH];
    t_systhread thread;
    void *qelem;
    long busy;
    long log;
    void *log_outlet;
    void *bang_outlet;

    char log_queue[LAZYVST_LOG_QUEUE_SIZE][1024];
    int log_head;
    int log_tail;
    t_critical log_lock;
    void *log_qelem;
} t_lazyvst;

void *lazyvst_new(t_symbol *s, long argc, t_atom *argv);
void lazyvst_free(t_lazyvst *x);
void lazyvst_assist(t_lazyvst *x, void *b, long m, long a, char *s);
void *lazyvst_worker(t_lazyvst *x);
void lazyvst_qfn(t_lazyvst *x);
void lazyvst_log_qfn(t_lazyvst *x);
void lazyvst_log(t_lazyvst *x, const char *fmt, ...);

static t_class *lazyvst_class;

void lazyvst_log(t_lazyvst *x, const char *fmt, ...) {
    if (!x || !x->log) return;
    va_list args;
    va_start(args, fmt);
    if (systhread_ismainthread()) {
        vcommon_log(x->log_outlet, x->log, "lazyvst~", fmt, args);
    } else {
        char buf[1024];
        vsnprintf(buf, 1024, fmt, args);
        critical_enter(x->log_lock);
        int next_tail = (x->log_tail + 1) % LAZYVST_LOG_QUEUE_SIZE;
        if (next_tail != x->log_head) {
            strncpy(x->log_queue[x->log_tail], buf, 1024);
            x->log_tail = next_tail;
        }
        critical_exit(x->log_lock);
        qelem_set(x->log_qelem);
    }
    va_end(args);
}

void lazyvst_log_qfn(t_lazyvst *x) {
    char msg[1024];
    while (1) {
        critical_enter(x->log_lock);
        if (x->log_head == x->log_tail) {
            critical_exit(x->log_lock);
            break;
        }
        strncpy(msg, x->log_queue[x->log_head], 1024);
        x->log_head = (x->log_head + 1) % LAZYVST_LOG_QUEUE_SIZE;
        critical_exit(x->log_lock);

        common_log(x->log_outlet, x->log, "lazyvst~", "%s", msg);
    }
}

void *lazyvst_worker(t_lazyvst *x) {
    const char *path = x->path;

    WIN32_FILE_ATTRIBUTE_DATA fileInfo;
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &fileInfo)) {
        LARGE_INTEGER size;
        size.LowPart = fileInfo.nFileSizeLow;
        size.HighPart = fileInfo.nFileSizeHigh;

        lazyvst_log(x, "Identified file: %s", path);
        lazyvst_log(x, "File size: %lld bytes", size.QuadPart);
        lazyvst_log(x, "Attributes: 0x%lx", fileInfo.dwFileAttributes);

        x->h_module = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (x->h_module) {
            lazyvst_log(x, "Successfully loaded library at address %p", x->h_module);

            VstPluginMainProc* main_ptr = (VstPluginMainProc*)GetProcAddress(x->h_module, "VSTPluginMain");
            if (!main_ptr) {
                main_ptr = (VstPluginMainProc*)GetProcAddress(x->h_module, "main");
            }

            if (main_ptr) {
                lazyvst_log(x, "VST entry point found.");
                AEffect* effect = main_ptr((VstHostCallback*)hostCallback);
                if (effect && effect->magic == kEffectMagic) {
                    lazyvst_log(x, "VST instance created successfully.");

                    effect->dispatcher(effect, effOpen, 0, 0, NULL, 0.0f);

                    char effectName[256];
                    memset(effectName, 0, 256);
                    effect->dispatcher(effect, effGetEffectName, 0, 0, effectName, 0.0f);

                    if (effectName[0] == '\0') {
                        // Fallback to Product String
                        effect->dispatcher(effect, effGetProductString, 0, 0, effectName, 0.0f);
                    }

                    if (effectName[0] != '\0') {
                        lazyvst_log(x, "VST Name: %s", effectName);
                    } else {
                        lazyvst_log(x, "VST Name: [Unknown]");
                    }

                    lazyvst_log(x, "Programs: %d", effect->numPrograms);
                    lazyvst_log(x, "Parameters: %d", effect->numParams);
                    lazyvst_log(x, "Inputs: %d", effect->numInputs);
                    lazyvst_log(x, "Outputs: %d", effect->numOutputs);
                    lazyvst_log(x, "Initial Delay: %d", effect->initialDelay);
                    lazyvst_log(x, "Unique ID: 0x%08X", effect->uniqueID);
                    lazyvst_log(x, "VST Version: %d", effect->version);
                    lazyvst_log(x, "Flags: 0x%08X", effect->flags);

                    effect->dispatcher(effect, effClose, 0, 0, NULL, 0.0f);
                } else if (effect) {
                    lazyvst_log(x, "Failed magic number check. Expected 0x%lx, Observed 0x%lx.", kEffectMagic, effect->magic);
                } else {
                    lazyvst_log(x, "Entry point returned NULL.");
                }
            } else {
                lazyvst_log(x, "VST entry point NOT FOUND.");
            }
        } else {
            lazyvst_log(x, "Failed to load library: %s (Error %lu)", path, GetLastError());
        }
    } else {
        lazyvst_log(x, "Could not identify file: %s (Error %lu)", path, GetLastError());
    }

    qelem_set(x->qelem);
    return NULL;
}

void lazyvst_qfn(t_lazyvst *x) {
    if (x->thread) {
        systhread_join(x->thread, NULL);
        x->thread = NULL;
    }
    outlet_bang(x->bang_outlet);
    x->busy = 0;
}

void ext_main(void *r) {
    t_class *c = class_new("lazyvst~", (method)lazyvst_new, (method)lazyvst_free, sizeof(t_lazyvst), 0L, A_GIMME, 0);

    class_addmethod(c, (method)lazyvst_assist, "assist", A_CANT, 0);

    CLASS_ATTR_LONG(c, "log", 0, t_lazyvst, log);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "onoff", "Enable Logging");
    CLASS_ATTR_DEFAULT(c, "log", 0, "0");

    class_register(CLASS_BOX, c);
    lazyvst_class = c;
}

void *lazyvst_new(t_symbol *s, long argc, t_atom *argv) {
    t_lazyvst *x = (t_lazyvst *)object_alloc(lazyvst_class);

    if (x) {
        x->h_module = NULL;
        x->log = 0;
        x->busy = 0;
        x->thread = NULL;
        x->qelem = qelem_new(x, (method)lazyvst_qfn);
        x->log_head = 0;
        x->log_tail = 0;
        critical_new(&x->log_lock);
        x->log_qelem = qelem_new(x, (method)lazyvst_log_qfn);
        memset(x->path, 0, MAX_PATH);

        t_symbol *path_sym = _sym_nothing;
        if (argc > 0 && atom_gettype(argv) == A_SYM && atom_getsym(argv)->s_name[0] != '@') {
            path_sym = atom_getsym(argv);
            argc--;
            argv++;
        }

        attr_args_process(x, argc, argv);

        x->log_outlet = outlet_new((t_object *)x, NULL);
        x->bang_outlet = outlet_new((t_object *)x, NULL);

        if (path_sym != _sym_nothing) {
            strncpy(x->path, path_sym->s_name, MAX_PATH - 1);
            x->busy = 1;
            systhread_create((method)lazyvst_worker, x, 0, 0, 0, &x->thread);
        } else {
            object_error((t_object *)x, "No valid path argument provided.");
        }
    }
    return x;
}

void lazyvst_free(t_lazyvst *x) {
    if (x->thread) {
        systhread_join(x->thread, NULL);
    }
    if (x->qelem) {
        qelem_free(x->qelem);
    }
    if (x->log_qelem) {
        qelem_free(x->log_qelem);
    }
    critical_free(x->log_lock);

    if (x->h_module) {
        FreeLibrary(x->h_module);
        x->h_module = NULL;
    }
}

void lazyvst_assist(t_lazyvst *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "Inlet 1: (symbol) absolute path to VST plugin");
    } else {
        switch (a) {
            case 0: sprintf(s, "Outlet 1: (bang) finished loading"); break;
            case 1: sprintf(s, "Outlet 2: (anything) Logging Outlet"); break;
        }
    }
}
