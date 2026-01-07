#include "ext.h"
#include "ext_obex.h"
#include <string.h> // For strcmp, strstr, etc.

// Struct for the crucible object
typedef struct _crucible {
    t_object s_obj;
    void *outlet_reach;
    void *outlet_palette;
    void *outlet_bar;
    void *outlet_offset;
    void *verbose_log_outlet;
    long bar_length;
    long verbose;
} t_crucible;

// Function prototypes
void *crucible_new(t_symbol *s, long argc, t_atom *argv);
void crucible_free(t_crucible *x);
void crucible_anything(t_crucible *x, t_symbol *s, long argc, t_atom *argv);
void crucible_in1(t_crucible *x, long n);
void crucible_assist(t_crucible *x, void *b, long m, long a, char *s);
void crucible_verbose_log(t_crucible *x, const char *fmt, ...);
int parse_selector(const char *selector_str, char **bar, char **key);

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

// Helper to parse selectors like "track::bar::key"
int parse_selector(const char *selector_str, char **bar, char **key) {
    const char *first_delim = strstr(selector_str, "::");
    if (!first_delim) return 0;

    const char *second_delim = strstr(first_delim + 2, "::");
    if (!second_delim) return 0;

    size_t bar_len = second_delim - (first_delim + 2);
    *bar = (char *)sysmem_newptr(bar_len + 1);
    if (!*bar) return 0;
    strncpy(*bar, first_delim + 2, bar_len);
    (*bar)[bar_len] = '\0';

    *key = (char *)sysmem_newptr(strlen(second_delim + 2) + 1);
    if (!*key) {
        sysmem_freeptr(*bar);
        return 0;
    }
    strcpy(*key, second_delim + 2);

    return 1;
}


void ext_main(void *r) {
    t_class *c;
    c = class_new("crucible", (method)crucible_new, (method)crucible_free, (short)sizeof(t_crucible), 0L, A_GIMME, 0);
    class_addmethod(c, (method)crucible_anything, "anything", A_GIMME, 0);
    class_addmethod(c, (method)crucible_in1, "in1", A_LONG, 0);
    class_addmethod(c, (method)crucible_assist, "assist", A_CANT, 0);

    CLASS_ATTR_LONG(c, "verbose", 0, t_crucible, verbose);
    CLASS_ATTR_STYLE_LABEL(c, "verbose", 0, "onoff", "Enable Verbose Logging");

    class_register(CLASS_BOX, c);
    crucible_class = c;
}

void *crucible_new(t_symbol *s, long argc, t_atom *argv) {
    t_crucible *x = (t_crucible *)object_alloc(crucible_class);
    if (x) {
        x->verbose_log_outlet = NULL;
        x->bar_length = 125; // Default bar length

        // Process attributes
        attr_args_process(x, argc, argv);

        // Inlets are created from right to left
        intin((t_object *)x, 1); // Inlet for bar_length

        // Outlets are created from right to left
        if (x->verbose) {
            x->verbose_log_outlet = outlet_new((t_object *)x, NULL);
        }
        x->outlet_offset = outlet_new((t_object *)x, NULL);
        x->outlet_bar = outlet_new((t_object *)x, NULL);
        x->outlet_palette = outlet_new((t_object *)x, NULL);
        x->outlet_reach = outlet_new((t_object *)x, NULL);
    }
    return (x);
}

void crucible_free(t_crucible *x) {
    // No special cleanup needed
}

void crucible_in1(t_crucible *x, long n) {
    x->bar_length = n;
    crucible_verbose_log(x, "Bar length set to: %ld", n);
}

void crucible_anything(t_crucible *x, t_symbol *s, long argc, t_atom *argv) {
    char *bar_str = NULL;
    char *key_str = NULL;

    if (parse_selector(s->s_name, &bar_str, &key_str)) {
        crucible_verbose_log(x, "Parsed selector: bar='%s', key='%s'", bar_str, key_str);

        if (strcmp(key_str, "offset") == 0) {
            if (argc > 0) {
                 if (atom_gettype(argv) == A_LONG) {
                    outlet_int(x->outlet_offset, atom_getlong(argv));
                } else if (atom_gettype(argv) == A_FLOAT) {
                    outlet_int(x->outlet_offset, (long)atom_getfloat(argv));
                }
            }
        } else if (strcmp(key_str, "palette") == 0) {
            if (argc > 0 && atom_gettype(argv) == A_SYM) {
                outlet_anything(x->outlet_palette, atom_getsym(argv), 0, NULL);
            }
        } else if (strcmp(key_str, "span") == 0) {
            long bar_val = atol(bar_str);

            if (argc > 0) {
                long max_val = 0;
                if (atom_gettype(argv) == A_LONG) {
                    max_val = atom_getlong(argv);
                } else if (atom_gettype(argv) == A_FLOAT) {
                    max_val = (long)atom_getfloat(argv);
                }

                for (long i = 1; i < argc; i++) {
                    long current_val = 0;
                     if (atom_gettype(argv+i) == A_LONG) {
                        current_val = atom_getlong(argv+i);
                    } else if (atom_gettype(argv+i) == A_FLOAT) {
                        current_val = (long)atom_getfloat(argv+i);
                    }
                    if (current_val > max_val) {
                        max_val = current_val;
                    }
                }

                // Output in right-to-left order
                outlet_int(x->outlet_bar, bar_val);
                outlet_int(x->outlet_reach, max_val + x->bar_length);
            }
        }

        sysmem_freeptr(bar_str);
        sysmem_freeptr(key_str);
    } else {
        crucible_verbose_log(x, "Unparsable message selector: %s", s->s_name);
    }
}

void crucible_assist(t_crucible *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        switch (a) {
            case 0: sprintf(s, "(anything) Message Stream from buildspans"); break;
            case 1: sprintf(s, "(int) Bar Length in ms"); break;
        }
    } else { // ASSIST_OUTLET
        if (x->verbose) {
            switch (a) {
                case 0: sprintf(s, "Reach (int)"); break;
                case 1: sprintf(s, "Palette (symbol)"); break;
                case 2: sprintf(s, "Bar (int)"); break;
                case 3: sprintf(s, "Offset (int)"); break;
                case 4: sprintf(s, "Verbose Logging Outlet"); break;
            }
        } else {
             switch (a) {
                case 0: sprintf(s, "Reach (int)"); break;
                case 1: sprintf(s, "Palette (symbol)"); break;
                case 2: sprintf(s, "Bar (int)"); break;
                case 3: sprintf(s, "Offset (int)"); break;
            }
        }
    }
}
