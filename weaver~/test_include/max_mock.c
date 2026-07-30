#include "max_mock.h"
#include <unistd.h>
#include <sys/time.h>
#include <stdarg.h>

// Global variables
t_symbol * _sym_nothing = NULL;

// Symbol registry
#define MAX_SYMBOLS 1024
static t_symbol g_symbols[MAX_SYMBOLS];
static int g_symbol_count = 0;

t_symbol *gensym(const char *s) {
    if (!s) s = "";
    for (int i = 0; i < g_symbol_count; i++) {
        if (strcmp(g_symbols[i].s_name, s) == 0) {
            return &g_symbols[i];
        }
    }
    if (g_symbol_count >= MAX_SYMBOLS) {
        fprintf(stderr, "MAX_SYMBOLS limit reached!\n");
        return NULL;
    }
    t_symbol *sym = &g_symbols[g_symbol_count++];
    sym->s_name = strdup(s);
    sym->s_thing = NULL;
    return sym;
}

void common_symbols_init(void) {
    _sym_nothing = gensym("");
}

// Memory allocation
void *sysmem_newptr(size_t size) {
    return calloc(1, size);
}

void sysmem_freeptr(void *ptr) {
    if (ptr) free(ptr);
}

void *sysmem_resizeptr(void *ptr, size_t size) {
    return realloc(ptr, size);
}

// Class definitions
t_class *class_new(const char *name, method newmethod, method freemethod, size_t size, method menu_or_dummy, int type, ...) {
    t_class *c = (t_class *)calloc(1, sizeof(t_class));
    c->name = strdup(name);
    return c;
}

void class_addmethod(t_class *c, method m, const char *name, ...) {}
void class_dspinit(t_class *c) {}
void class_register(int type, t_class *c) {}

// Outlets
void *outlet_new(void *ob, const char *classname) {
    // Return an allocated pointer that can be safely freed
    return calloc(1, 16);
}

// Global log file pointer for test output redirect
extern FILE *g_log_file;

void outlet_anything(void *outlet, t_symbol *s, short argc, t_atom *argv) {
    if (g_log_file) {
        fprintf(g_log_file, "[OUTLET verbose] %s", s->s_name);
        for (int i = 0; i < argc; i++) {
            if (atom_gettype(&argv[i]) == A_LONG) {
                fprintf(g_log_file, " %lld", (long long)atom_getlong(&argv[i]));
            } else if (atom_gettype(&argv[i]) == A_FLOAT) {
                fprintf(g_log_file, " %f", atom_getfloat(&argv[i]));
            } else if (atom_gettype(&argv[i]) == A_SYM) {
                fprintf(g_log_file, " %s", atom_getsym(&argv[i])->s_name);
            }
        }
        fprintf(g_log_file, "\n");
        fflush(g_log_file);
    }
}

void outlet_list(void *outlet, t_symbol *s, short argc, t_atom *argv) {
    if (g_log_file) {
        fprintf(g_log_file, "[OUTLET list]");
        for (int i = 0; i < argc; i++) {
            if (atom_gettype(&argv[i]) == A_LONG) {
                fprintf(g_log_file, " %lld", (long long)atom_getlong(&argv[i]));
            } else if (atom_gettype(&argv[i]) == A_FLOAT) {
                fprintf(g_log_file, " %f", atom_getfloat(&argv[i]));
            } else if (atom_gettype(&argv[i]) == A_SYM) {
                fprintf(g_log_file, " %s", atom_getsym(&argv[i])->s_name);
            }
        }
        fprintf(g_log_file, "\n");
        fflush(g_log_file);
    }
}

void outlet_int(void *outlet, t_atom_long n) {
    if (g_log_file) {
        fprintf(g_log_file, "[OUTLET int] %lld\n", (long long)n);
        fflush(g_log_file);
    }
}

void outlet_bang(void *outlet) {
    if (g_log_file) {
        fprintf(g_log_file, "[OUTLET bang]\n");
        fflush(g_log_file);
    }
}

// Object Allocation
void *object_alloc(t_class *c) {
    // Allocates maximum possible size needed for weaver struct (e.g. 10MB to be totally safe)
    void *x = calloc(1, 1024 * 1024);
    ((t_object *)x)->o_class = c;
    return x;
}

void object_free(void *x) {
    if (x) free(x);
}

void object_warn(void *x, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (g_log_file) {
        fprintf(g_log_file, "[WARN] ");
        vfprintf(g_log_file, fmt, args);
        fprintf(g_log_file, "\n");
    } else {
        printf("[WARN] ");
        vprintf(fmt, args);
        printf("\n");
    }
    va_end(args);
}

// Redefine to avoid clash in link
void object_error(void *x, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (g_log_file) {
        fprintf(g_log_file, "[ERROR] ");
        vfprintf(g_log_file, fmt, args);
        fprintf(g_log_file, "\n");
    } else {
        printf("[ERROR] ");
        vprintf(fmt, args);
        printf("\n");
    }
    va_end(args);
}

void object_post(void *x, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (g_log_file) {
        fprintf(g_log_file, "[POST] ");
        vfprintf(g_log_file, fmt, args);
        fprintf(g_log_file, "\n");
    } else {
        printf("[POST] ");
        vprintf(fmt, args);
        printf("\n");
    }
    va_end(args);
}

// Proxies
void *proxy_new(void *x, long id, long *proxy_id) {
    *proxy_id = id;
    return calloc(1, 16);
}

long g_mock_inlet = 0;
long proxy_getinlet(void *x) {
    return g_mock_inlet;
}

// Qelem
t_qelem *qelem_new(void *obj, method fn) {
    t_qelem *q = (t_qelem *)calloc(1, sizeof(t_qelem));
    q->obj = obj;
    q->fn = fn;
    return q;
}

void qelem_set(t_qelem *q) {
    if (q) q->set = 1;
}

void qelem_free(t_qelem *q) {
    if (q) free(q);
}

// Critical sections using pthread mutex
void critical_new(t_critical *lock) {
    *lock = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(*lock, NULL);
}

void critical_enter(t_critical lock) {
    if (lock) pthread_mutex_lock(lock);
}

void critical_exit(t_critical lock) {
    if (lock) pthread_mutex_unlock(lock);
}

void critical_free(t_critical lock) {
    if (lock) {
        pthread_mutex_destroy(lock);
        free(lock);
    }
}

t_max_err critical_tryenter(t_critical lock) {
    if (lock && pthread_mutex_trylock(lock) == 0) {
        return MAX_ERR_NONE;
    }
    return MAX_ERR_GENERIC;
}

// Threading using POSIX threads
int systhread_create(method fn, void *arg, size_t stacksize, int priority, int flags, t_systhread *thread) {
    return pthread_create(thread, NULL, (void *(*)(void *))fn, arg) == 0 ? 0 : 1;
}

void systhread_join(t_systhread thread, unsigned int *retval) {
    void *ret;
    pthread_join(thread, &ret);
    if (retval) *retval = (unsigned int)(uintptr_t)ret;
}

void systhread_sleep(unsigned int ms) {
    usleep(ms * 1000);
}

t_systhread systhread_self(void) {
    return pthread_self();
}

void systhread_set_name(const char *name) {}

void systhread_exit(unsigned int retval) {
    pthread_exit((void *)(uintptr_t)retval);
}

// Mutexes
void systhread_mutex_new(t_systhread_mutex *mutex, int flags) {
    *mutex = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(*mutex, NULL);
}

void systhread_mutex_free(t_systhread_mutex mutex) {
    if (mutex) {
        pthread_mutex_destroy(mutex);
        free(mutex);
    }
}

void systhread_mutex_lock(t_systhread_mutex mutex) {
    if (mutex) pthread_mutex_lock(mutex);
}

void systhread_mutex_unlock(t_systhread_mutex mutex) {
    if (mutex) pthread_mutex_unlock(mutex);
}

// Condition variables
void systhread_cond_new(t_systhread_cond *cond, int flags) {
    *cond = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
    pthread_cond_init(*cond, NULL);
}

void systhread_cond_free(t_systhread_cond cond) {
    if (cond) {
        pthread_cond_destroy(cond);
        free(cond);
    }
}

void systhread_cond_wait(t_systhread_cond cond, t_systhread_mutex mutex) {
    if (cond && mutex) pthread_cond_wait(cond, mutex);
}

void systhread_cond_signal(t_systhread_cond cond) {
    if (cond) pthread_cond_signal(cond);
}

// Global dictionary/registration registry
#define MAX_REG_DICTS 128
static struct {
    t_symbol *name;
    t_dictionary *dict;
} g_registered_dicts[MAX_REG_DICTS];
static int g_registered_dict_count = 0;

t_dictionary *dictionary_new(void) {
    t_dictionary *d = (t_dictionary *)calloc(1, sizeof(t_dictionary));
    return d;
}

t_max_err dictionary_getkeys(t_dictionary *d, long *numkeys, t_symbol ***keys) {
    if (!d) return MAX_ERR_GENERIC;
    *numkeys = d->count;
    *keys = (t_symbol **)calloc(d->count, sizeof(t_symbol *));
    for (int i = 0; i < d->count; i++) {
        (*keys)[i] = gensym(d->keys[i]);
    }
    return MAX_ERR_NONE;
}

t_max_err dictionary_getdictionary(t_dictionary *d, t_symbol *key, t_object **val) {
    if (!d || !key) return MAX_ERR_GENERIC;
    for (int i = 0; i < d->count; i++) {
        if (strcmp(d->keys[i], key->s_name) == 0) {
            if (d->values[i].a_type == A_OBJ) {
                *val = d->values[i].a_w.w_obj;
                return MAX_ERR_NONE;
            }
        }
    }
    return MAX_ERR_GENERIC;
}

t_max_err dictionary_getatom(t_dictionary *d, t_symbol *key, t_atom *val) {
    if (!d || !key) return MAX_ERR_GENERIC;
    for (int i = 0; i < d->count; i++) {
        if (strcmp(d->keys[i], key->s_name) == 0) {
            *val = d->values[i];
            return MAX_ERR_NONE;
        }
    }
    return MAX_ERR_GENERIC;
}

t_max_err dictionary_getatomarray(t_dictionary *d, t_symbol *key, t_object **val) {
    if (!d || !key) return MAX_ERR_GENERIC;
    for (int i = 0; i < d->count; i++) {
        if (strcmp(d->keys[i], key->s_name) == 0) {
            if (d->values[i].a_type == A_OBJ) {
                *val = d->values[i].a_w.w_obj;
                return MAX_ERR_NONE;
            }
        }
    }
    return MAX_ERR_GENERIC;
}

void dictionary_clear(t_dictionary *d) {
    if (!d) return;
    for (int i = 0; i < d->count; i++) {
        free(d->keys[i]);
    }
    free(d->keys);
    free(d->values);
    d->keys = NULL;
    d->values = NULL;
    d->count = 0;
    d->capacity = 0;
}

t_max_err dictionary_appenddictionary(t_dictionary *d, t_symbol *key, t_object *val) {
    if (!d || !key) return MAX_ERR_GENERIC;
    t_atom a;
    a.a_type = A_OBJ;
    a.a_w.w_obj = val;
    return dictionary_appendatom(d, key, &a);
}

t_max_err dictionary_appendatomarray(t_dictionary *d, t_symbol *key, t_object *val) {
    if (!d || !key) return MAX_ERR_GENERIC;
    t_atom a;
    a.a_type = A_OBJ;
    a.a_w.w_obj = val;
    return dictionary_appendatom(d, key, &a);
}

t_max_err dictionary_appendatom(t_dictionary *d, t_symbol *key, t_atom *val) {
    if (!d || !key) return MAX_ERR_GENERIC;
    // Overwrite existing key if present
    for (int i = 0; i < d->count; i++) {
        if (strcmp(d->keys[i], key->s_name) == 0) {
            d->values[i] = *val;
            return MAX_ERR_NONE;
        }
    }
    if (d->count >= d->capacity) {
        d->capacity = d->capacity == 0 ? 8 : d->capacity * 2;
        d->keys = (char **)realloc(d->keys, d->capacity * sizeof(char *));
        d->values = (t_atom *)realloc(d->values, d->capacity * sizeof(t_atom));
    }
    d->keys[d->count] = strdup(key->s_name);
    d->values[d->count] = *val;
    d->count++;
    return MAX_ERR_NONE;
}

t_max_err dictionary_appendlong(t_dictionary *d, t_symbol *key, t_atom_long val) {
    t_atom a;
    a.a_type = A_LONG;
    a.a_w.w_long = (long)val;
    return dictionary_appendatom(d, key, &a);
}

int dictionary_hasentry(t_dictionary *d, t_symbol *key) {
    if (!d || !key) return 0;
    for (int i = 0; i < d->count; i++) {
        if (strcmp(d->keys[i], key->s_name) == 0) return 1;
    }
    return 0;
}

t_max_err dictionary_deleteentry(t_dictionary *d, t_symbol *key) {
    if (!d || !key) return MAX_ERR_GENERIC;
    for (int i = 0; i < d->count; i++) {
        if (strcmp(d->keys[i], key->s_name) == 0) {
            free(d->keys[i]);
            // Shift remaining
            for (int j = i; j < d->count - 1; j++) {
                d->keys[j] = d->keys[j + 1];
                d->values[j] = d->values[j + 1];
            }
            d->count--;
            return MAX_ERR_NONE;
        }
    }
    return MAX_ERR_GENERIC;
}

long dictionary_getentrycount(t_dictionary *d) {
    return d ? d->count : 0;
}

void object_release(t_object *obj) {
    // In actual Max, dictionary/atomarray ownership/releases can be complex.
    // In our mock we can safely leave freeing to custom cleanup or keep it simple.
}

void object_retain(t_object *obj) {}

t_dictionary *dictobj_findregistered_retain(t_symbol *name) {
    for (int i = 0; i < g_registered_dict_count; i++) {
        if (g_registered_dicts[i].name == name) {
            return g_registered_dicts[i].dict;
        }
    }
    return NULL;
}

void dictobj_release(t_dictionary *d) {}

void mock_register_dict(const char *name, t_dictionary *dict) {
    t_symbol *sym = gensym(name);
    // Overwrite existing registration if present
    for (int i = 0; i < g_registered_dict_count; i++) {
        if (g_registered_dicts[i].name == sym) {
            g_registered_dicts[i].dict = dict;
            return;
        }
    }
    if (g_registered_dict_count < MAX_REG_DICTS) {
        g_registered_dicts[g_registered_dict_count].name = sym;
        g_registered_dicts[g_registered_dict_count].dict = dict;
        g_registered_dict_count++;
    }
}

// Atom Array
t_atomarray *atomarray_new(long ac, t_atom *av) {
    t_atomarray *aa = (t_atomarray *)calloc(1, sizeof(t_atomarray));
    aa->ac = ac;
    aa->av = (t_atom *)calloc(ac, sizeof(t_atom));
    memcpy(aa->av, av, ac * sizeof(t_atom));
    return aa;
}

t_max_err atomarray_getindex(t_atomarray *aa, long idx, t_atom *val) {
    if (!aa || idx < 0 || idx >= aa->ac) return MAX_ERR_GENERIC;
    *val = aa->av[idx];
    return MAX_ERR_NONE;
}

t_max_err atomarray_getatoms(t_atomarray *aa, long *ac, t_atom **av) {
    if (!aa) return MAX_ERR_GENERIC;
    *ac = aa->ac;
    *av = aa->av;
    return MAX_ERR_NONE;
}

// Hash Table
t_hashtab *hashtab_new(long size) {
    return (t_hashtab *)calloc(1, sizeof(t_hashtab));
}

t_max_err hashtab_lookup(t_hashtab *h, t_symbol *key, t_object **val) {
    if (!h || !key) return MAX_ERR_GENERIC;
    for (int i = 0; i < h->count; i++) {
        if (strcmp(h->keys[i], key->s_name) == 0) {
            *val = h->values[i];
            return MAX_ERR_NONE;
        }
    }
    return MAX_ERR_GENERIC;
}

t_max_err hashtab_store(t_hashtab *h, t_symbol *key, t_object *val) {
    if (!h || !key) return MAX_ERR_GENERIC;
    for (int i = 0; i < h->count; i++) {
        if (strcmp(h->keys[i], key->s_name) == 0) {
            h->values[i] = val;
            return MAX_ERR_NONE;
        }
    }
    if (h->count >= h->capacity) {
        h->capacity = h->capacity == 0 ? 8 : h->capacity * 2;
        h->keys = (char **)realloc(h->keys, h->capacity * sizeof(char *));
        h->values = (t_object **)realloc(h->values, h->capacity * sizeof(t_object *));
    }
    h->keys[h->count] = strdup(key->s_name);
    h->values[h->count] = val;
    h->count++;
    return MAX_ERR_NONE;
}

t_max_err hashtab_getkeys(t_hashtab *h, long *numkeys, t_symbol ***keys) {
    if (!h) return MAX_ERR_GENERIC;
    *numkeys = h->count;
    *keys = (t_symbol **)calloc(h->count, sizeof(t_symbol *));
    for (int i = 0; i < h->count; i++) {
        (*keys)[i] = gensym(h->keys[i]);
    }
    return MAX_ERR_NONE;
}

void hashtab_clear(t_hashtab *h) {
    if (!h) return;
    for (int i = 0; i < h->count; i++) {
        free(h->keys[i]);
    }
    free(h->keys);
    free(h->values);
    h->keys = NULL;
    h->values = NULL;
    h->count = 0;
    h->capacity = 0;
}

// Global buffer registry
#define MAX_BUFFERS 256
static t_buffer_obj g_buffers[MAX_BUFFERS];
static int g_buffer_count = 0;

void mock_register_buffer(const char *name, float *samples, long long framecount, long channelcount, double samplerate) {
    // Update existing buffer if same name
    for (int i = 0; i < g_buffer_count; i++) {
        if (strcmp(g_buffers[i].name, name) == 0) {
            g_buffers[i].samples = samples;
            g_buffers[i].framecount = framecount;
            g_buffers[i].channelcount = channelcount;
            g_buffers[i].samplerate = samplerate;
            return;
        }
    }
    if (g_buffer_count < MAX_BUFFERS) {
        strncpy(g_buffers[g_buffer_count].name, name, 255);
        g_buffers[g_buffer_count].samples = samples;
        g_buffers[g_buffer_count].framecount = framecount;
        g_buffers[g_buffer_count].channelcount = channelcount;
        g_buffers[g_buffer_count].samplerate = samplerate;
        g_buffer_count++;
    }
}

void mock_unregister_all_buffers(void) {
    g_buffer_count = 0;
}

void mock_clear_registry(void) {
    g_buffer_count = 0;
    g_registered_dict_count = 0;
}

t_buffer_ref *buffer_ref_new(t_object *x, t_symbol *name) {
    t_buffer_ref *br = (t_buffer_ref *)calloc(1, sizeof(t_buffer_ref));
    br->owner = x;
    br->name = name;
    return br;
}

void buffer_ref_set(t_buffer_ref *br, t_symbol *name) {
    if (br) br->name = name;
}

t_buffer_obj *buffer_ref_getobject(t_buffer_ref *br) {
    if (!br || !br->name) return NULL;
    for (int i = 0; i < g_buffer_count; i++) {
        if (strcmp(g_buffers[i].name, br->name->s_name) == 0) {
            return &g_buffers[i];
        }
    }
    return NULL;
}

void buffer_ref_notify(t_buffer_ref *br, t_symbol *s, t_symbol *msg, void *sender, void *data) {}

float *buffer_locksamples(t_buffer_obj *b) {
    return b ? b->samples : NULL;
}

void buffer_unlocksamples(t_buffer_obj *b) {}

long long buffer_getframecount(t_buffer_obj *b) {
    return b ? b->framecount : 0;
}

long buffer_getchannelcount(t_buffer_obj *b) {
    return b ? b->channelcount : 0;
}

double buffer_getsamplerate(t_buffer_obj *b) {
    return b ? b->samplerate : 44100.0;
}

void buffer_setdirty(t_buffer_obj *b) {
    if (b) b->dirty = 1;
}

double sys_getsr(void) {
    return 44100.0;
}

double sys_getms(void) {
    return systime_ms();
}

double g_mock_time_ms = 0.0;
int g_use_mock_time = 0;

double systime_ms(void) {
    if (g_use_mock_time) {
        return g_mock_time_ms;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

void dsp_setup(t_pxobject *x, long inputs) {}
void dsp_free(t_pxobject *x) {}
void dsp_add64(void *dsp64, t_object *x, t_perfroutine64 perform, long flags, void *userparam) {}

void attr_args_process(void *x, short argc, t_atom *argv) {}

t_symbol *object_classname(void *x) {
    return gensym("weaver~");
}

long object_attr_getlong(void *x, t_symbol *attrname) {
    // Reads log and visualize directly from structured weaver struct
    typedef struct {
        t_pxobject t_obj;
        t_symbol *poly_prefix;
        long log;
        long visualize;
    } t_weaver_min;
    t_weaver_min *w = (t_weaver_min *)x;
    if (strcmp(attrname->s_name, "log") == 0) {
        return w->log;
    }
    if (strcmp(attrname->s_name, "visualize") == 0) {
        return w->visualize;
    }
    return 0;
}

int object_classname_compare(void *x, t_symbol *classname) {
    return strcmp(classname->s_name, "weaver~") == 0;
}

// Atom helper functions
t_symbol *atom_getsym(t_atom *a) {
    return (a && a->a_type == A_SYM) ? a->a_w.w_sym : NULL;
}

double atom_getfloat(t_atom *a) {
    if (!a) return 0.0;
    if (a->a_type == A_FLOAT) return a->a_w.w_float;
    if (a->a_type == A_LONG) return (double)a->a_w.w_long;
    return 0.0;
}

t_atom_long atom_getlong(t_atom *a) {
    if (!a) return 0;
    if (a->a_type == A_LONG) return (t_atom_long)a->a_w.w_long;
    if (a->a_type == A_FLOAT) return (t_atom_long)a->a_w.w_float;
    return 0;
}

long atom_gettype(t_atom *a) {
    return a ? a->a_type : A_NOTHING;
}

t_object *atom_getobj(t_atom *a) {
    return (a && a->a_type == A_OBJ) ? a->a_w.w_obj : NULL;
}

void atom_setlong(t_atom *a, t_atom_long val) {
    if (a) {
        a->a_type = A_LONG;
        a->a_w.w_long = (long)val;
    }
}

void atom_setfloat(t_atom *a, double val) {
    if (a) {
        a->a_type = A_FLOAT;
        a->a_w.w_float = val;
    }
}

void atom_setsym(t_atom *a, t_symbol *sym) {
    if (a) {
        a->a_type = A_SYM;
        a->a_w.w_sym = sym;
    }
}
