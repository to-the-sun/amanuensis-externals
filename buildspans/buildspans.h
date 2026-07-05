#ifndef BUILDSPANS_H
#define BUILDSPANS_H

#include "ext.h"
#include "ext_obex.h"
#include "ext_dictobj.h"
#include "ext_buffer.h"
#include "../shared/async_worker.h"

// Forward declaration
struct _buildspans;

typedef struct _buildspans {
    t_object s_obj;
    t_dictionary *building;
    t_dictionary *tracks_ended_in_current_event;
    long current_track;
    double current_offset;
    double loop_start;
    t_buffer_ref *buffer_ref;
    t_symbol *s_buffer_name;
    t_symbol *current_palette;
    void *span_outlet;
    void *track_outlet;
    void *out_bar_data;
    void *log_outlet;
    long log;
    long visualize;
    long defer;
    long async;
    t_async_worker *worker;
    double local_bar_length;
    long instance_id;
    long bar_warn_sent;
    t_symbol *last_msg_type;
    double last_note_calc;
    double last_note_store;
    double last_note_score;

    t_symbol *bind_name;
    void *bound_crucible;
    void *bind_clock;
    long bind_attempt_count;
} t_buildspans;

// Function prototypes for direct module-to-module coordination
void buildspans_do_list(t_buildspans *x, t_symbol *s, long argc, t_atom *argv);
void buildspans_do_bang(t_buildspans *x, t_symbol *s, long argc, t_atom *argv);
void buildspans_do_track(t_buildspans *x, long n);
void buildspans_do_offset(t_buildspans *x, double f, double loop_start);
void buildspans_do_anything(t_buildspans *x, t_symbol *s, long argc, t_atom *argv, long inlet_num);
void buildspans_do_local_bar_length(t_buildspans *x, t_symbol *s, long argc, t_atom *argv);

#endif // BUILDSPANS_H
