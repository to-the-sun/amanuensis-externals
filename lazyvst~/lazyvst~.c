#include <windows.h>
#include "ext.h"
#include "ext_obex.h"

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

                x->h_module = LoadLibraryA(path);
                if (x->h_module) {
                    post("lazyvst~: Successfully loaded library at address %p", x->h_module);

                    FARPROC main_ptr = GetProcAddress(x->h_module, "main");
                    FARPROC vmain_ptr = GetProcAddress(x->h_module, "VSTPluginMain");
                    FARPROC gpf_ptr = GetProcAddress(x->h_module, "GetPluginFactory");

                    post("lazyvst~: Entry point 'main': %s", main_ptr ? "FOUND" : "NOT FOUND");
                    post("lazyvst~: Entry point 'VSTPluginMain': %s", vmain_ptr ? "FOUND" : "NOT FOUND");
                    post("lazyvst~: Entry point 'GetPluginFactory': %s", gpf_ptr ? "FOUND" : "NOT FOUND");
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
