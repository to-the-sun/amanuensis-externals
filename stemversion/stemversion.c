#include "ext.h"
#include "ext_obex.h"
#include <time.h>

typedef struct _stemversion {
    t_object s_obj;
    void *outlet;
    long verbose;
    void *verbose_log_outlet;
} t_stemversion;

void *stemversion_new(t_symbol *s, long argc, t_atom *argv);
void stemversion_bang(t_stemversion *x);
void stemversion_assist(t_stemversion *x, void *b, long m, long a, char *s);

t_class *stemversion_class;

void stemversion_verbose_log(t_stemversion *x, const char *fmt, ...) {
    if (x->verbose && x->verbose_log_outlet) {
        char buf[1024];
        char final_buf[1100];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, 1024, fmt, args);
        va_end(args);
        snprintf(final_buf, 1100, "stemversion: %s", buf);
        outlet_anything(x->verbose_log_outlet, gensym(final_buf), 0, NULL);
    }
}

void ext_main(void *r) {
    common_symbols_init();
    t_class *c;

    c = class_new("stemversion", (method)stemversion_new, (method)NULL, sizeof(t_stemversion), 0L, A_GIMME, 0);
    class_addmethod(c, (method)stemversion_bang, "bang", 0);
    class_addmethod(c, (method)stemversion_assist, "assist", A_CANT, 0);

    CLASS_ATTR_LONG(c, "verbose", 0, t_stemversion, verbose);
    CLASS_ATTR_STYLE_LABEL(c, "verbose", 0, "onoff", "Enable Verbose Logging");

    class_register(CLASS_BOX, c);
    stemversion_class = c;
}

void *stemversion_new(t_symbol *s, long argc, t_atom *argv) {
    t_stemversion *x = (t_stemversion *)object_alloc(stemversion_class);
    if (x) {
        x->verbose = 0;
        x->verbose_log_outlet = outlet_new((t_object *)x, NULL);
        x->outlet = outlet_new((t_object *)x, "symbol");

        attr_args_process(x, argc, argv);
    }
    return (x);
}

void stemversion_bang(t_stemversion *x) {
    time_t rawtime;
    struct tm *timeinfo;
    char final_symbol_str[80];

    time(&rawtime);
    timeinfo = localtime(&rawtime);

    snprintf(final_symbol_str, sizeof(final_symbol_str), "[%d-%d-%d-%d-%d-%d]",
             timeinfo->tm_year + 1900,
             timeinfo->tm_mon + 1,
             timeinfo->tm_mday,
             timeinfo->tm_hour,
             timeinfo->tm_min,
             timeinfo->tm_sec);

    outlet_anything(x->outlet, gensym(final_symbol_str), 0, NULL);
}

void stemversion_assist(t_stemversion *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "(bang) Output Current Timestamp");
    } else { // ASSIST_OUTLET
        switch (a) {
            case 0: sprintf(s, "Timestamp Symbol (e.g., [2025-12-8-15-16-16])"); break;
            case 1: sprintf(s, "Verbose Logging Outlet"); break;
        }
    }
}
