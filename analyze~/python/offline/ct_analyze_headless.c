#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32) || defined(_WIN64)
#include <direct.h>
#define mkdir_cross(dir) _mkdir(dir)
#else
#define mkdir_cross(dir) mkdir(dir, 0755)
#endif

#include "cumulative_transience.h"

int export_ctbin(const char* output_filepath, const float* y, int len, int sr, int window_ms);

static void get_directory_and_stem(const char* filepath, char* out_dir, char* out_stem) {
    const char* last_slash = strrchr(filepath, '/');
    const char* last_backslash = strrchr(filepath, '\\');
    const char* filename = filepath;

    if (last_slash && last_slash >= filename) filename = last_slash + 1;
    if (last_backslash && last_backslash >= filename) filename = last_backslash + 1;

    const char* dot = strrchr(filename, '.');
    size_t stem_len = dot ? (size_t)(dot - filename) : strlen(filename);

    strncpy(out_stem, filename, stem_len);
    out_stem[stem_len] = '\0';

    size_t dir_len = (size_t)(filename - filepath);
    if (dir_len > 0) {
        strncpy(out_dir, filepath, dir_len);
        out_dir[dir_len] = '\0';
    } else {
        strcpy(out_dir, "./");
    }
}

int main(int argc, char** argv) {
    printf("============================================================\n");
    printf("  Headless Cumulative Transience Audio Analyzer (C Core)\n");
    printf("============================================================\n\n");

    if (argc < 2) {
        printf("Usage: Drag and drop a WAV audio file onto this executable,\n");
        printf("       or run: %s <path_to_audio.wav> [window_ms]\n\n", argv[0]);
        printf("Press Enter to exit...");
        getchar();
        return 1;
    }

    const char* audio_filepath = argv[1];
    int window_ms = (argc >= 3) ? atoi(argv[2]) : 15000;
    if (window_ms <= 0 || window_ms > 15000) {
        window_ms = 15000;
    }

    printf("Loading WAV audio file: %s\n", audio_filepath);

    unsigned int channels;
    unsigned int sample_rate;
    drwav_uint64 total_pcm_frames;

    float* p_sample_data = drwav_open_file_and_read_pcm_frames_f32(
        audio_filepath, &channels, &sample_rate, &total_pcm_frames, NULL
    );

    if (!p_sample_data) {
        printf("Error: Could not open or decode WAV file: %s\n", audio_filepath);
        printf("\nPress Enter to exit...");
        getchar();
        return 1;
    }

    drwav_uint64 mono_frames = total_pcm_frames;

    float* mono_data = (float*)malloc(mono_frames * sizeof(float));
    if (!mono_data) {
        printf("Error: Memory allocation failed for audio buffer.\n");
        drwav_free(p_sample_data, NULL);
        printf("\nPress Enter to exit...");
        getchar();
        return 1;
    }

    if (channels == 1) {
        memcpy(mono_data, p_sample_data, mono_frames * sizeof(float));
    } else {
        for (drwav_uint64 i = 0; i < mono_frames; i++) {
            double sum = 0.0;
            for (unsigned int c = 0; c < channels; c++) {
                sum += p_sample_data[i * channels + c];
            }
            mono_data[i] = (float)(sum / channels);
        }
    }
    drwav_free(p_sample_data, NULL);

    double total_duration_s = (double)mono_frames / (double)sample_rate;
    printf("Loaded audio successfully: %.2f seconds (%llu frames, %u Hz, %u ch)\n",
           total_duration_s, (unsigned long long)mono_frames, sample_rate, channels);

    char dir_path[2048];
    char stem_name[1024];
    get_directory_and_stem(audio_filepath, dir_path, stem_name);

    char output_dir[4096];
    snprintf(output_dir, sizeof(output_dir), "%s%s", dir_path, stem_name);
    mkdir_cross(output_dir);

    char ctbin_path[8192];
    snprintf(ctbin_path, sizeof(ctbin_path), "%s/%s.ctbin", output_dir, stem_name);

    printf("\nRunning headless transience analysis in C core...\n");
    printf("Exporting binary transience stream to: %s\n", ctbin_path);

    int success = export_ctbin(ctbin_path, mono_data, (int)mono_frames, (int)sample_rate, window_ms);
    free(mono_data);

    if (!success) {
        printf("\nError: Headless analysis failed during export_ctbin.\n");
        printf("Press Enter to exit...");
        getchar();
        return 1;
    }

    printf("\n============================================================\n");
    printf("ANALYSIS COMPLETE!\n");
    printf("Output Directory: %s/\n", output_dir);
    printf("Binary Artifact:  %s.ctbin\n", stem_name);
    printf("============================================================\n\n");

    printf("Press Enter to exit...");
    getchar();
    return 0;
}
