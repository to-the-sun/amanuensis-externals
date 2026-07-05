#ifndef ASYNC_WORKER_H
#define ASYNC_WORKER_H

#ifndef STANDALONE_TEST
#include "ext.h"
#include "ext_systhread.h"
#include "ext_linklist.h"
#else
#include <stdint.h>
#include <stddef.h>
typedef void (*method)(void *x, ...);
typedef struct _symbol { char *s_name; } t_symbol;
typedef struct _atom { int type; } t_atom;
typedef intptr_t t_atom_long;
typedef int t_max_err;
typedef struct _systhread *t_systhread;
typedef struct _systhread_mutex *t_systhread_mutex;
typedef struct _systhread_cond *t_systhread_cond;
typedef struct _linklist t_linklist;
#endif

typedef struct _async_task {
    void *x;                // Target object
    method m;               // Method to call
    t_symbol *s;            // Selector
    long argc;              // Argument count
    t_atom *argv;           // Arguments
} t_async_task;

typedef struct _async_worker {
    t_systhread thread;
    t_systhread_mutex mutex;
    t_systhread_cond cond;
    t_linklist *queue;
    int exit_flag;
    int ref_count;
} t_async_worker;

t_async_worker *async_worker_create(void);
void async_worker_retain(t_async_worker *worker);
void async_worker_release(t_async_worker *worker);
void async_worker_enqueue(t_async_worker *worker, void *x, method m, t_symbol *s, long argc, t_atom *argv);

#endif // ASYNC_WORKER_H
