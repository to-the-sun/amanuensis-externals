#ifndef BUILDSPANS_H
#define BUILDSPANS_H

#include "ext.h"
#include "ext_obex.h"
#include "ext_dictobj.h"
#include "ext_buffer.h"
#include "../shared/async_worker.h"

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
} t_buildspans;

#endif // BUILDSPANS_H
