#include "ext.h"
#include "ext_obex.h"
#include "ext_dictobj.h"

// Struct for the crucible object
typedef struct _crucible {
    t_object s_obj;
    t_symbol *dict_name;
    void *outlet_palette;
    void *outlet_bar;
    void *outlet_span_max;
    void *verbose_log_outlet;
    long verbose;
} t_crucible;

// Function prototypes
void *crucible_new(t_symbol *s, long argc, t_atom *argv);
void crucible_free(t_crucible *x);
void crucible_anything(t_crucible *x, t_symbol *s, long argc, t_atom *argv);
void crucible_assist(t_crucible *x, void *b, long m, long a, char *s);
void crucible_bang(t_crucible *x);
void crucible_process_dictionary(t_crucible *x, t_symbol *dict_name);
void crucible_verbose_log(t_crucible *x, const char *fmt, ...);

// Global class pointer
t_class *crucible_class;

// Helper function to send verbose log messages
void crucible_verbose_log(t_crucible *x, const char *fmt, ...) {
    if (x->verbose && x->verbose_log_outlet) {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, 1024, fmt, args);
        va_end(args);
        outlet_anything(x->verbose_log_outlet, gensym(buf), 0, NULL);
    }
}

void ext_main(void *r) {
    t_class *c;
    c = class_new("crucible", (method)crucible_new, (method)crucible_free, (short)sizeof(t_crucible), 0L, A_GIMME, 0);
    class_addmethod(c, (method)crucible_anything, "anything", A_GIMME, 0);
    class_addmethod(c, (method)crucible_bang, "bang", 0);
    class_addmethod(c, (method)crucible_assist, "assist", A_CANT, 0);

    CLASS_ATTR_LONG(c, "verbose", 0, t_crucible, verbose);
    CLASS_ATTR_STYLE_LABEL(c, "verbose", 0, "onoff", "Enable Verbose Logging");
    CLASS_ATTR_DEFAULT(c, "verbose", 0, "0");

    class_register(CLASS_BOX, c);
    crucible_class = c;
}

void *crucible_new(t_symbol *s, long argc, t_atom *argv) {
    t_crucible *x = (t_crucible *)object_alloc(crucible_class);
    if (x) {
        x->dict_name = gensym("");
        x->verbose_log_outlet = NULL;

        // Process arguments: first is dictionary name
        if (argc > 0 && atom_gettype(argv) == A_SYM) {
            x->dict_name = atom_getsym(argv);
        }

        // Process attributes
        attr_args_process(x, argc, argv);

        // Outlets are created from right to left
        if (x->verbose) {
            x->verbose_log_outlet = outlet_new((t_object *)x, NULL);
        }
        x->outlet_span_max = outlet_new((t_object *)x, NULL);
        x->outlet_bar = outlet_new((t_object *)x, NULL);
        x->outlet_palette = outlet_new((t_object *)x, NULL);
    }
    return (x);
}

void crucible_free(t_crucible *x) {
    // No special cleanup needed
}

void crucible_anything(t_crucible *x, t_symbol *s, long argc, t_atom *argv) {
    if (s == gensym("dictionary") && argc > 0 && atom_gettype(argv) == A_SYM) {
        crucible_process_dictionary(x, atom_getsym(argv));
    }
}

void crucible_bang(t_crucible *x) {
    if (x->dict_name && x->dict_name->s_name[0]) {
        crucible_process_dictionary(x, x->dict_name);
    }
}

void crucible_process_dictionary(t_crucible *x, t_symbol *dict_name) {
    t_dictionary *d = dictobj_findregistered_retain(dict_name);
    if (!d) {
        object_error((t_object *)x, "could not find dictionary %s", dict_name->s_name);
        return;
    }

    t_atom palette_atom;
    if (dictionary_hasentry(d, gensym("palette"))) {
        dictionary_getatom(d, gensym("palette"), &palette_atom);
        outlet_anything(x->outlet_palette, atom_getsym(&palette_atom), 0, NULL);
    }

    t_atom bar_atom;
    if (dictionary_hasentry(d, gensym("bar"))) {
        dictionary_getatom(d, gensym("bar"), &bar_atom);
        outlet_int(x->outlet_bar, atom_getlong(&bar_atom));
    }

    if (dictionary_hasentry(d, gensym("span"))) {
        t_atom span_atom_ref;
        dictionary_getatom(d, gensym("span"), &span_atom_ref);

        t_object *obj = atom_getobj(&span_atom_ref);
        if (object_classname(obj) == gensym("atomarray")) {
            t_atomarray *span_array = (t_atomarray *)obj;
            long count;
            t_atom *atoms;
            atomarray_getatoms(span_array, &count, &atoms);

            if (count > 0) {
                long max_val = atom_getlong(atoms);
                for (long i = 1; i < count; i++) {
                    long current_val = atom_getlong(atoms + i);
                    if (current_val > max_val) {
                        max_val = current_val;
                    }
                }
                outlet_int(x->outlet_span_max, max_val);
            }
        }
    }

    object_release((t_object *)d);
}

void crucible_assist(t_crucible *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "dictionary <name>, bang");
    } else { // ASSIST_OUTLET
        if (x->verbose) {
            switch (a) {
                case 0: sprintf(s, "Palette (symbol)"); break;
                case 1: sprintf(s, "Bar (int)"); break;
                case 2: sprintf(s, "Largest Value in Span (int)"); break;
                case 3: sprintf(s, "Verbose Logging Outlet"); break;
            }
        } else {
             switch (a) {
                case 0: sprintf(s, "Palette (symbol)"); break;
                case 1: sprintf(s, "Bar (int)"); break;
                case 2: sprintf(s, "Largest Value in Span (int)"); break;
            }
        }
    }
}
