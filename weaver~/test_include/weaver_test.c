#include "max_mock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Global log file for redirecting Max messages and outlet outputs
FILE *g_log_file = NULL;

// Declare the weaver~ internal functions we need to call
void *weaver_new(t_symbol *s, long argc, t_atom *argv);
void weaver_free(void *x);
void weaver_process_vector(void *x, double *ramp_in, long sampleframes);
void weaver_audio_qtask(void *x);

// Forward declarations for JSON parser
typedef struct {
    const char *json;
    int pos;
} t_json_parser;

static void json_skip_whitespace(t_json_parser *p) {
    while (p->json[p->pos] && (p->json[p->pos] == ' ' || p->json[p->pos] == '\t' ||
                               p->json[p->pos] == '\n' || p->json[p->pos] == '\r')) {
        p->pos++;
    }
}

static char *json_parse_string(t_json_parser *p) {
    json_skip_whitespace(p);
    if (p->json[p->pos] != '"') return NULL;
    p->pos++; // skip initial '"'
    int start = p->pos;
    while (p->json[p->pos] && p->json[p->pos] != '"') {
        if (p->json[p->pos] == '\\' && p->json[p->pos + 1]) p->pos++;
        p->pos++;
    }
    int len = p->pos - start;
    char *str = malloc(len + 1);
    strncpy(str, p->json + start, len);
    str[len] = '\0';
    if (p->json[p->pos] == '"') p->pos++; // skip closing '"'
    return str;
}

static int json_parse_value(t_json_parser *p, t_atom *out_atom);

static t_dictionary *json_parse_object(t_json_parser *p) {
    json_skip_whitespace(p);
    if (p->json[p->pos] != '{') return NULL;
    p->pos++; // skip '{'

    t_dictionary *dict = dictionary_new();

    while (1) {
        json_skip_whitespace(p);
        if (p->json[p->pos] == '}') {
            p->pos++;
            break;
        }

        char *key_str = json_parse_string(p);
        if (!key_str) {
            // parsing error or empty object
            break;
        }

        json_skip_whitespace(p);
        if (p->json[p->pos] != ':') {
            free(key_str);
            break;
        }
        p->pos++; // skip ':'

        t_atom val;
        memset(&val, 0, sizeof(t_atom));
        if (json_parse_value(p, &val)) {
            dictionary_appendatom(dict, gensym(key_str), &val);
        }
        free(key_str);

        json_skip_whitespace(p);
        if (p->json[p->pos] == ',') {
            p->pos++;
        } else if (p->json[p->pos] == '}') {
            p->pos++;
            break;
        } else {
            break; // error
        }
    }
    return dict;
}

static t_atomarray *json_parse_array(t_json_parser *p) {
    json_skip_whitespace(p);
    if (p->json[p->pos] != '[') return NULL;
    p->pos++; // skip '['

    int count = 0;
    int capacity = 8;
    t_atom *atoms = malloc(capacity * sizeof(t_atom));

    while (1) {
        json_skip_whitespace(p);
        if (p->json[p->pos] == ']') {
            p->pos++;
            break;
        }

        t_atom val;
        memset(&val, 0, sizeof(t_atom));
        if (json_parse_value(p, &val)) {
            if (count >= capacity) {
                capacity *= 2;
                atoms = realloc(atoms, capacity * sizeof(t_atom));
            }
            atoms[count++] = val;
        }

        json_skip_whitespace(p);
        if (p->json[p->pos] == ',') {
            p->pos++;
        } else if (p->json[p->pos] == ']') {
            p->pos++;
            break;
        } else {
            break; // error
        }
    }
    t_atomarray *aa = atomarray_new(count, atoms);
    free(atoms);
    return aa;
}

static int json_parse_value(t_json_parser *p, t_atom *out_atom) {
    json_skip_whitespace(p);
    char c = p->json[p->pos];

    if (c == '{') {
        t_dictionary *dict = json_parse_object(p);
        if (dict) {
            out_atom->a_type = A_OBJ;
            out_atom->a_w.w_obj = (t_object *)dict;
            return 1;
        }
    } else if (c == '[') {
        t_atomarray *aa = json_parse_array(p);
        if (aa) {
            out_atom->a_type = A_OBJ;
            out_atom->a_w.w_obj = (t_object *)aa;
            return 1;
        }
    } else if (c == '"') {
        char *str = json_parse_string(p);
        if (str) {
            out_atom->a_type = A_SYM;
            out_atom->a_w.w_sym = gensym(str);
            free(str);
            return 1;
        }
    } else if ((c >= '0' && c <= '9') || c == '-' || c == '+') {
        // Parse number
        int start = p->pos;
        int is_float = 0;
        if (p->json[p->pos] == '-' || p->json[p->pos] == '+') p->pos++;
        while (p->json[p->pos] && ((p->json[p->pos] >= '0' && p->json[p->pos] <= '9') || p->json[p->pos] == '.' ||
                                   p->json[p->pos] == 'e' || p->json[p->pos] == 'E' || p->json[p->pos] == '-' || p->json[p->pos] == '+')) {
            if (p->json[p->pos] == '.') is_float = 1;
            p->pos++;
        }
        int len = p->pos - start;
        char *num_str = malloc(len + 1);
        strncpy(num_str, p->json + start, len);
        num_str[len] = '\0';

        if (is_float) {
            out_atom->a_type = A_FLOAT;
            out_atom->a_w.w_float = atof(num_str);
        } else {
            out_atom->a_type = A_LONG;
            out_atom->a_w.w_long = atol(num_str);
        }
        free(num_str);
        return 1;
    } else if (strncmp(p->json + p->pos, "true", 4) == 0) {
        out_atom->a_type = A_LONG;
        out_atom->a_w.w_long = 1;
        p->pos += 4;
        return 1;
    } else if (strncmp(p->json + p->pos, "false", 5) == 0) {
        out_atom->a_type = A_LONG;
        out_atom->a_w.w_long = 0;
        p->pos += 5;
        return 1;
    } else if (strncmp(p->json + p->pos, "null", 4) == 0) {
        out_atom->a_type = A_NOTHING;
        p->pos += 4;
        return 1;
    }
    return 0;
}

t_dictionary *load_transcript_from_json(const char *filepath) {
    FILE *f = fopen(filepath, "r");
    if (!f) {
        printf("Could not open JSON file %s\n", filepath);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *json_str = malloc(size + 1);
    fread(json_str, 1, size, f);
    json_str[size] = '\0';
    fclose(f);

    t_json_parser p;
    p.json = json_str;
    p.pos = 0;

    t_dictionary *dict = json_parse_object(&p);
    free(json_str);
    return dict;
}

t_dictionary *synthesize_transcript() {
    printf("Synthesizing transcript dictionary with negative bars...\n");
    t_dictionary *root = dictionary_new();

    // We will create 2 tracks: Track 1 and Track 2
    for (int t = 1; t <= 2; t++) {
        t_dictionary *track_dict = dictionary_new();
        char track_id_str[16];
        snprintf(track_id_str, sizeof(track_id_str), "%d", t);

        // Add 4 bars per track at 2526 ms interval: -5052, -2526, 0, 2526
        for (int b = -2; b <= 1; b++) {
            t_dictionary *bar_dict = dictionary_new();
            char bar_ts_str[16];
            snprintf(bar_ts_str, sizeof(bar_ts_str), "%d", b * 2526);

            // palette
            t_atom pal;
            pal.a_type = A_SYM;
            pal.a_w.w_sym = gensym(t == 1 ? "pal_A.wav" : "pal_B.wav");
            dictionary_appendatom(bar_dict, gensym("palette"), &pal);

            // offset
            t_atom off;
            off.a_type = A_FLOAT;
            off.a_w.w_float = (double)((b + 2) * 100);
            dictionary_appendatom(bar_dict, gensym("offset"), &off);

            // rating
            t_atom rat;
            rat.a_type = A_FLOAT;
            rat.a_w.w_float = 0.95 - (double)(b + 2) * 0.1; // rating decreases slightly
            dictionary_appendatom(bar_dict, gensym("rating"), &rat);

            // absolutes (either array or single atom)
            if ((b + 2) % 2 == 0) {
                t_atom abs_atom;
                abs_atom.a_type = A_FLOAT;
                abs_atom.a_w.w_float = 12000.0 + (b + 2) * 2526;
                dictionary_appendatom(bar_dict, gensym("absolutes"), &abs_atom);

                t_atom sc_atom;
                sc_atom.a_type = A_FLOAT;
                sc_atom.a_w.w_float = 0.85;
                dictionary_appendatom(bar_dict, gensym("scores"), &sc_atom);
            } else {
                t_atom abs_atoms[2];
                abs_atoms[0].a_type = A_FLOAT;
                abs_atoms[0].a_w.w_float = 12000.0 + (b + 2) * 2526;
                abs_atoms[1].a_type = A_FLOAT;
                abs_atoms[1].a_w.w_float = 13000.0 + (b + 2) * 2526;
                t_atomarray *abs_aa = atomarray_new(2, abs_atoms);
                dictionary_appendatomarray(bar_dict, gensym("absolutes"), (t_object *)abs_aa);

                t_atom sc_atoms[2];
                sc_atoms[0].a_type = A_FLOAT;
                sc_atoms[0].a_w.w_float = 0.9;
                sc_atoms[1].a_type = A_FLOAT;
                sc_atoms[1].a_w.w_float = 0.8;
                t_atomarray *sc_aa = atomarray_new(2, sc_atoms);
                dictionary_appendatomarray(bar_dict, gensym("scores"), (t_object *)sc_aa);
            }

            t_atom bar_ts_atom;
            bar_ts_atom.a_type = A_LONG;
            bar_ts_atom.a_w.w_long = b * 2526;
            t_atomarray *span_aa = atomarray_new(1, &bar_ts_atom);
            dictionary_appendatomarray(bar_dict, gensym("span"), (t_object *)span_aa);

            t_atom bar_dict_atom;
            bar_dict_atom.a_type = A_OBJ;
            bar_dict_atom.a_w.w_obj = (t_object *)bar_dict;
            dictionary_appendatom(track_dict, gensym(bar_ts_str), &bar_dict_atom);
        }

        t_atom track_dict_atom;
        track_dict_atom.a_type = A_OBJ;
        track_dict_atom.a_w.w_obj = (t_object *)track_dict;
        dictionary_appendatom(root, gensym(track_id_str), &track_dict_atom);
    }

    return root;
}

int main(int argc, char **argv) {
    printf("DEBUG: Entering main...\n"); fflush(stdout);

    // Static arrays to avoid stack overflow
    static float pal_a_samples[2526 * 44];
    static float pal_b_samples[2526 * 44];
    static float stems_1_samples[50000];
    static float stems_2_samples[50000];
    static float dest_poly_1[200000];
    static float dest_poly_2[200000];

    printf("DEBUG: Opening log file...\n"); fflush(stdout);
    // Open the log file
    g_log_file = fopen("weaver_verbose_log.txt", "w");
    if (!g_log_file) {
        printf("Could not create log file weaver_verbose_log.txt!\n");
        return 1;
    }

    fprintf(g_log_file, "=========================================\n");
    fprintf(g_log_file, "Starting Weaver~ Standalone Unit Test Runner\n");
    fprintf(g_log_file, "=========================================\n\n");

    printf("DEBUG: Initializing common symbols...\n"); fflush(stdout);
    common_symbols_init();

    // Step 1: Set up mock buffers
    printf("Registering mock audio buffers...\n"); fflush(stdout);

    // Bar buffer~: contains bar length in the first sample
    float bar_length_sample[1] = { 2526.0f };
    mock_register_buffer("bar", bar_length_sample, 1, 1, 44100.0);

    // Fill with simple test signal (alternating positive/negative samples)
    for (int i = 0; i < 2526 * 44; i++) {
        pal_a_samples[i] = (i % 2 == 0) ? 0.5f : -0.5f;
        pal_b_samples[i] = (i % 2 == 0) ? 0.3f : -0.3f;
    }
    mock_register_buffer("pal_A.wav", pal_a_samples, 2526 * 44, 1, 44100.0);
    mock_register_buffer("pal_B.wav", pal_b_samples, 2526 * 44, 1, 44100.0);

    // Fill fallback stems buffers with non-zero samples as well
    for (int i = 0; i < 50000; i++) {
        stems_1_samples[i] = (i % 2 == 0) ? 0.4f : -0.4f;
        stems_2_samples[i] = (i % 2 == 0) ? 0.2f : -0.2f;
    }
    mock_register_buffer("stems.1", stems_1_samples, 50000, 1, 44100.0);
    mock_register_buffer("stems.2", stems_2_samples, 50000, 1, 44100.0);

    mock_register_buffer("poly.1", dest_poly_1, 200000, 1, 44100.0);
    mock_register_buffer("poly.2", dest_poly_2, 200000, 1, 44100.0);

    // Step 2: Load or synthesize transcript dictionary
    t_dictionary *transcript = NULL;
    if (argc > 1) {
        printf("Loading transcript dictionary from JSON: %s...\n", argv[1]); fflush(stdout);
        transcript = load_transcript_from_json(argv[1]);
    }

    if (!transcript) {
        printf("Falling back to programmatically synthesized transcript...\n"); fflush(stdout);
        transcript = synthesize_transcript();
    }

    if (!transcript) {
        printf("CRITICAL ERROR: Failed to obtain a transcript dictionary!\n"); fflush(stdout);
        fclose(g_log_file);
        return 1;
    }

    printf("DEBUG: Registering dict...\n"); fflush(stdout);
    // Register the dictionary in our global mock registry so dictobj_findregistered_retain can find it!
    mock_register_dict("my_transcript_dict", transcript);

    // Step 3: Instantiate weaver~ object
    printf("Instantiating weaver~ object...\n"); fflush(stdout);
    t_atom args[2];
    atom_setsym(&args[0], gensym("my_transcript_dict"));
    atom_setsym(&args[1], gensym("poly"));

    printf("DEBUG: Calling weaver_new...\n"); fflush(stdout);
    // Instantiate!
    void *x = weaver_new(gensym("weaver~"), 2, args);
    printf("DEBUG: weaver_new returned %p...\n", x); fflush(stdout);
    if (!x) {
        printf("CRITICAL ERROR: Failed to instantiate weaver~ object!\n"); fflush(stdout);
        fclose(g_log_file);
        return 1;
    }

    // Enable logging and visualization by modifying attributes directly on our mock-allocated object
    printf("Enabling log and visualize attributes...\n"); fflush(stdout);
    // Layout-agnostic attribute modification
    t_atom log_atom, viz_atom;
    atom_setlong(&log_atom, 1);
    atom_setlong(&viz_atom, 1);

    // Under our mock we can call class setter or directly modify the memory using offset since we know struct_weaver layout:
    // log is the 3rd field, visualize is the 4th field after t_obj
    struct {
        t_pxobject t_obj;
        t_symbol *poly_prefix;
        long log;
        long visualize;
    } *min_w = (void *)x;
    min_w->log = 1;
    min_w->visualize = 1;

    // Step 4: Run continuous simulated ramp signal to song end
    double song_length = 2526.0 * 4.0; // 4 bars * 2526ms = 10104ms
    double sr = 44100.0;
    long vector_size = 512;
    double ms_per_vector = (double)vector_size * 1000.0 / sr;

    printf("Starting simulated audio ramp from 0.00 to %.2f ms (Step size: %.2f ms)...\n", song_length, ms_per_vector); fflush(stdout);

    double simulated_ramp[512];
    double current_time_ms = 0.0;

    g_use_mock_time = 1;
    g_mock_time_ms = 100.0; // initial start time

    int vector_count = 0;
    while (current_time_ms < song_length) {
        // Fill the simulated ramp vector (starts at current_time_ms and increments linearly)
        for (int i = 0; i < vector_size; i++) {
            simulated_ramp[i] = current_time_ms + ((double)i * 1000.0 / sr);
        }

        // 1. Process audio vector
        weaver_process_vector(x, simulated_ramp, vector_size);

        // 2. Process main-thread qelem queue (events handoff, logging, visualization)
        weaver_audio_qtask(x);

        current_time_ms += ms_per_vector;
        g_mock_time_ms += ms_per_vector; // Advance the wall clock so visualization doesn't throttle!
        vector_count++;

        if (vector_count % 100 == 0) {
            printf("  Processed %.2f ms / %.2f ms (%.1f%%)...\n", current_time_ms, song_length, (current_time_ms / song_length) * 100.0); fflush(stdout);
        }
    }

    // Run an extra few vectors past the end to complete any pending crossfades or loops
    printf("Simulating extra overrun vectors to let fades complete...\n"); fflush(stdout);
    for (int k = 0; k < 20; k++) {
        for (int i = 0; i < vector_size; i++) {
            simulated_ramp[i] = current_time_ms + ((double)i * 1000.0 / sr);
        }
        weaver_process_vector(x, simulated_ramp, vector_size);
        weaver_audio_qtask(x);
        current_time_ms += ms_per_vector;
        g_mock_time_ms += ms_per_vector;
    }

    printf("DSP simulation run completed successfully!\n"); fflush(stdout);

    // Let's print some statistics on the destination buffers
    printf("\nDestination Buffer Diagnostics:\n"); fflush(stdout);
    int has_non_zero_1 = 0;
    int has_non_zero_2 = 0;
    for (int i = 0; i < 200000; i++) {
        if (dest_poly_1[i] != 0.0f) has_non_zero_1 = 1;
        if (dest_poly_2[i] != 0.0f) has_non_zero_2 = 1;
    }
    printf("  poly.1 received written audio: %s\n", has_non_zero_1 ? "YES (SUCCESS)" : "NO"); fflush(stdout);
    printf("  poly.2 received written audio: %s\n", has_non_zero_2 ? "YES (SUCCESS)" : "NO"); fflush(stdout);

    // Free the weaver instance
    printf("\nFreeing weaver~ object...\n"); fflush(stdout);
    weaver_free(x);

    printf("Standalone test execution finished successfully. All details printed to weaver_verbose_log.txt.\n"); fflush(stdout);

    fprintf(g_log_file, "\n=========================================\n");
    fprintf(g_log_file, "Weaver~ Standalone Unit Test Finished Successfully\n");
    fprintf(g_log_file, "=========================================\n");

    fclose(g_log_file);
    return 0;
}
