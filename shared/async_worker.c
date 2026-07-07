#include "async_worker.h"
#include <stdlib.h>
#include <string.h>

#ifdef STANDALONE_TEST
void *sysmem_newptr(size_t size);
void sysmem_freeptr(void *ptr);
t_linklist *linklist_new(void);
void linklist_chuck(t_linklist *x);
t_atom_long linklist_getsize(t_linklist *x);
void *linklist_getindex(t_linklist *x, long index);
long linklist_chuckindex(t_linklist *x, long index);
t_atom_long linklist_append(t_linklist *x, void *o);
t_max_err systhread_create(method proc, void *arg, long stack, long priority, long flags, t_systhread *thread);
void systhread_exit(void *status);
void systhread_join(t_systhread thread, unsigned int *ret);
void systhread_mutex_new(t_systhread_mutex *m, long flags);
void systhread_mutex_lock(t_systhread_mutex m);
void systhread_mutex_unlock(t_systhread_mutex m);
void systhread_mutex_free(t_systhread_mutex m);
void systhread_cond_new(t_systhread_cond *c, long flags);
void systhread_cond_wait(t_systhread_cond c, t_systhread_mutex m);
void systhread_cond_signal(t_systhread_cond c);
void systhread_cond_free(t_systhread_cond c);
#endif

void *async_worker_thread_proc(void *arg) {
    t_async_worker *worker = (t_async_worker *)arg;
    
    while (1) {
        t_async_task *task = NULL;
        
        systhread_mutex_lock(worker->mutex);
        while (linklist_getsize(worker->queue) == 0 && !worker->exit_flag) {
            systhread_cond_wait(worker->cond, worker->mutex);
        }
        
        if (worker->exit_flag && linklist_getsize(worker->queue) == 0) {
            systhread_mutex_unlock(worker->mutex);
            break;
        }
        
        task = (t_async_task *)linklist_getindex(worker->queue, 0);
        if (task) linklist_chuckindex(worker->queue, 0);
        
        systhread_mutex_unlock(worker->mutex);
        
        if (task) {
            if (task->m) {
                ((void (*)(void *, t_symbol *, long, t_atom *))task->m)(task->x, task->s, task->argc, task->argv);
            }
            
            if (task->argv) sysmem_freeptr(task->argv);
            sysmem_freeptr(task);
        }
    }
    
    systhread_exit(0);
    return NULL;
}


t_async_worker *async_worker_create(void) {
    t_async_worker *worker = (t_async_worker *)sysmem_newptr(sizeof(t_async_worker));
    if (!worker) return NULL;
    
    worker->exit_flag = 0;
    worker->ref_count = 1;
    worker->queue = linklist_new();
    systhread_mutex_new(&worker->mutex, 0);
    systhread_cond_new(&worker->cond, 0);
    
    systhread_create((method)async_worker_thread_proc, worker, 0, 0, 0, &worker->thread);
    
    return worker;
}

void async_worker_retain(t_async_worker *worker) {
    if (!worker) return;
    systhread_mutex_lock(worker->mutex);
    worker->ref_count++;
    systhread_mutex_unlock(worker->mutex);
}

void async_worker_release(t_async_worker *worker) {
    if (!worker) return;
    
    int should_destroy = 0;
    systhread_mutex_lock(worker->mutex);
    worker->ref_count--;
    if (worker->ref_count <= 0) {
        worker->exit_flag = 1;
        should_destroy = 1;
    }
    systhread_cond_signal(worker->cond);
    systhread_mutex_unlock(worker->mutex);
    
    if (should_destroy) {
        unsigned int ret;
        systhread_join(worker->thread, &ret);
        
        // Clean up queue if anything left
        t_async_task *task;
        while (linklist_getsize(worker->queue) > 0) {
            task = (t_async_task *)linklist_getindex(worker->queue, 0);
            if (task) {
                linklist_chuckindex(worker->queue, 0);
                if (task->argv) sysmem_freeptr(task->argv);
                sysmem_freeptr(task);
            }
        }
        linklist_chuck(worker->queue);
        
        systhread_mutex_free(worker->mutex);
        systhread_cond_free(worker->cond);
        sysmem_freeptr(worker);
    }
}

void async_worker_enqueue(t_async_worker *worker, void *x, method m, t_symbol *s, long argc, t_atom *argv) {
    if (!worker) return;
    
    t_async_task *task = (t_async_task *)sysmem_newptr(sizeof(t_async_task));
    if (!task) return;
    
    task->x = x;
    task->m = m;
    task->s = s;
    task->argc = argc;
    task->argv = NULL;
    
    if (argc > 0 && argv) {
        task->argv = (t_atom *)sysmem_newptr(sizeof(t_atom) * argc);
        if (task->argv) {
            memcpy(task->argv, argv, sizeof(t_atom) * argc);
        } else {
            task->argc = 0;
        }
    }
    
    systhread_mutex_lock(worker->mutex);
    linklist_append(worker->queue, task);
    systhread_cond_signal(worker->cond);
    systhread_mutex_unlock(worker->mutex);
}

int async_worker_is_worker_thread(t_async_worker *worker) {
    if (!worker) return 0;
#ifndef STANDALONE_TEST
    return (systhread_self() == worker->thread);
#else
    return 0; // Not easily testable in standalone without more mocks
#endif
}
