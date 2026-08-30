#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <dlfcn.h>
#include <float.h>
#include <json-c/json.h>
#include "sound_design.h"
#include "analysis_utils.h"

int main() {
    char sounds_dir[] = "sounds";
    DIR *dir;
    struct dirent *ent;
    struct json_object* all_probes = json_object_new_object();

    if ((dir = opendir(sounds_dir)) != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_type == DT_DIR && strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
                printf("Processing sound %s...\n", ent->d_name);
                char folder_path[512]; sprintf(folder_path, "%s/%s", sounds_dir, ent->d_name);
                char src_path[512]; sprintf(src_path, "%s/sound_design.c", folder_path);
                if (access(src_path, F_OK) == -1) { printf("Skipping %s, sound_design.c not found.\n", ent->d_name); continue; }

                char old_wav[512]; sprintf(old_wav, "%s/design_output.wav", folder_path);
                unlink(old_wav);

                char cmd[1024];
                sprintf(cmd, "gcc -fPIC -shared -o %s/libsd.so %s -lm", folder_path, src_path);
                if (system(cmd) != 0) { fprintf(stderr, "Failed to compile %s\n", src_path); continue; }

                char lib_path[512]; sprintf(lib_path, "./%s/libsd.so", folder_path);
                void *handle = dlopen(lib_path, RTLD_NOW);
                if (!handle) { fprintf(stderr, "%s\n", dlerror()); continue; }

                double* (*render_midi_ptr)(MidiMessage*, int, double, int, int*);
                render_midi_ptr = dlsym(handle, "render_midi");
                if (render_midi_ptr) {
                    struct json_object* probes_obj = json_object_new_object();
                    for (int p = 0; p < NUM_PROBES; p++) {
                        ProbeConfig* cfg = &PROBE_CONFIGS[p];
                        int num_samples = 0;
                        double* audio = render_midi_ptr(cfg->sequence, cfg->sequence_len, cfg->duration, 44100, &num_samples);

                        if (cfg->save_wav_filename != NULL) {
                            char output_path[512];
                            sprintf(output_path, "%s/%s", folder_path, cfg->save_wav_filename);
                            save_wav(output_path, audio, num_samples, 44100);
                        }

                        struct json_object* probe_res = analyze_audio(audio, num_samples, 44100);
                        json_object_object_add(probes_obj, cfg->name, probe_res);
                        free(audio);
                    }
                    json_object_object_add(all_probes, ent->d_name, probes_obj);
                }
                dlclose(handle); unlink(lib_path);
            }
        }
        closedir(dir);
    }

    json_object_object_foreach(all_probes, key, probes1) {
        struct json_object* distances = json_object_new_object();
        double min_dist = DBL_MAX;

        json_object_object_foreach(all_probes, other_key, probes2) {
            if (strcmp(key, other_key) != 0) {
                double dist = calculate_distance(probes1, probes2);
                json_object_object_add(distances, other_key, json_object_new_double(dist));
                if (dist < min_dist) {
                    min_dist = dist;
                }
            }
        }

        double uniqueness_score = (min_dist == DBL_MAX) ? 0.0 : min_dist;

        struct json_object* results = json_object_new_object();
        json_object_object_add(results, "uniqueness_score", json_object_new_double(uniqueness_score));
        json_object_object_add(results, "distances", distances);

        json_object_object_add(results, "probes", json_object_get(probes1));

        char json_path[512]; sprintf(json_path, "%s/%s/analysis.json", sounds_dir, key);
        json_object_to_file_ext(json_path, results, JSON_C_TO_STRING_PRETTY);
        printf("Updated %s (Uniqueness score: %.6f)\n", json_path, uniqueness_score);
        json_object_put(results);
    }

    json_object_put(all_probes);
    return 0;
}
