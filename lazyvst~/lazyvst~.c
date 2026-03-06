#include <windows.h>
#include <stdint.h>
#include "ext.h"
#include "ext_obex.h"

#define kEffectMagic 0x56737450

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
} t_lazyvst;

void *lazyvst_new(t_symbol *s, long argc, t_atom *argv);
void lazyvst_free(t_lazyvst *x);

static t_class *lazyvst_class;

void ext_main(void *r) {
    t_class *c = class_new("lazyvst~", (method)lazyvst_new, (method)lazyvst_free, sizeof(t_lazyvst), 0L, A_GIMME, 0);

    class_register(CLASS_BOX, c);
    lazyvst_class = c;
}

void *lazyvst_new(t_symbol *s, long argc, t_atom *argv) {
    t_lazyvst *x = (t_lazyvst *)object_alloc(lazyvst_class);

    if (x) {
        x->h_module = NULL;
        if (argc > 0 && atom_gettype(argv) == A_SYM) {
            t_symbol *path_sym = atom_getsym(argv);
            const char *path = path_sym->s_name;

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

                    VstPluginMainProc* main_ptr = (VstPluginMainProc*)GetProcAddress(x->h_module, "VSTPluginMain");
                    if (!main_ptr) {
                        main_ptr = (VstPluginMainProc*)GetProcAddress(x->h_module, "main");
                    }

                    if (main_ptr) {
                        post("lazyvst~: VST entry point found.");
                        AEffect* effect = main_ptr((VstHostCallback*)hostCallback);
                        if (effect && effect->magic == kEffectMagic) {
                            post("lazyvst~: VST instance created successfully.");

                            effect->dispatcher(effect, effOpen, 0, 0, NULL, 0.0f);

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

                            effect->dispatcher(effect, effClose, 0, 0, NULL, 0.0f);
                        } else if (effect) {
                            post("lazyvst~: Failed magic number check. Expected 0x%lx, Observed 0x%lx.", kEffectMagic, effect->magic);
                        } else {
                            post("lazyvst~: Entry point returned NULL.");
                        }
                    } else {
                        post("lazyvst~: VST entry point NOT FOUND.");
                    }
                } else {
                    post("lazyvst~: Failed to load library: %s (Error %lu)", path, GetLastError());
                }
            } else {
                post("lazyvst~: Could not identify file: %s (Error %lu)", path, GetLastError());
            }
        } else {
            post("lazyvst~: No valid path argument provided.");
        }
    }
    return x;
}

void lazyvst_free(t_lazyvst *x) {
    if (x->h_module) {
        FreeLibrary(x->h_module);
        x->h_module = NULL;
    }
}
