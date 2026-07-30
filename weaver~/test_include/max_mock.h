#ifndef MAX_MOCK_H
#define MAX_MOCK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <math.h>

#define WIN_VERSION
#define C74_X64

typedef int t_max_err;
#define MAX_ERR_NONE 0
#define MAX_ERR_GENERIC 1

typedef struct _symbol {
    const char *s_name;
    struct _object *s_thing;
} t_symbol;

extern t_symbol * _sym_nothing;

typedef enum {
    A_NOTHING = 0,
    A_LONG = 1,
    A_FLOAT = 2,
    A_SYM = 3,
    A_OBJ = 4,
    A_GIMME = 5,
    A_CANT = 6
} t_atomtype;

typedef struct _object {
    struct _class *o_class;
} t_object;

typedef union _word {
    long w_long;
    double w_float;
    t_symbol *w_sym;
    t_object *w_obj;
} t_word;

typedef struct _atom {
    long a_type;
    t_word a_w;
} t_atom;

typedef int64_t t_atom_long;

#define ASSIST_INLET 1
#define ASSIST_OUTLET 2
#define CLASS_BOX 1

typedef void *(*method)(void *, ...);

typedef struct _class {
    const char *name;
} t_class;

// Attributes
#define CLASS_ATTR_LONG(c, name, flags, structname, member)
#define CLASS_ATTR_STYLE_LABEL(c, name, flags, style, label)
#define CLASS_ATTR_DEFAULT(c, name, flags, val)
#define CLASS_ATTR_ACCESSORS(c, name, getter, setter)
#define CLASS_ATTR_LABEL(c, name, flags, label)
#define CLASS_ATTR_DOUBLE(c, name, flags, structname, member)

// DSP PX Object
typedef struct _pxobject {
    t_object z_ob;
} t_pxobject;

// Threading & Critical Mocks using POSIX threads
typedef pthread_mutex_t* t_critical;
typedef pthread_t t_systhread;
typedef pthread_mutex_t* t_systhread_mutex;
typedef pthread_cond_t* t_systhread_cond;

// Mock structures
typedef struct _dictionary {
    // Array of key-value pairs
    int count;
    int capacity;
    char **keys;
    t_atom *values;
} t_dictionary;

typedef struct _atomarray {
    long ac;
    t_atom *av;
} t_atomarray;

typedef struct _hashtab {
    int count;
    int capacity;
    char **keys;
    t_object **values;
} t_hashtab;

typedef struct _qelem {
    void *obj;
    method fn;
    int set;
} t_qelem;

typedef struct _buffer_obj {
    char name[256];
    float *samples;
    long long framecount;
    long channelcount;
    double samplerate;
    int dirty;
} t_buffer_obj;

typedef struct _buffer_ref {
    t_object *owner;
    t_symbol *name;
} t_buffer_ref;

// DSP typedefs
typedef void (*t_perfroutine64)(void *x, void *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);

// Max API mocks declarations
void common_symbols_init(void);
t_symbol *gensym(const char *s);
void *sysmem_newptr(size_t size);
void sysmem_freeptr(void *ptr);
void *sysmem_resizeptr(void *ptr, size_t size);

t_class *class_new(const char *name, method newmethod, method freemethod, size_t size, method menu_or_dummy, int type, ...);
void class_addmethod(t_class *c, method m, const char *name, ...);
void class_dspinit(t_class *c);
void class_register(int type, t_class *c);

void *outlet_new(void *ob, const char *classname);
void outlet_anything(void *outlet, t_symbol *s, short argc, t_atom *argv);
void outlet_list(void *outlet, t_symbol *s, short argc, t_atom *argv);
void outlet_int(void *outlet, t_atom_long n);
void outlet_bang(void *outlet);

void *object_alloc(t_class *c);
void object_free(void *x);
void object_warn(void *x, const char *fmt, ...);
void object_error(void *x, const char *fmt, ...);
void object_post(void *x, const char *fmt, ...);

void *proxy_new(void *x, long id, long *proxy_id);
long proxy_getinlet(void *x);
extern long g_mock_inlet;

t_qelem *qelem_new(void *obj, method fn);
void qelem_set(t_qelem *q);
void qelem_free(t_qelem *q);

void critical_new(t_critical *lock);
void critical_enter(t_critical lock);
void critical_exit(t_critical lock);
void critical_free(t_critical lock);
t_max_err critical_tryenter(t_critical lock);

int systhread_create(method fn, void *arg, size_t stacksize, int priority, int flags, t_systhread *thread);
void systhread_join(t_systhread thread, unsigned int *retval);
void systhread_sleep(unsigned int ms);
t_systhread systhread_self(void);
void systhread_set_name(const char *name);
void systhread_exit(unsigned int retval);

void systhread_mutex_new(t_systhread_mutex *mutex, int flags);
void systhread_mutex_free(t_systhread_mutex mutex);
void systhread_mutex_lock(t_systhread_mutex mutex);
void systhread_mutex_unlock(t_systhread_mutex mutex);

void systhread_cond_new(t_systhread_cond *cond, int flags);
void systhread_cond_free(t_systhread_cond cond);
void systhread_cond_wait(t_systhread_cond cond, t_systhread_mutex mutex);
void systhread_cond_signal(t_systhread_cond cond);

t_dictionary *dictionary_new(void);
t_max_err dictionary_getkeys(t_dictionary *d, long *numkeys, t_symbol ***keys);
t_max_err dictionary_getdictionary(t_dictionary *d, t_symbol *key, t_object **val);
t_max_err dictionary_getatom(t_dictionary *d, t_symbol *key, t_atom *val);
t_max_err dictionary_getatomarray(t_dictionary *d, t_symbol *key, t_object **val);
void dictionary_clear(t_dictionary *d);
t_max_err dictionary_appenddictionary(t_dictionary *d, t_symbol *key, t_object *val);
t_max_err dictionary_appendatomarray(t_dictionary *d, t_symbol *key, t_object *val);
t_max_err dictionary_appendatom(t_dictionary *d, t_symbol *key, t_atom *val);
t_max_err dictionary_appendlong(t_dictionary *d, t_symbol *key, t_atom_long val);
int dictionary_hasentry(t_dictionary *d, t_symbol *key);
t_max_err dictionary_deleteentry(t_dictionary *d, t_symbol *key);
long dictionary_getentrycount(t_dictionary *d);
void object_release(t_object *obj);
void object_retain(t_object *obj);

t_dictionary *dictobj_findregistered_retain(t_symbol *name);
void dictobj_release(t_dictionary *d);
void mock_register_dict(const char *name, t_dictionary *dict);

t_atomarray *atomarray_new(long ac, t_atom *av);
t_max_err atomarray_getindex(t_atomarray *aa, long idx, t_atom *val);
t_max_err atomarray_getatoms(t_atomarray *aa, long *ac, t_atom **av);

t_hashtab *hashtab_new(long size);
t_max_err hashtab_lookup(t_hashtab *h, t_symbol *key, t_object **val);
t_max_err hashtab_store(t_hashtab *h, t_symbol *key, t_object *val);
t_max_err hashtab_getkeys(t_hashtab *h, long *numkeys, t_symbol ***keys);
void hashtab_clear(t_hashtab *h);

t_buffer_ref *buffer_ref_new(t_object *x, t_symbol *name);
void buffer_ref_set(t_buffer_ref *br, t_symbol *name);
t_buffer_obj *buffer_ref_getobject(t_buffer_ref *br);
void buffer_ref_notify(t_buffer_ref *br, t_symbol *s, t_symbol *msg, void *sender, void *data);

float *buffer_locksamples(t_buffer_obj *b);
void buffer_unlocksamples(t_buffer_obj *b);
long long buffer_getframecount(t_buffer_obj *b);
long buffer_getchannelcount(t_buffer_obj *b);
double buffer_getsamplerate(t_buffer_obj *b);
void buffer_setdirty(t_buffer_obj *b);

double sys_getsr(void);
double systime_ms(void);

extern double g_mock_time_ms;
extern int g_use_mock_time;

void dsp_setup(t_pxobject *x, long inputs);
void dsp_free(t_pxobject *x);
void dsp_add64(void *dsp64, t_object *x, t_perfroutine64 perform, long flags, void *userparam);

void attr_args_process(void *x, short argc, t_atom *argv);

t_symbol *object_classname(void *x);
long object_attr_getlong(void *x, t_symbol *attrname);
int object_classname_compare(void *x, t_symbol *classname);

// Helper functions for our mock environment
void mock_register_buffer(const char *name, float *samples, long long framecount, long channelcount, double samplerate);
void mock_unregister_all_buffers(void);
void mock_clear_registry(void);
t_symbol *atom_getsym(t_atom *a);
double atom_getfloat(t_atom *a);
t_atom_long atom_getlong(t_atom *a);
long atom_gettype(t_atom *a);
t_object *atom_getobj(t_atom *a);
void atom_setlong(t_atom *a, t_atom_long val);
void atom_setfloat(t_atom *a, double val);
void atom_setsym(t_atom *a, t_symbol *sym);

#endif // MAX_MOCK_H
