#include "ext.h"
#include "ext_obex.h"
#include "ext_dictobj.h"
#include "../shared/visualize.h"

typedef struct _whichoffset {
    t_object s_obj;
    t_symbol *dict_name;
    long track;
    long verbose;
    void *outlet;
} t_whichoffset;

void *whichoffset_new(t_symbol *s, long argc, t_atom *argv);
void whichoffset_free(t_whichoffset *x);
void whichoffset_bang(t_whichoffset *x);
void whichoffset_int(t_whichoffset *x, long n);
void whichoffset_span(t_whichoffset *x, t_symbol *s, long argc, t_atom *argv);
void whichoffset_assist(t_whichoffset *x, void *b, long m, long a, char *s);
void whichoffset_print_dict(t_dictionary *d, int indent);
t_max_err dictionary_getatoms_nested(t_dictionary *d, const char *path, long *argc, t_atom **argv);
int compare_doubles(const void *a, const void *b);
void dictionary_to_json_string_recursive(t_dictionary *d, char **str, long *len, long *capacity);
char* dictionary_to_string(t_dictionary *d);
void whichoffset_send_dict_in_chunks(t_whichoffset *x, t_dictionary *d);

t_class *whichoffset_class;

void ext_main(void *r) {
    t_class *c;

    c = class_new("whichoffset", (method)whichoffset_new, (method)whichoffset_free, (short)sizeof(t_whichoffset), 0L, A_GIMME, 0);
    class_addmethod(c, (method)whichoffset_bang, "bang", 0);
    class_addmethod(c, (method)whichoffset_int, "int", A_LONG, 0);
    class_addmethod(c, (method)whichoffset_span, "span", A_GIMME, 0);
    class_addmethod(c, (method)whichoffset_assist, "assist", A_CANT, 0);

    CLASS_ATTR_LONG(c, "verbose", 0, t_whichoffset, verbose);
    CLASS_ATTR_STYLE_LABEL(c, "verbose", 0, "onoff", "Enable Verbose Logging");
    CLASS_ATTR_DEFAULT(c, "verbose", 0, "0");

    class_register(CLASS_BOX, c);
    whichoffset_class = c;
}

void *whichoffset_new(t_symbol *s, long argc, t_atom *argv) {
    t_whichoffset *x = (t_whichoffset *)object_alloc(whichoffset_class);
    if (x) {
        x->dict_name = _sym_nothing;
        x->track = 0;
        x->verbose = 0;
        x->outlet = floatout((t_object *)x); // Create a float outlet

        if (argc > 0 && atom_gettype(argv) == A_SYM && atom_getsym(argv)->s_name[0] != '@') {
            x->dict_name = atom_getsym(argv);
            argc--;
            argv++;
        }

        attr_args_process(x, argc, argv);

        if (x->verbose) {
            if (visualize_init() != 0) {
                object_error((t_object *)x, "Failed to initialize UDP connection.");
            } else {
                post("UDP connection initialized for visualization.");
            }
        }
    }
    return (x);
}

void whichoffset_free(t_whichoffset *x) {
    visualize_cleanup();
}

void whichoffset_bang(t_whichoffset *x) {
    t_dictionary *d = dictobj_findregistered_clone(x->dict_name);

    if (d) {
        if (x->verbose) {
            //whichoffset_print_dict(d, 0);
            whichoffset_send_dict_in_chunks(x, d);
        }
        object_free(d);
    } else {
        object_error((t_object *)x, "could not find dictionary %s", x->dict_name->s_name);
    }
}

void whichoffset_int(t_whichoffset *x, long n) {
    x->track = n;
    post("whichoffset: track set to %ld", n);
}

void whichoffset_span(t_whichoffset *x, t_symbol *s, long argc, t_atom *argv) {
    if (argc == 0) {
        object_error((t_object *)x, "span: received empty list");
        return;
    }

    t_dictionary *dict = dictobj_findregistered_clone(x->dict_name);
    if (!dict) {
        object_error((t_object *)x, "span: could not find dictionary %s", x->dict_name->s_name);
        return;
    }

    post("whichoffset: received %ld timestamps", argc);
    long min_ts = atom_getlong(argv);
    long max_ts = atom_getlong(argv);
    for (long i = 1; i < argc; i++) {
        long current_ts = atom_getlong(argv + i);
        if (current_ts < min_ts) min_ts = current_ts;
        if (current_ts > max_ts) max_ts = current_ts;
    }
    post("whichoffset: min_ts %ld, max_ts %ld", min_ts, max_ts);

    char min_path[256], max_path[256];
    snprintf(min_path, 256, "%ld::%ld::absolutes", x->track, min_ts);
    snprintf(max_path, 256, "%ld::%ld::absolutes", x->track, max_ts);

    t_atom *min_abs_ts_atoms = NULL, *max_abs_ts_atoms = NULL;
    long min_abs_ts_argc = 0, max_abs_ts_argc = 0;
    dictionary_getatoms_nested(dict, min_path, &min_abs_ts_argc, &min_abs_ts_atoms);
    dictionary_getatoms_nested(dict, max_path, &max_abs_ts_argc, &max_abs_ts_atoms);
    
    if (min_abs_ts_argc == 0) {
        object_error((t_object *)x, "span: could not find dictionary key '%s'", min_path);
        if (min_abs_ts_atoms) sysmem_freeptr(min_abs_ts_atoms);
        if (max_abs_ts_atoms) sysmem_freeptr(max_abs_ts_atoms);
        object_free(dict);
        return;
    }
    if (max_abs_ts_argc == 0) {
        object_error((t_object *)x, "span: could not find dictionary key '%s'", max_path);
        if (min_abs_ts_atoms) sysmem_freeptr(min_abs_ts_atoms);
        if (max_abs_ts_atoms) sysmem_freeptr(max_abs_ts_atoms);
        object_free(dict);
        return;
    }

    double min_abs_ts = atom_getfloat(min_abs_ts_atoms);
    double max_abs_ts = atom_getfloat(max_abs_ts_atoms);
    
    for(long i=1; i < min_abs_ts_argc; ++i) {
        double current_val = atom_getfloat(min_abs_ts_atoms + i);
        if(current_val < min_abs_ts) min_abs_ts = current_val;
    }
    for(long i=1; i < max_abs_ts_argc; ++i) {
        double current_val = atom_getfloat(max_abs_ts_atoms + i);
        if(current_val > max_abs_ts) max_abs_ts = current_val;
    }
    post("whichoffset: min_abs_ts %f, max_abs_ts %f", min_abs_ts, max_abs_ts);

    if (min_abs_ts_atoms) sysmem_freeptr(min_abs_ts_atoms);
    if (max_abs_ts_atoms) sysmem_freeptr(max_abs_ts_atoms);

    double *gathered_offsets = (double *)sysmem_newptr(argc * sizeof(double));
    long gathered_offsets_count = 0;
    for (long i = 0; i < argc; i++) {
        char offset_path[256];
        snprintf(offset_path, 256, "%ld::%ld::offset", x->track, atom_getlong(argv + i));
        
        t_atom *offset_atoms = NULL;
        long offset_argc = 0;
        dictionary_getatoms_nested(dict, offset_path, &offset_argc, &offset_atoms);
        
        if (offset_argc > 0) {
            gathered_offsets[gathered_offsets_count++] = atom_getfloat(offset_atoms);
        }
        if (offset_atoms) sysmem_freeptr(offset_atoms);
    }
    
    if (gathered_offsets_count == 0) {
        object_post((t_object *)x, "span: no offsets found");
        sysmem_freeptr(gathered_offsets);
        object_free(dict);
        return;
    }

    qsort(gathered_offsets, gathered_offsets_count, sizeof(double), compare_doubles);
    
    char offsets_str[1024] = "whichoffset: gathered offsets: ";
    for (int i=0; i < gathered_offsets_count; i++) {
        char temp[32];
        snprintf(temp, 32, "%.2f ", gathered_offsets[i]);
        strcat(offsets_str, temp);
    }
    post(offsets_str);
    
    double loop_length = 0;
    if (gathered_offsets_count > 1) {
        long second_unique_index = -1;
        for (long i = 1; i < gathered_offsets_count; i++) {
            if (gathered_offsets[i] != gathered_offsets[0]) {
                second_unique_index = i;
                break;
            }
        }
        
        if (second_unique_index != -1) {
            loop_length = gathered_offsets[second_unique_index] - gathered_offsets[0];
        }
    }
    
    post("whichoffset: loop_length %f", loop_length);
    
    if (loop_length == 0 && gathered_offsets_count > 0) {
        post("whichoffset: only one unique offset, outputting %f", gathered_offsets[0]);
        outlet_float(x->outlet, gathered_offsets[0]);
        sysmem_freeptr(gathered_offsets);
        object_free(dict);
        return;
    }

    double smallest_sum = -1;
    long smallest_index = -1;
    
    for (long i = 0; i < gathered_offsets_count; i++) {
        double min_value = fmax(0, gathered_offsets[i] - min_abs_ts);
        double max_value = fmax(0, max_abs_ts - (gathered_offsets[i] + loop_length));
        double sum = min_value + max_value;
        
        post("whichoffset: offset %f, excess before start: %f, excess after end: %f",
             gathered_offsets[i], min_value, max_value);

        if (smallest_index == -1 || sum < smallest_sum) {
            smallest_sum = sum;
            smallest_index = i;
        }
    }

    if (smallest_index != -1) {
        post("whichoffset: optimal offset is %f", gathered_offsets[smallest_index]);
        outlet_float(x->outlet, gathered_offsets[smallest_index]);
    }
    
    sysmem_freeptr(gathered_offsets);
    object_free(dict);
}

int compare_doubles(const void *a, const void *b) {
    double fa = *(const double*) a;
    double fb = *(const double*) b;
    return (fa > fb) - (fa < fb);
}

void ensure_string_capacity(char **str, long *capacity, long required) {
    if (required > *capacity) {
        *capacity = required * 2; // Double the required size to reduce reallocations
        *str = (char *)sysmem_resizeptr(*str, *capacity);
    }
}

void dictionary_to_json_string_recursive(t_dictionary *d, char **str, long *len, long *capacity) {
    long numkeys;
    t_symbol **keys;
    long i;

    dictionary_getkeys(d, &numkeys, &keys);
    if (keys) {
        for (i = 0; i < numkeys; i++) {
            t_atom *argv = NULL;
            long argc = 0;
            long required_len;

            // Append key
            required_len = *len + strlen(keys[i]->s_name) + 4; // for "key":
            ensure_string_capacity(str, capacity, required_len);
            *len += sprintf(*str + *len, "\"%s\":", keys[i]->s_name);

            dictionary_getatoms(d, keys[i], &argc, &argv);

            if (argc > 0 && atom_gettype(argv) == A_OBJ && object_classname(atom_getobj(argv)) == gensym("dictionary")) {
                t_dictionary *subdict = (t_dictionary *)atom_getobj(argv);
                ensure_string_capacity(str, capacity, *len + 1);
                (*str)[(*len)++] = '{';
                dictionary_to_json_string_recursive(subdict, str, len, capacity);
                ensure_string_capacity(str, capacity, *len + 1);
                (*str)[(*len)++] = '}';
            } else {
                if (argc > 1) {
                    ensure_string_capacity(str, capacity, *len + 1);
                    (*str)[(*len)++] = '[';
                }
                for (int j = 0; j < argc; j++) {
                    char temp_val[256];
                    int val_len = 0;
                    switch (atom_gettype(argv + j)) {
                        case A_LONG:
                            val_len = snprintf(temp_val, 256, "%ld", atom_getlong(argv + j));
                            break;
                        case A_FLOAT:
                            val_len = snprintf(temp_val, 256, "%f", atom_getfloat(argv + j));
                            break;
                        case A_SYM:
                            val_len = snprintf(temp_val, 256, "\"%s\"", atom_getsym(argv + j)->s_name);
                            break;
                        default:
                            break;
                    }
                    ensure_string_capacity(str, capacity, *len + val_len);
                    strcpy(*str + *len, temp_val);
                    *len += val_len;

                    if (j < argc - 1) {
                        ensure_string_capacity(str, capacity, *len + 1);
                        (*str)[(*len)++] = ',';
                    }
                }
                if (argc > 1) {
                    ensure_string_capacity(str, capacity, *len + 1);
                    (*str)[(*len)++] = ']';
                }
            }
            if (i < numkeys - 1) {
                ensure_string_capacity(str, capacity, *len + 1);
                (*str)[(*len)++] = ',';
            }
            if (argv) {
                sysmem_freeptr(argv);
            }
        }
        sysmem_freeptr(keys);
    }
}

char* dictionary_to_string(t_dictionary *d) {
    long len = 0;
    long capacity = 1024; // Start with a reasonable buffer size
    char *str = (char *)sysmem_newptr(capacity);
    
    str[len++] = '{';
    dictionary_to_json_string_recursive(d, &str, &len, &capacity);
    str[len++] = '}';
    str[len] = '\0'; // Null-terminate the string
    
    // Optional: Resize to the final exact size
    str = (char *)sysmem_resizeptr(str, len + 1);

    return str;
}

void whichoffset_send_dict_in_chunks(t_whichoffset *x, t_dictionary *d) {
    long num_tracks;
    t_symbol **tracks;
    dictionary_getkeys(d, &num_tracks, &tracks);

    if (tracks) {
        for (long i = 0; i < num_tracks; i++) {
            t_symbol *track_key = tracks[i];

            t_atom *track_atom_ptr = NULL;
            long track_argc = 0;
            dictionary_getatoms(d, track_key, &track_argc, &track_atom_ptr);

            if (track_argc == 1 && atom_gettype(track_atom_ptr) == A_OBJ && object_classname(atom_getobj(track_atom_ptr)) == gensym("dictionary")) {
                t_dictionary *track_dict = (t_dictionary *)atom_getobj(track_atom_ptr);

                long num_measures;
                t_symbol **measures;
                dictionary_getkeys(track_dict, &num_measures, &measures);

                if (measures) {
                    for (long j = 0; j < num_measures; j++) {
                        t_symbol *measure_key = measures[j];

                        t_atom *measure_atom_ptr = NULL;
                        long measure_argc = 0;
                        dictionary_getatoms(track_dict, measure_key, &measure_argc, &measure_atom_ptr);

                        if (measure_argc == 1 && atom_gettype(measure_atom_ptr) == A_OBJ && object_classname(atom_getobj(measure_atom_ptr)) == gensym("dictionary")) {
                            t_dictionary *measure_dict = (t_dictionary *)atom_getobj(measure_atom_ptr);
                            t_dictionary *measure_dict_clone = (t_dictionary *)object_clone((t_object *)measure_dict);

                            t_dictionary *final_dict = dictionary_new();
                            t_dictionary *transcript_dict = dictionary_new();
                            t_dictionary *single_track_dict = dictionary_new();

                            dictionary_appenddictionary(single_track_dict, measure_key, (t_object *)measure_dict_clone);
                            dictionary_appenddictionary(transcript_dict, track_key, (t_object *)single_track_dict);
                            dictionary_appenddictionary(final_dict, gensym("transcript"), (t_object *)transcript_dict);

                            char *json_str = dictionary_to_string(final_dict);
                            visualize(json_str);
                            sysmem_freeptr(json_str);

                            object_free(final_dict);
                        }

                        if (measure_atom_ptr) {
                            sysmem_freeptr(measure_atom_ptr);
                        }
                    }
                    sysmem_freeptr(measures);
                }
            }
            if (track_atom_ptr) {
                sysmem_freeptr(track_atom_ptr);
            }
        }
        sysmem_freeptr(tracks);
    }
}

void whichoffset_assist(t_whichoffset *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "(bang) Visualize Dictionary, (int) Set Track, (span <list>) Calculate Offset");
    } else { // ASSIST_OUTLET
        sprintf(s, "Calculated Optimal Offset (float)");
    }
}

t_max_err dictionary_getatoms_nested(t_dictionary *d, const char *path, long *argc, t_atom **argv) {
    *argc = 0;
    *argv = NULL;
    
    char key[256];
    const char *path_ptr = path;
    t_dictionary *current_dict = d;

    while (path_ptr && *path_ptr) {
        const char *separator = strstr(path_ptr, "::");
        
        if (separator) {
            long key_len = separator - path_ptr;
            if (key_len >= 256) return MAX_ERR_GENERIC; 
            
            strncpy(key, path_ptr, key_len);
            key[key_len] = 0;
            
            t_atom *subdict_atom = NULL;
            long subdict_argc = 0;
            t_symbol *key_sym = gensym(key);
            
            if (dictionary_getatoms(current_dict, key_sym, &subdict_argc, &subdict_atom) == MAX_ERR_NONE &&
                subdict_argc == 1 && atom_gettype(subdict_atom) == A_OBJ &&
                object_classname(atom_getobj(subdict_atom)) == gensym("dictionary"))
            {
                current_dict = (t_dictionary *)atom_getobj(subdict_atom);
                path_ptr = separator + 2; 
                sysmem_freeptr(subdict_atom);
            } else {
                if(subdict_atom) sysmem_freeptr(subdict_atom);
                return MAX_ERR_GENERIC;
            }
        } else { 
            return dictionary_getatoms(current_dict, gensym(path_ptr), argc, argv);
        }
    }
    
    return MAX_ERR_GENERIC; 
}

void whichoffset_print_dict(t_dictionary *d, int indent) {
    long numkeys;
    t_symbol **keys;
    long i;

    dictionary_getkeys(d, &numkeys, &keys);
    if (keys) {
        for (i = 0; i < numkeys; i++) {
            t_atom *argv = NULL;
            long argc = 0;
            char indent_str[128];

            memset(indent_str, ' ', indent * 2);
            indent_str[indent * 2] = 0;

            post("%s%s:", indent_str, keys[i]->s_name);

            dictionary_getatoms(d, keys[i], &argc, &argv);

            if (argc > 0 && atom_gettype(argv) == A_OBJ && object_classname(atom_getobj(argv)) == gensym("dictionary")) {
                t_dictionary *subdict = (t_dictionary *)atom_getobj(argv);
                whichoffset_print_dict(subdict, indent + 1);
            } else {
                for (int j = 0; j < argc; j++) {
                    switch (atom_gettype(argv + j)) {
                        case A_LONG:
                            post("%s  %ld", indent_str, atom_getlong(argv + j));
                            break;
                        case A_FLOAT:
                            post("%s  %f", indent_str, atom_getfloat(argv + j));
                            break;
                        case A_SYM:
                            post("%s  %s", indent_str, atom_getsym(argv + j)->s_name);
                            break;
                        default:
                            break;
                    }
                }
            }
            if (argv) {
                sysmem_freeptr(argv);
            }
        }
        sysmem_freeptr(keys);
    }
}
