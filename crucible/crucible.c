#include "ext.h"
#include "ext_obex.h"
#include "ext_dictionary.h"
#include "ext_dictobj.h"
#include <string.h>

typedef struct _crucible {
    t_object s_obj;
    t_dictionary *challenger_dict;
    t_dictionary *span_tracker_dict;
    t_symbol *incumbent_dict_name;
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
void crucible_process_span(t_crucible *x, t_symbol *track_sym, t_atomarray *span_atomarray);
void crucible_in1(t_crucible *x, long n);
void crucible_assist(t_crucible *x, void *b, long m, long a, char *s);
void crucible_verbose_log(t_crucible *x, const char *fmt, ...);
int parse_selector(const char *selector_str, char **track, char **bar, char **key);


t_class *crucible_class;

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

int parse_selector(const char *selector_str, char **track, char **bar, char **key) {
    const char *first_delim = strstr(selector_str, "::");
    if (!first_delim) return 0;
    const char *second_delim = strstr(first_delim + 2, "::");
    if (!second_delim) return 0;

    size_t track_len = first_delim - selector_str;
    *track = (char *)sysmem_newptr(track_len + 1);
    if (!*track) return 0;
    strncpy(*track, selector_str, track_len);
    (*track)[track_len] = '\0';

    size_t bar_len = second_delim - (first_delim + 2);
    *bar = (char *)sysmem_newptr(bar_len + 1);
    if (!*bar) {
        sysmem_freeptr(*track);
        return 0;
    }
    strncpy(*bar, first_delim + 2, bar_len);
    (*bar)[bar_len] = '\0';

    *key = (char *)sysmem_newptr(strlen(second_delim + 2) + 1);
    if (!*key) {
        sysmem_freeptr(*track);
        sysmem_freeptr(*bar);
        return 0;
    }
    strcpy(*key, second_delim + 2);

    return 1;
}

void ext_main(void *r) {
    t_class *c;
    c = class_new("crucible", (method)crucible_new, (method)crucible_free, (short)sizeof(t_crucible), 0L, A_DEFSYM, 0);
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
        x->incumbent_dict_name = s;
        x->challenger_dict = dictionary_new();
        x->span_tracker_dict = dictionary_new();
        x->verbose_log_outlet = NULL;
        x->bar_length = 125;

        attr_args_process(x, argc, argv);

        if (x->verbose) {
            x->verbose_log_outlet = outlet_new((t_object *)x, NULL);
        }
        x->outlet_offset = outlet_new((t_object *)x, NULL);
        x->outlet_bar = outlet_new((t_object *)x, NULL);
        x->outlet_palette = outlet_new((t_object *)x, NULL);
        x->outlet_reach = outlet_new((t_object *)x, NULL);

        intin((t_object *)x, 1);
    }
    return (x);
}

void crucible_free(t_crucible *x) {
    if (x->challenger_dict) {
        object_release((t_object *)x->challenger_dict);
    }
    if (x->span_tracker_dict) {
        object_release((t_object *)x->span_tracker_dict);
    }
}

void crucible_in1(t_crucible *x, long n) {
    x->bar_length = n;
    crucible_verbose_log(x, "Bar length set to: %ld", n);
}

void crucible_process_span(t_crucible *x, t_symbol *track_sym, t_atomarray *span_atomarray) {
    t_dictionary *incumbent_dict = dictobj_findregistered_retain(x->incumbent_dict_name);
    if (!incumbent_dict) {
        object_error((t_object *)x, "could not find dictionary named %s", x->incumbent_dict_name->s_name);
        return;
    }

    long span_len = 0;
    t_atom *span_atoms = NULL;
    atomarray_getatoms(span_atomarray, &span_len, &span_atoms);

    int challenger_wins = 1;

    for (long i = 0; i < span_len; i++) {
        long bar_ts = atom_getlong(&span_atoms[i]);
        char challenger_rating_key[256];
        snprintf(challenger_rating_key, 256, "%s::%ld::rating", track_sym->s_name, bar_ts);

        t_atomarray *challenger_rating_atomarray = NULL;
        dictionary_getatomarray(x->challenger_dict, gensym(challenger_rating_key), (t_object **)&challenger_rating_atomarray);

        if (!challenger_rating_atomarray) continue;

        long challenger_rating_len = 0;
        t_atom *challenger_rating_atoms = NULL;
        atomarray_getatoms(challenger_rating_atomarray, &challenger_rating_len, &challenger_rating_atoms);
        if(challenger_rating_len == 0) continue;

        double challenger_rating = atom_getfloat(challenger_rating_atoms);

        char incumbent_rating_key[256];
        snprintf(incumbent_rating_key, 256, "%s::%ld::rating", track_sym->s_name, bar_ts);

        if (dictionary_hasentry(incumbent_dict, gensym(incumbent_rating_key))) {
            t_atomarray *incumbent_rating_atomarray = NULL;
            dictionary_getatomarray(incumbent_dict, gensym(incumbent_rating_key), (t_object **)&incumbent_rating_atomarray);

            long incumbent_rating_len = 0;
            t_atom *incumbent_rating_atoms = NULL;
            atomarray_getatoms(incumbent_rating_atomarray, &incumbent_rating_len, &incumbent_rating_atoms);
            if(incumbent_rating_len == 0) {
                crucible_verbose_log(x, "Bar %ld: Challenger rating %.2f vs Incumbent (no-contest, empty atomarray). Challenger wins bar.", bar_ts, challenger_rating);
                post("Bar %ld: Challenger rating %.2f vs Incumbent (no-contest, empty atomarray). Challenger wins bar.", bar_ts, challenger_rating);
                continue;
            }

            double incumbent_rating = atom_getfloat(incumbent_rating_atoms);
            crucible_verbose_log(x, "Bar %ld: Challenger rating %.2f vs Incumbent rating %.2f.", bar_ts, challenger_rating, incumbent_rating);
            post("Bar %ld: Challenger rating %.2f vs Incumbent rating %.2f.", bar_ts, challenger_rating, incumbent_rating);
            if (challenger_rating <= incumbent_rating) {
                crucible_verbose_log(x, "-> Challenger loses bar. Span comparison failed.");
                post("-> Challenger loses bar. Span comparison failed.");
                challenger_wins = 0;
                break;
            } else {
                crucible_verbose_log(x, "-> Challenger wins bar.");
                post("-> Challenger wins bar.");
            }
        } else {
            crucible_verbose_log(x, "Bar %ld: Challenger rating %.2f vs Incumbent (no-contest, no entry). Challenger wins bar.", bar_ts, challenger_rating);
            post("Bar %ld: Challenger rating %.2f vs Incumbent (no-contest, no entry). Challenger wins bar.", bar_ts, challenger_rating);
        }
    }

    if (challenger_wins) {
        crucible_verbose_log(x, "Challenger span for track %s won. Overwriting incumbent dictionary.", track_sym->s_name);
        post("Challenger span for track %s won. Overwriting incumbent dictionary.", track_sym->s_name);
        t_symbol **challenger_keys;
        long num_keys;
        dictionary_getkeys(x->challenger_dict, &num_keys, &challenger_keys);

        for (long i = 0; i < num_keys; i++) {
            char *track_str, *bar_str, *key_str;
            if (parse_selector(challenger_keys[i]->s_name, &track_str, &bar_str, &key_str)) {
                 if (strcmp(track_str, track_sym->s_name) == 0) {
                    if (dictionary_hasentry(incumbent_dict, challenger_keys[i])) {
                        dictionary_deleteentry(incumbent_dict, challenger_keys[i]);
                    }
                    t_atomarray *value = NULL;
                    dictionary_getatomarray(x->challenger_dict, challenger_keys[i], (t_object **)&value);
                    dictionary_appendatomarray(incumbent_dict, challenger_keys[i], (t_object *)value);
                    crucible_verbose_log(x, "  -> Wrote key to incumbent: %s", challenger_keys[i]->s_name);
                 }
                 sysmem_freeptr(track_str);
                 sysmem_freeptr(bar_str);
                 sysmem_freeptr(key_str);
            }
        }
        if(challenger_keys) sysmem_freeptr(challenger_keys);

        for (long i = 0; i < span_len; i++) {
             long bar_ts = atom_getlong(&span_atoms[i]);

             char offset_key[256], palette_key[256], span_key[256];
             snprintf(offset_key, 256, "%s::%ld::offset", track_sym->s_name, bar_ts);
             snprintf(palette_key, 256, "%s::%ld::palette", track_sym->s_name, bar_ts);
             snprintf(span_key, 256, "%s::%ld::span", track_sym->s_name, bar_ts);

             t_atomarray *offset_atomarray, *palette_atomarray, *bar_span_atomarray;

             dictionary_getatomarray(x->challenger_dict, gensym(offset_key), (t_object **)&offset_atomarray);
             dictionary_getatomarray(x->challenger_dict, gensym(palette_key), (t_object **)&palette_atomarray);
             dictionary_getatomarray(x->challenger_dict, gensym(span_key), (t_object **)&bar_span_atomarray);

             if(offset_atomarray){
                long len; t_atom *atoms;
                atomarray_getatoms(offset_atomarray, &len, &atoms);
                if(len > 0) outlet_int(x->outlet_offset, atom_getlong(atoms));
             }
             if(palette_atomarray){
                long len; t_atom *atoms;
                atomarray_getatoms(palette_atomarray, &len, &atoms);
                if(len > 0) outlet_anything(x->outlet_palette, atom_getsym(atoms), 0, NULL);
             }

             if(bar_span_atomarray){
                long bar_span_len = 0;
                t_atom *bar_span_atoms = NULL;
                atomarray_getatoms(bar_span_atomarray, &bar_span_len, &bar_span_atoms);

                long max_val = 0;
                for(long j=0; j < bar_span_len; j++){
                    long current_val = atom_getlong(bar_span_atoms + j);
                    if(current_val > max_val) max_val = current_val;
                }
                outlet_int(x->outlet_bar, bar_ts);
                outlet_int(x->outlet_reach, max_val + x->bar_length);
             }
        }

    } else {
        crucible_verbose_log(x, "Challenger span for track %s lost.", track_sym->s_name);
    }

    t_symbol **keys;
    long numkeys;
    dictionary_getkeys(x->challenger_dict, &numkeys, &keys);
    for(long i=0; i<numkeys; i++){
        char *track_str, *bar_str, *key_str;
        if(parse_selector(keys[i]->s_name, &track_str, &bar_str, &key_str)){
            if(strcmp(track_str, track_sym->s_name) == 0){
                dictionary_deleteentry(x->challenger_dict, keys[i]);
            }
            sysmem_freeptr(track_str);
            sysmem_freeptr(bar_str);
            sysmem_freeptr(key_str);
        }
    }
    if(keys) sysmem_freeptr(keys);


    object_release((t_object *)incumbent_dict);
}

void crucible_anything(t_crucible *x, t_symbol *s, long argc, t_atom *argv) {
    crucible_verbose_log(x, "Received message: %s", s->s_name);

    char *track_str = NULL;
    char *bar_str = NULL;
    char *key_str = NULL;

    if (parse_selector(s->s_name, &track_str, &bar_str, &key_str)) {
        t_symbol *track_sym = gensym(track_str);

        dictionary_appendatomarray(x->challenger_dict, s, (t_object *)atomarray_new(argc, argv));

        // Hierarchical keys for tracker
        char received_keys_key_str[256];
        snprintf(received_keys_key_str, 256, "%s::received_keys", track_sym->s_name);
        t_symbol *received_keys_sym = gensym(received_keys_key_str);

        char expected_keys_key_str[256];
        snprintf(expected_keys_key_str, 256, "%s::expected_keys", track_sym->s_name);
        t_symbol *expected_keys_sym = gensym(expected_keys_key_str);

        char span_bars_key_str[256];
        snprintf(span_bars_key_str, 256, "%s::span_bars", track_sym->s_name);
        t_symbol *span_bars_sym = gensym(span_bars_key_str);

        t_atom_long received_keys = 0;
        if (dictionary_hasentry(x->span_tracker_dict, received_keys_sym)) {
            dictionary_getlong(x->span_tracker_dict, received_keys_sym, &received_keys);
        }
        received_keys++;
        dictionary_appendlong(x->span_tracker_dict, received_keys_sym, received_keys);

        if (strcmp(key_str, "span") == 0) {
            long num_bars = argc;
            t_atom_long expected_keys = num_bars * 7;
            dictionary_appendlong(x->span_tracker_dict, expected_keys_sym, expected_keys);
            dictionary_appendatomarray(x->span_tracker_dict, span_bars_sym, (t_object *)atomarray_new(argc, argv));
        }

        if (dictionary_hasentry(x->span_tracker_dict, expected_keys_sym)) {
            t_atom_long expected_keys = 0;
            dictionary_getlong(x->span_tracker_dict, expected_keys_sym, &expected_keys);

            if (received_keys >= expected_keys) {
                t_atomarray *span_atomarray = NULL;
                dictionary_getatomarray(x->span_tracker_dict, span_bars_sym, (t_object **)&span_atomarray);
                if (span_atomarray) {
                    crucible_process_span(x, track_sym, span_atomarray);
                }
                dictionary_deleteentry(x->span_tracker_dict, received_keys_sym);
                dictionary_deleteentry(x->span_tracker_dict, expected_keys_sym);
                dictionary_deleteentry(x->span_tracker_dict, span_bars_sym);
            }
        }

        sysmem_freeptr(track_str);
        sysmem_freeptr(bar_str);
        sysmem_freeptr(key_str);
    } else {
        crucible_verbose_log(x, "Unparsable message selector: %s", s->s_name);
    }
}


void crucible_assist(t_crucible *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        switch (a) {
            case 0: sprintf(s, "(anything) Message Stream from buildspans, (symbol) Incumbent Dictionary Name"); break;
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
