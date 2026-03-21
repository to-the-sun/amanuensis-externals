#include "ext.h"
#include "ext_obex.h"
#include "../shared/logging.h"
#include <time.h>

typedef struct _stemversion {
    t_object s_obj;
    void *outlet;
    long log;
    void *log_outlet;
    t_symbol *format;
} t_stemversion;

void *stemversion_new(t_symbol *s, long argc, t_atom *argv);
void stemversion_bang(t_stemversion *x);
void stemversion_assist(t_stemversion *x, void *b, long m, long a, char *s);
void stemversion_log(t_stemversion *x, const char *fmt, ...);

t_class *stemversion_class;

void ext_main(void *r) {
    t_class *c;

    common_symbols_init();

    c = class_new("stemversion", (method)stemversion_new, (method)NULL, sizeof(t_stemversion), 0L, A_GIMME, 0);
    class_addmethod(c, (method)stemversion_bang, "bang", 0);
    class_addmethod(c, (method)stemversion_assist, "assist", A_CANT, 0);

    CLASS_ATTR_LONG(c, "log", 0, t_stemversion, log);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "onoff", "Enable Logging");
    CLASS_ATTR_DEFAULT(c, "log", 0, "0");

    CLASS_ATTR_SYM(c, "format", 0, t_stemversion, format);
    CLASS_ATTR_ENUM(c, "format", 0, "default live");
    CLASS_ATTR_LABEL(c, "format", 0, "Timestamp Format");
    CLASS_ATTR_DEFAULT(c, "format", 0, "default");

    class_register(CLASS_BOX, c);
    stemversion_class = c;
}

void *stemversion_new(t_symbol *s, long argc, t_atom *argv) {
    t_stemversion *x = (t_stemversion *)object_alloc(stemversion_class);
    if (x) {
        x->log = 0;
        x->log_outlet = NULL;
        x->format = gensym("default");

        attr_args_process(x, argc, argv);

        x->log_outlet = outlet_new((t_object *)x, NULL);
        x->outlet = outlet_new((t_object *)x, "symbol");
    }
    return (x);
}

void stemversion_log(t_stemversion *x, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vcommon_log(x->log_outlet, x->log, "stemversion", fmt, args);
    va_end(args);
}

void stemversion_bang(t_stemversion *x) {
    time_t rawtime;
    struct tm *timeinfo;
    char final_symbol_str[80];

    time(&rawtime);
    timeinfo = localtime(&rawtime);

    if (x->format == gensym("live")) {
        snprintf(final_symbol_str, sizeof(final_symbol_str), "[%04d-%02d-%02d %02d%02d%02d]",
                 timeinfo->tm_year + 1900,
                 timeinfo->tm_mon + 1,
                 timeinfo->tm_mday,
                 timeinfo->tm_hour,
                 timeinfo->tm_min,
                 timeinfo->tm_sec);
    } else {
        snprintf(final_symbol_str, sizeof(final_symbol_str), "[%d-%d-%d-%d-%d-%d]",
                 timeinfo->tm_year + 1900,
                 timeinfo->tm_mon + 1,
                 timeinfo->tm_mday,
                 timeinfo->tm_hour,
                 timeinfo->tm_min,
                 timeinfo->tm_sec);
    }

    outlet_anything(x->outlet, gensym(final_symbol_str), 0, NULL);
}

void stemversion_assist(t_stemversion *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "Inlet 1: (bang) Output Current Timestamp");
    } else { // ASSIST_OUTLET
        switch (a) {
            case 0:
                if (x->format == gensym("live")) {
                    sprintf(s, "Outlet 1: Timestamp Symbol (e.g., [2025-12-08 151616])");
                } else {
                    sprintf(s, "Outlet 1: Timestamp Symbol (e.g., [2025-12-8-15-16-16])");
                }
                break;
            case 1: sprintf(s, "Outlet 2: Logging Outlet"); break;
        }
    }
}
