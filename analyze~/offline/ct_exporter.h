#ifndef CT_EXPORTER_H
#define CT_EXPORTER_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(_WIN64)
#define PATH_SEP '\\'
#define PATH_SEP_STR "\\"
#else
#define PATH_SEP '/'
#define PATH_SEP_STR "/"
#endif

#include "cumulative_transience.h"

int mkdir_p(const char* path);

int export_ctbin(const char* output_filepath, const float* y, int len, int sr, int window_ms);

int export_all_assets_and_html(
    const char* audio_filepath,
    const char* output_dir,
    const char* stem_name,
    const float* y,
    int len,
    int sr,
    int pass1_window_ms
);

#ifdef __cplusplus
}
#endif

#endif // CT_EXPORTER_H
