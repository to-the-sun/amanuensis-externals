#include "ext.h"
#include "ext_obex.h"
#include "ext_common.h"
#include "ext_buffer.h"
#include "ext_sysfile.h"
#include "ext_path.h"

typedef struct _varibuffer {
    t_object b_obj;
    t_symbol *b_name;
    void *b_outlet;
    long b_embed;
    long b_quiet;
} t_varibuffer;

t_class *varibuffer_class;

// Prototypes
void *varibuffer_new(t_symbol *s, long argc, t_atom *argv);
void varibuffer_free(t_varibuffer *x);
void varibuffer_assist(t_varibuffer *x, void *b, long m, long a, char *s);
void varibuffer_append(t_varibuffer *x, t_symbol *s, long argc, t_atom *argv);
void varibuffer_appendempty(t_varibuffer *x, t_symbol *s, long argc, t_atom *argv);
void varibuffer_clear(t_varibuffer *x);
void varibuffer_dump(t_varibuffer *x);
void varibuffer_getcount(t_varibuffer *x);
void varibuffer_getbufferlist(t_varibuffer *x);
void varibuffer_getshortname(t_varibuffer *x);
void varibuffer_getsize(t_varibuffer *x);
void varibuffer_send(t_varibuffer *x, t_symbol *s, long argc, t_atom *argv);
void varibuffer_open(t_varibuffer *x);
void varibuffer_wclose(t_varibuffer *x);
void varibuffer_readfolder(t_varibuffer *x, t_symbol *s, long argc, t_atom *argv);
void varibuffer_writetofolder(t_varibuffer *x, t_symbol *s, long argc, t_atom *argv);
void varibuffer_dblclick(t_varibuffer *x);
void varibuffer_readfolder_deferred(t_varibuffer *x, t_symbol *s, long argc, t_atom *argv);

// Helpers
long varibuffer_find_next_index(t_varibuffer *x);
void varibuffer_do_append(t_varibuffer *x, t_symbol *filename);
void varibuffer_do_appendempty(t_varibuffer *x, double duration, long channels);

void ext_main(void *r) {
    t_class *c = class_new("varibuffer~", (method)varibuffer_new, (method)varibuffer_free, sizeof(t_varibuffer), 0L, A_GIMME, 0);

    class_addmethod(c, (method)varibuffer_append, "append", A_GIMME, 0);
    class_addmethod(c, (method)varibuffer_appendempty, "appendempty", A_GIMME, 0);
    class_addmethod(c, (method)varibuffer_clear, "clear", 0);
    class_addmethod(c, (method)varibuffer_dump, "dump", 0);
    class_addmethod(c, (method)varibuffer_getcount, "getcount", 0);
    class_addmethod(c, (method)varibuffer_getbufferlist, "getbufferlist", 0);
    class_addmethod(c, (method)varibuffer_getshortname, "getshortname", 0);
    class_addmethod(c, (method)varibuffer_getsize, "getsize", 0);
    class_addmethod(c, (method)varibuffer_send, "send", A_GIMME, 0);
    class_addmethod(c, (method)varibuffer_open, "open", 0);
    class_addmethod(c, (method)varibuffer_wclose, "wclose", 0);
    class_addmethod(c, (method)varibuffer_readfolder, "readfolder", A_GIMME, 0);
    class_addmethod(c, (method)varibuffer_writetofolder, "writetofolder", A_GIMME, 0);
    class_addmethod(c, (method)varibuffer_dblclick, "dblclick", A_CANT, 0);
    class_addmethod(c, (method)varibuffer_assist, "assist", A_CANT, 0);

    CLASS_ATTR_LONG(c, "embed", 0, t_varibuffer, b_embed);
    CLASS_ATTR_STYLE_LABEL(c, "embed", 0, "onoff", "Save buffer references in patcher");

    CLASS_ATTR_LONG(c, "quiet", 0, t_varibuffer, b_quiet);
    CLASS_ATTR_STYLE_LABEL(c, "quiet", 0, "onoff", "Suppress warnings");

    class_register(CLASS_BOX, c);
    varibuffer_class = c;
}

void *varibuffer_new(t_symbol *s, long argc, t_atom *argv) {
    t_varibuffer *x = (t_varibuffer *)object_alloc(varibuffer_class);
    if (x) {
        x->b_name = (argc > 0 && atom_gettype(argv) == A_SYM) ? atom_getsym(argv) : gensym("varibuf");
        x->b_embed = 0;
        x->b_quiet = 0;

        attr_args_process(x, argc, argv);

        x->b_outlet = outlet_new(x, NULL);
    }
    return x;
}

void varibuffer_free(t_varibuffer *x) {
    // Basic cleanup
}

void varibuffer_assist(t_varibuffer *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "Messages");
    } else {
        sprintf(s, "Dump / Info Output");
    }
}

long varibuffer_find_next_index(t_varibuffer *x) {
    long i = 1;
    while (1) {
        char bufname[256];
        snprintf(bufname, 256, "%s.%ld", x->b_name->s_name, i);
        t_buffer_ref *ref = buffer_ref_new((t_object *)x, gensym(bufname));
        t_atom_long exists = buffer_ref_exists(ref);
        object_free(ref);
        if (!exists) return i;
        i++;
        if (i > 10000) return -1; // Safety break
    }
}

void varibuffer_do_append(t_varibuffer *x, t_symbol *filename) {
    long index = varibuffer_find_next_index(x);
    if (index < 0) return;

    char bufname[256];
    snprintf(bufname, 256, "%s.%ld", x->b_name->s_name, index);
    t_symbol *s_name = gensym(bufname);

    t_atom a[2];
    atom_setsym(&a[0], s_name);
    long nargc = 1;
    if (filename && filename != gensym("")) {
        atom_setsym(&a[1], filename);
        nargc = 2;
    }

    t_object *b = (t_object *)object_new_typed(CLASS_NOBOX, gensym("buffer~"), nargc, a);
    if (b) {
        outlet_bang(x->b_outlet);
    }
}

void varibuffer_append(t_varibuffer *x, t_symbol *s, long argc, t_atom *argv) {
    t_symbol *filename = (argc > 0 && atom_gettype(argv) == A_SYM) ? atom_getsym(argv) : NULL;

    if (!filename) {
        char name[MAX_FILENAME_CHARS];
        short vol;
        t_fourcc type;
        if (open_dialog(name, &vol, &type, NULL, 0) == 0) {
            char fullpath[MAX_PATH_CHARS];
            path_topathname(vol, name, fullpath);
            varibuffer_do_append(x, gensym(fullpath));
        }
    } else {
        varibuffer_do_append(x, filename);
    }
}

void varibuffer_do_appendempty(t_varibuffer *x, double duration, long channels) {
    long index = varibuffer_find_next_index(x);
    if (index < 0) return;

    char bufname[256];
    snprintf(bufname, 256, "%s.%ld", x->b_name->s_name, index);
    t_symbol *s_name = gensym(bufname);

    t_atom a[1];
    atom_setsym(&a[0], s_name);
    t_object *b = (t_object *)object_new_typed(CLASS_NOBOX, gensym("buffer~"), 1, a);
    if (b) {
        if (duration > 0) {
            t_atom sa[2];
            atom_setlong(&sa[0], (long)(duration * buffer_getsamplerate(b) / 1000.0));
            atom_setlong(&sa[1], channels);
            object_method_typed(b, gensym("sizeinsamps"), 2, sa, NULL);
        }
    }
    outlet_bang(x->b_outlet);
}

void varibuffer_appendempty(t_varibuffer *x, t_symbol *s, long argc, t_atom *argv) {
    double duration = (argc > 0) ? atom_getfloat(argv) : 0.0;
    long channels = (argc > 1) ? atom_getlong(argv+1) : 1;
    varibuffer_do_appendempty(x, duration, channels);
}

void varibuffer_getcount(t_varibuffer *x) {
    long count = 0;
    long i = 1;
    long consecutive_missing = 0;
    while (consecutive_missing < 10) {
        char bufname[256];
        snprintf(bufname, 256, "%s.%ld", x->b_name->s_name, i);
        t_buffer_ref *ref = buffer_ref_new((t_object *)x, gensym(bufname));
        if (buffer_ref_exists(ref)) {
            count++;
            consecutive_missing = 0;
        } else {
            consecutive_missing++;
        }
        object_free(ref);
        i++;
        if (i > 10000) break;
    }
    t_atom a;
    atom_setlong(&a, count);
    outlet_anything(x->b_outlet, gensym("count"), 1, &a);
}

void varibuffer_dump(t_varibuffer *x) {
    long i = 1;
    long consecutive_missing = 0;
    while (consecutive_missing < 10) {
        char bufname[256];
        snprintf(bufname, 256, "%s.%ld", x->b_name->s_name, i);
        t_symbol *s_name = gensym(bufname);
        t_buffer_ref *ref = buffer_ref_new((t_object *)x, s_name);
        if (buffer_ref_exists(ref)) {
            consecutive_missing = 0;
            t_buffer_obj *b = buffer_ref_getobject(ref);
            if (b) {
                t_buffer_info info;
                buffer_getinfo(b, &info);

                t_atom out[6];
                atom_setlong(&out[0], i);
                atom_setsym(&out[1], s_name);
                atom_setsym(&out[2], buffer_getfilename(b));
                atom_setfloat(&out[3], (double)info.b_frames * 1000.0 / info.b_sr);
                atom_setlong(&out[4], info.b_nchans);
                atom_setfloat(&out[5], info.b_sr);

                outlet_list(x->b_outlet, NULL, 6, out);
            }
        } else {
            consecutive_missing++;
        }
        object_free(ref);
        i++;
        if (i > 10000) break;
    }
}

void varibuffer_getbufferlist(t_varibuffer *x) {
    long i = 1;
    long consecutive_missing = 0;
    while (consecutive_missing < 10) {
        char bufname[256];
        snprintf(bufname, 256, "%s.%ld", x->b_name->s_name, i);
        t_symbol *s_name = gensym(bufname);
        t_buffer_ref *ref = buffer_ref_new((t_object *)x, s_name);
        if (buffer_ref_exists(ref)) {
            consecutive_missing = 0;
            t_atom a;
            atom_setsym(&a, s_name);
            outlet_anything(x->b_outlet, gensym("bufferlist"), 1, &a);
        } else {
            consecutive_missing++;
        }
        object_free(ref);
        i++;
        if (i > 10000) break;
    }
    outlet_anything(x->b_outlet, gensym("done"), 0, NULL);
}

void varibuffer_getshortname(t_varibuffer *x) {
    long i = 1;
    long consecutive_missing = 0;
    while (consecutive_missing < 10) {
        char bufname[256];
        snprintf(bufname, 256, "%s.%ld", x->b_name->s_name, i);
        t_symbol *s_name = gensym(bufname);
        t_buffer_ref *ref = buffer_ref_new((t_object *)x, s_name);
        if (buffer_ref_exists(ref)) {
            consecutive_missing = 0;
            t_buffer_obj *b = buffer_ref_getobject(ref);
            if (b) {
                t_atom out[2];
                atom_setsym(&out[0], s_name);
                t_symbol *fname = buffer_getfilename(b);
                char nameonly[256];
                strncpy(nameonly, fname->s_name, 256);
                char *dot = strrchr(nameonly, '.');
                if (dot) *dot = '\0';
                atom_setsym(&out[1], gensym(nameonly));
                outlet_list(x->b_outlet, NULL, 2, out);
            }
        } else {
            consecutive_missing++;
        }
        object_free(ref);
        i++;
        if (i > 10000) break;
    }
    outlet_anything(x->b_outlet, gensym("done"), 0, NULL);
}

void varibuffer_getsize(t_varibuffer *x) {
    long total_size = 0;
    long i = 1;
    long consecutive_missing = 0;
    while (consecutive_missing < 10) {
        char bufname[256];
        snprintf(bufname, 256, "%s.%ld", x->b_name->s_name, i);
        t_buffer_ref *ref = buffer_ref_new((t_object *)x, gensym(bufname));
        if (buffer_ref_exists(ref)) {
            consecutive_missing = 0;
            t_buffer_obj *b = buffer_ref_getobject(ref);
            if (b) {
                t_buffer_info info;
                buffer_getinfo(b, &info);
                total_size += info.b_frames * info.b_nchans * sizeof(float);
            }
        } else {
            consecutive_missing++;
        }
        object_free(ref);
        i++;
        if (i > 10000) break;
    }
    t_atom a;
    atom_setlong(&a, total_size);
    outlet_anything(x->b_outlet, gensym("size"), 1, &a);
}

void varibuffer_send(t_varibuffer *x, t_symbol *s, long argc, t_atom *argv) {
    if (argc < 2) return;
    long index = atom_getlong(argv);
    t_symbol *msg = atom_getsym(argv+1);

    if (index == 0) {
        long i = 1;
        long consecutive_missing = 0;
        while (consecutive_missing < 10) {
            char bufname[256];
            snprintf(bufname, 256, "%s.%ld", x->b_name->s_name, i);
            t_buffer_ref *ref = buffer_ref_new((t_object *)x, gensym(bufname));
            if (buffer_ref_exists(ref)) {
                consecutive_missing = 0;
                t_buffer_obj *b = buffer_ref_getobject(ref);
                if (b) object_method_typed(b, msg, argc-2, argv+2, NULL);
            } else {
                consecutive_missing++;
            }
            object_free(ref);
            i++;
            if (i > 10000) break;
        }
    } else {
        char bufname[256];
        snprintf(bufname, 256, "%s.%ld", x->b_name->s_name, index);
        t_buffer_ref *ref = buffer_ref_new((t_object *)x, gensym(bufname));
        if (buffer_ref_exists(ref)) {
            t_buffer_obj *b = buffer_ref_getobject(ref);
            if (b) object_method_typed(b, msg, argc-2, argv+2, NULL);
        }
        object_free(ref);
    }
}

void varibuffer_open(t_varibuffer *x) {
    long i = 1;
    char bufname[256];
    snprintf(bufname, 256, "%s.%ld", x->b_name->s_name, i);
    t_buffer_ref *ref = buffer_ref_new((t_object *)x, gensym(bufname));
    if (buffer_ref_exists(ref)) {
        t_buffer_obj *b = buffer_ref_getobject(ref);
        if (b) buffer_view(b);
    }
    object_free(ref);
}

void varibuffer_wclose(t_varibuffer *x) {
    // Placeholder
}

void varibuffer_readfolder(t_varibuffer *x, t_symbol *s, long argc, t_atom *argv) {
    defer_low(x, (method)varibuffer_readfolder_deferred, s, (short)argc, argv);
}

void varibuffer_readfolder_deferred(t_varibuffer *x, t_symbol *s, long argc, t_atom *argv) {
    char path[MAX_PATH_CHARS];
    short vol = 0;

    if (argc > 0 && atom_gettype(argv) == A_SYM) {
        strncpy_zero(path, atom_getsym(argv)->s_name, MAX_PATH_CHARS);
        if (path_frompathname(path, &vol, NULL)) {
            object_error((t_object *)x, "readfolder: invalid path %s", path);
            return;
        }
    } else {
        if (getfolder(&vol)) return;
    }

    void *fd = path_openfolder(vol);
    if (fd) {
        t_fourcc type;
        char name[MAX_FILENAME_CHARS];
        while (path_foldernextfile(fd, &type, name, 0) == 0) {
            // We should ideally check if it's an audio file.
            // For now, try to append it.
            char fullpath[MAX_PATH_CHARS];
            path_topathname(vol, name, fullpath);
            varibuffer_do_append(x, gensym(fullpath));
        }
        path_closefolder(fd);
    }
}

void varibuffer_writetofolder(t_varibuffer *x, t_symbol *s, long argc, t_atom *argv) {
    // Placeholder
}

void varibuffer_dblclick(t_varibuffer *x) {
    varibuffer_open(x);
}

void varibuffer_clear(t_varibuffer *x) {
    long i = 1;
    long consecutive_missing = 0;
    while (consecutive_missing < 10) {
        char bufname[256];
        snprintf(bufname, 256, "%s.%ld", x->b_name->s_name, i);
        t_symbol *s_name = gensym(bufname);
        t_object *b = (t_object *)object_findregistered(CLASS_NOBOX, s_name);

        if (b) {
            object_free(b);
            consecutive_missing = 0;
        } else {
            consecutive_missing++;
        }
        i++;
        if (i > 10000) break;
    }
    outlet_bang(x->b_outlet);
}
