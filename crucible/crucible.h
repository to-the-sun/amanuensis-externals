#ifndef CRUCIBLE_H
#define CRUCIBLE_H

#include "ext.h"
#include "ext_obex.h"
#include "ext_dictionary.h"
#include "ext_dictobj.h"
#include "ext_buffer.h"
#include "../shared/async_worker.h"

typedef struct _crucible {
    t_object s_obj;
    t_dictionary *challenger_dict;
    t_symbol *last_track_id;
    t_symbol *incumbent_dict_name;
    void *outlet_data;
    void *outlet_fill;
    void *outlet_reach_int;
    void *log_outlet;
    t_buffer_ref *buffer_ref;
    long log;
    long consume;
    long defer;
    long async;
    t_async_worker *worker;
    long visualize;
    long fill;
    t_atom_long song_reach;
    t_atom_long song_min;
    t_dictionary *track_reaches_dict;
    double local_bar_length;
    long instance_id;
    long bar_warn_sent;
} t_crucible;

void crucible_anything(t_crucible *x, t_symbol *s, long argc, t_atom *argv);
void crucible_do_anything(t_crucible *x, t_symbol *s, long argc, t_atom *argv);
void crucible_local_bar_length(t_crucible *x, double f);
void crucible_do_local_bar_length(t_crucible *x, t_symbol *s, long argc, t_atom *argv);
void crucible_process_span(t_crucible *x, t_symbol *track_sym, t_atomarray *span_atomarray);
void crucible_rebar(t_crucible *x, t_atom_long new_bar_length);
void crucible_do_rebar(t_crucible *x, t_symbol *s, long argc, t_atom *argv);

#endif // CRUCIBLE_H
