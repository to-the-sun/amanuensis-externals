#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <float.h>
#include <math.h>
#include <json-c/json.h>
#include "sound_design.h"
#include "analysis_utils.h"

int main(int argc, char** argv) {
    int sr = 44100;

    char sounds_dir[] = "sounds";
    char version_str[16];
    sprintf(version_str, "%d", SOUND_DESIGN_VERSION);

    char subfolder[256];
    sprintf(subfolder, "%s/%s", sounds_dir, version_str);

    mkdir(sounds_dir, 0755);
    mkdir(subfolder, 0755);

    char old_wav[512];
    sprintf(old_wav, "%s/design_output.wav", subfolder);
    unlink(old_wav);

    printf("Rendering diagnostic probes for version %d...\n", SOUND_DESIGN_VERSION);

    struct json_object* probes_obj = json_object_new_object();
    double probe_peaks[NUM_PROBES];
    int probe_max_vels[NUM_PROBES];
    double probe_expected_peaks[NUM_PROBES];

    for (int p = 0; p < NUM_PROBES; p++) {
        ProbeConfig* cfg = &PROBE_CONFIGS[p];
        int num_samples = 0;
        double* audio = render_midi(cfg->sequence, cfg->sequence_len, cfg->duration, sr, &num_samples);

        if (cfg->save_wav_filename != NULL) {
            char output_path[512];
            sprintf(output_path, "%s/%s", subfolder, cfg->save_wav_filename);
            save_wav(output_path, audio, num_samples, sr);
            printf("Saved audio probe: %s\n", output_path);
        }

        double peak = 0.0;
        for (int k = 0; k < num_samples; k++) {
            double abs_val = fabs(audio[k]);
            if (abs_val > peak) peak = abs_val;
        }
        probe_peaks[p] = peak;

        int max_v = 0;
        for (int m = 0; m < cfg->sequence_len; m++) {
            if (strcmp(cfg->sequence[m].type, "note_on") == 0 && cfg->sequence[m].velocity > max_v) {
                max_v = cfg->sequence[m].velocity;
            }
        }
        probe_max_vels[p] = max_v;
        probe_expected_peaks[p] = (double)max_v / 127.0;

        struct json_object* probe_res = analyze_audio(audio, num_samples, sr);
        json_object_object_add(probes_obj, cfg->name, probe_res);
        free(audio);
    }

    printf("\n=== Volume Calibration Analysis ===\n");
    printf("%-20s %-10s %-15s %-15s %-10s\n", "Probe", "Max Vel", "Observed Peak", "Expected Peak", "Status");
    printf("--------------------------------------------------------------------\n");
    int all_calibrated = 1;
    for (int p = 0; p < NUM_PROBES; p++) {
        ProbeConfig* cfg = &PROBE_CONFIGS[p];
        double diff = fabs(probe_peaks[p] - probe_expected_peaks[p]);
        const char* status = (diff <= 0.01) ? "OK" : "WARN";
        if (diff > 0.01) all_calibrated = 0;
        printf("%-20s %-10d %-15.6f %-15.6f %-10s\n",
               cfg->name, probe_max_vels[p], probe_peaks[p], probe_expected_peaks[p], status);
    }
    if (all_calibrated) {
        printf("Volume calibration verified: Linear amplitude scaling matched across full range.\n");
    } else {
        printf("Notice: Some probes deviate from expected linear amplitude scaling.\n");
    }
    printf("--------------------------------------------------------------------\n\n");

    char cmd[1024];
    sprintf(cmd, "cp sound_design.c sound_design.h %s/", subfolder);
    int sys_res = system(cmd);
    (void)sys_res;

    struct json_object* distances = json_object_new_object();
    double min_dist = DBL_MAX;

    DIR *dir;
    struct dirent *ent;
    if ((dir = opendir(sounds_dir)) != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_type == DT_DIR && strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0 && strcmp(ent->d_name, version_str) != 0) {
                char other_json_path[512];
                sprintf(other_json_path, "%s/%s/analysis.json", sounds_dir, ent->d_name);
                struct json_object* other_results = json_object_from_file(other_json_path);
                if (other_results) {
                    double dist = calculate_distance(probes_obj, other_results);
                    json_object_object_add(distances, ent->d_name, json_object_new_double(dist));
                    if (dist < min_dist) {
                        min_dist = dist;
                    }
                    json_object_put(other_results);
                }
            }
        }
        closedir(dir);
    }

    double uniqueness_score = (min_dist == DBL_MAX) ? 0.0 : min_dist;

    struct json_object* results = json_object_new_object();
    json_object_object_add(results, "uniqueness_score", json_object_new_double(uniqueness_score));
    json_object_object_add(results, "distances", distances);
    json_object_object_add(results, "probes", probes_obj);

    char json_path[512];
    sprintf(json_path, "%s/analysis.json", subfolder);
    json_object_to_file_ext(json_path, results, JSON_C_TO_STRING_PRETTY);
    printf("Analysis saved to %s (Uniqueness score: %.6f)\n", json_path, uniqueness_score);

    json_object_put(results);
    return 0;
}
