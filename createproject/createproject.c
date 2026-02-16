#include "ext.h"
#include "ext_obex.h"
#include <windows.h>

typedef struct _createproject {
    t_object s_obj;
    long log;
    void *log_outlet;
} t_createproject;

void *createproject_new(t_symbol *s, long argc, t_atom *argv);
void createproject_create(t_createproject *x, t_symbol *s);
void createproject_log(t_createproject *x, const char *fmt, ...);
void createproject_assist(t_createproject *x, void *b, long m, long a, char *s);
void copy_directory_recursively(t_createproject *x, const char *src_dir, const char *dest_dir);

t_class *createproject_class;

void ext_main(void *r) {
    t_class *c;

    common_symbols_init();

    c = class_new("createproject", (method)createproject_new, (method)NULL, sizeof(t_createproject), 0L, A_GIMME, 0);
    class_addmethod(c, (method)createproject_create, "create", A_SYM, 0);
    class_addmethod(c, (method)createproject_assist, "assist", A_CANT, 0);

    CLASS_ATTR_LONG(c, "log", 0, t_createproject, log);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "onoff", "Enable Logging");
    CLASS_ATTR_DEFAULT(c, "log", 0, "0");

    class_register(CLASS_BOX, c);
    createproject_class = c;
}

void convert_path_to_windows(const char* max_path, char* win_path) {
    int i = 0;
    while (max_path[i] != '\0') {
        if (max_path[i] == '/') {
            win_path[i] = '\\';
        } else {
            win_path[i] = max_path[i];
        }
        i++;
    }
    win_path[i] = '\0';
}

void createproject_log(t_createproject *x, const char *fmt, ...) {
    if (x->log && x->log_outlet) {
        char buf[1024];
        char final_buf[1100];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, 1024, fmt, args);
        va_end(args);
        snprintf(final_buf, 1100, "createproject: %s", buf);
        outlet_anything(x->log_outlet, gensym(final_buf), 0, NULL);
    }
}

void createproject_create(t_createproject *x, t_symbol *s) {
    const char *dest_path_max = s->s_name;
    char dest_path_win[MAX_PATH];
    const char *template_path = "D:\\\\[Library]\\\\[Audio]\\\\[Works]\\\\[Projects]\\\\[Template]";

    char path_copy[MAX_PATH];
    strncpy(path_copy, dest_path_max, MAX_PATH);

    long len = strlen(path_copy);
    if (len > 0 && path_copy[len - 1] == '/') {
        path_copy[len - 1] = '\0';
    }

    char *last_slash = strrchr(path_copy, '/');
    if (last_slash) {
        *last_slash = '\0';
        char *parent_folder = strrchr(path_copy, '/');
        if (parent_folder) {
            parent_folder++;
            if (strcmp(parent_folder, "[Projects]") != 0) {
                 object_error((t_object *)x, "Direct parent folder must be '[Projects]'.");
                 return;
            }
        } else {
            object_error((t_object *)x, "Direct parent folder must be '[Projects]'.");
            return;
        }
    } else {
        object_error((t_object *)x, "Invalid path: Could not determine parent folder.");
        return;
    }

    createproject_log(x, "Received path %s", dest_path_max);
    convert_path_to_windows(dest_path_max, dest_path_win);
    createproject_log(x, "Converted path to %s", dest_path_win);

    if (!CreateDirectoryA(dest_path_win, NULL)) {
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            createproject_log(x, "Directory already exists: %s", dest_path_win);
        } else {
            object_error((t_object *)x, "Failed to create directory: %s (Error %ld)", dest_path_win, GetLastError());
            return;
        }
    } else {
        createproject_log(x, "Successfully created directory: %s", dest_path_win);
    }

    copy_directory_recursively(x, template_path, dest_path_win);
    createproject_log(x, "Project creation complete.");
}

void *createproject_new(t_symbol *s, long argc, t_atom *argv) {
    t_createproject *x = (t_createproject *)object_alloc(createproject_class);
    if (x) {
        x->log = 0;
        x->log_outlet = NULL;

        attr_args_process(x, argc, argv);

        if (x->log) {
            x->log_outlet = outlet_new((t_object *)x, NULL);
        }
    }
    return (x);
}

void createproject_assist(t_createproject *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "Inlet 1: (create <path>) Create Project from Template");
    } else {
        if (x->log) {
            switch (a) {
                case 0: sprintf(s, "Outlet 1: Logging Outlet"); break;
            }
        }
    }
}

void copy_directory_recursively(t_createproject *x, const char *src_dir, const char *dest_dir) {
    WIN32_FIND_DATAA find_file_data;
    HANDLE h_find = INVALID_HANDLE_VALUE;
    char search_path[MAX_PATH];

    snprintf(search_path, MAX_PATH, "%s\\*", src_dir);
    h_find = FindFirstFileA(search_path, &find_file_data);

    if (h_find == INVALID_HANDLE_VALUE) {
        object_error((t_object *)x, "Failed to find first file in %s (Error %ld)", src_dir, GetLastError());
        return;
    }

    do {
        const char *file_name = find_file_data.cFileName;
        if (strcmp(file_name, ".") != 0 && strcmp(file_name, "..") != 0) {
            char src_path[MAX_PATH];
            char dest_path[MAX_PATH];

            snprintf(src_path, MAX_PATH, "%s\\%s", src_dir, file_name);
            snprintf(dest_path, MAX_PATH, "%s\\%s", dest_dir, file_name);

            if (find_file_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (CreateDirectoryA(dest_path, NULL)) {
                    createproject_log(x, "Created subdirectory %s", dest_path);
                    copy_directory_recursively(x, src_path, dest_path);
                } else {
                    object_error((t_object *)x, "Failed to create subdirectory %s (Error %ld)", dest_path, GetLastError());
                }
            } else {
                if (CopyFileA(src_path, dest_path, FALSE)) {
                    createproject_log(x, "Copied %s to %s", src_path, dest_path);
                } else {
                    object_error((t_object *)x, "Failed to copy file %s (Error %ld)", src_path, GetLastError());
                }
            }
        }
    } while (FindNextFileA(h_find, &find_file_data) != 0);

    FindClose(h_find);
}
