#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>

#if defined(_WIN32) || defined(_WIN64)
#include <direct.h>
#define mkdir_cross(dir) _mkdir(dir)
#else
#define mkdir_cross(dir) mkdir(dir, 0755)
#endif

#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & S_IFDIR) == S_IFDIR)
#endif

#include "cumulative_transience.h"
#include "ct_exporter.h"

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

static int iequals(const char* a, const char* b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static int has_wav_extension(const char* filename) {
    size_t len = strlen(filename);
    if (len < 4) return 0;
    const char* ext = filename + len - 4;
    return iequals(ext, ".wav");
}

static int is_excluded_filename(const char* filename) {
    if (iequals(filename, "preview.wav") || iequals(filename, "glued.wav")) {
        return 1;
    }
    return 0;
}

typedef struct {
    char** items;
    int count;
    int capacity;
} FileList;

static void file_list_init(FileList* list) {
    list->count = 0;
    list->capacity = 16;
    list->items = (char**)malloc(list->capacity * sizeof(char*));
}

static void file_list_add(FileList* list, const char* name) {
    if (list->count >= list->capacity) {
        list->capacity *= 2;
        list->items = (char**)realloc(list->items, list->capacity * sizeof(char*));
    }
    list->items[list->count] = strdup(name);
    list->count++;
}

static void file_list_free(FileList* list) {
    for (int i = 0; i < list->count; i++) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int compare_strings(const void* a, const void* b) {
    return strcmp(*(const char**)a, *(const char**)b);
}

static int process_single_file(const char* audio_filepath, int window_ms) {
    printf("\n------------------------------------------------------------\n");
    printf("Loading WAV audio file: %s\n", audio_filepath);

    unsigned int channels;
    unsigned int sample_rate;
    drwav_uint64 total_pcm_frames;

    float* p_sample_data = drwav_open_file_and_read_pcm_frames_f32(
        audio_filepath, &channels, &sample_rate, &total_pcm_frames, NULL
    );

    if (!p_sample_data) {
        printf("Error: Could not open or decode WAV file: %s\n", audio_filepath);
        return 0;
    }

    drwav_uint64 mono_frames = total_pcm_frames;

    float* mono_data = (float*)malloc(mono_frames * sizeof(float));
    if (!mono_data) {
        printf("Error: Memory allocation failed for audio buffer.\n");
        drwav_free(p_sample_data, NULL);
        return 0;
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

    printf("\nRunning standalone transience analysis, pattern finding, & asset exports in pure C...\n");

    int success = export_all_assets_and_html(
        audio_filepath,
        output_dir,
        stem_name,
        mono_data,
        (int)mono_frames,
        (int)sample_rate,
        window_ms
    );
    free(mono_data);

    if (!success) {
        printf("\nError: Standalone analysis failed during asset export for %s.\n", audio_filepath);
        return 0;
    }

    printf("\n============================================================\n");
    printf("ASSETS & HTML REPORT GENERATED IN PURE C FOR: %s\n", stem_name);
    printf("Output Directory: %s/\n", output_dir);
    printf("Assets Exported:  .ctbin, manifest.json, snapshots.bin, snapshots.js,\n");
    printf("                  report_data.json, report_data.js, pattern WAVs,\n");
    printf("                  %s_pattern_analysis.html\n", stem_name);
    printf("============================================================\n");

    return 1;
}

int main(int argc, char** argv) {
    printf("============================================================\n");
    printf("  Standalone C Cumulative Transience Analyzer & Exporter\n");
    printf("============================================================\n\n");

    if (argc >= 2) {
        // Drag and drop or command line single-file argument provided
        const char* audio_filepath = argv[1];
        int window_ms = (argc >= 3) ? atoi(argv[2]) : 15000;
        if (window_ms <= 0 || window_ms > 15000) {
            window_ms = 15000;
        }

        int success = process_single_file(audio_filepath, window_ms);

        printf("\nPress Enter to exit...");
        getchar();
        return success ? 0 : 1;
    }

    // No file arguments passed (e.g. double-clicked executable)
    // Scan executable directory for all .wav files (excluding preview.wav and glued.wav)
    char exe_dir[2048];
    char exe_stem[1024];
    get_directory_and_stem(argv[0], exe_dir, exe_stem);

    char scan_dir[2048];
    strncpy(scan_dir, exe_dir, sizeof(scan_dir) - 1);
    scan_dir[sizeof(scan_dir) - 1] = '\0';
    size_t scan_len = strlen(scan_dir);
    if (scan_len > 1 && (scan_dir[scan_len - 1] == '/' || scan_dir[scan_len - 1] == '\\')) {
        scan_dir[scan_len - 1] = '\0';
    }

    DIR* dir = opendir(scan_dir);
    if (!dir) {
        dir = opendir(".");
        strcpy(exe_dir, "./");
    }

    FileList wav_files;
    file_list_init(&wav_files);

    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            const char* name = entry->d_name;
            if (!has_wav_extension(name)) continue;
            if (is_excluded_filename(name)) continue;

            // Check if entry is a directory
            char full_path[4096];
            snprintf(full_path, sizeof(full_path), "%s%s", exe_dir, name);
            struct stat st;
            if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                continue;
            }

            file_list_add(&wav_files, name);
        }
        closedir(dir);
    }

    if (wav_files.count == 0) {
        printf("No WAV files found to analyze in directory: %s\n", exe_dir);
        printf("(Explicitly excluded files named 'preview.wav' or 'glued.wav')\n\n");
        file_list_free(&wav_files);
        printf("Press Enter to exit...");
        getchar();
        return 0;
    }

    qsort(wav_files.items, wav_files.count, sizeof(char*), compare_strings);

    printf("Found %d WAV file(s) in %s (excluding preview.wav & glued.wav):\n", wav_files.count, exe_dir);
    for (int i = 0; i < wav_files.count; i++) {
        printf("  [%d/%d] %s\n", i + 1, wav_files.count, wav_files.items[i]);
    }

    int success_count = 0;
    int fail_count = 0;
    int window_ms = 15000;

    for (int i = 0; i < wav_files.count; i++) {
        char full_path[4096];
        snprintf(full_path, sizeof(full_path), "%s%s", exe_dir, wav_files.items[i]);
        printf("\n============================================================\n");
        printf(" Processing [%d/%d]: %s\n", i + 1, wav_files.count, wav_files.items[i]);
        printf("============================================================\n");

        if (process_single_file(full_path, window_ms)) {
            success_count++;
        } else {
            fail_count++;
        }
    }

    file_list_free(&wav_files);

    printf("\n============================================================\n");
    printf("BATCH ANALYSIS COMPLETE!\n");
    printf("Processed %d file(s) successfully (%d failed).\n", success_count, fail_count);
    printf("============================================================\n\n");

    printf("Press Enter to exit...");
    getchar();
    return (fail_count == 0) ? 0 : 1;
}
