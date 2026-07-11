#include "ext.h"
#include "ext_obex.h"
#include "ext_systhread.h"
#include "ext_critical.h"
#include "z_dsp.h"
#include "../shared/logging.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <windows.h>

#define RING_BUFFER_SIZE_SECONDS 10

typedef struct _video {
    t_pxobject v_obj;
    t_symbol *save_path;
    t_symbol *pending_save_path;
    double sr;

    // Outlets
    void *status_outlet;
    void *log_outlet;

    // Ring buffer for audio signal
    double *ring_buffer;
    long long ring_size;
    long long ring_write_ptr;
    long long ring_read_ptr;
    t_critical ring_lock;

    // Recording and Thread status
    volatile int is_recording;
    volatile int is_thread_active;
    long long total_samples_recorded;

    // Thread & System process handles
    t_systhread worker_thread;
    HANDLE hFfmpegProcess;
    HANDLE hFfmpegStdinWrite;
    FILE *wav_file;

    // Temporary & final file paths
    char temp_audio_path[1024];
    char temp_video_path[1024];
    char final_video_path[1024];

    // Status queuing for main-thread outlets
    void *status_qelem;
    char q_status_msg[1024];
    char q_status_arg[1024];
} t_video;

// Function prototypes
void *video_new(t_symbol *s, long argc, t_atom *argv);
void video_free(t_video *x);
void video_assist(t_video *x, void *b, long m, long a, char *s);
void video_int(t_video *x, long n);
void video_float(t_video *x, double f);
void video_path(t_video *x, t_symbol *s);
void video_dsp64(t_video *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
void video_perform64(t_video *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);
void video_status_qfn(t_video *x);
void *video_worker_thread(t_video *x);
void get_object_directory(char *dir_out, size_t max_len);
void write_wav_header(FILE *f, int samplerate, long num_samples);
int start_ffmpeg_screen_recording(t_video *x, const char *temp_video_path);
void stop_ffmpeg_screen_recording(t_video *x);

static t_class *video_class;

void ext_main(void *r) {
    common_symbols_init();
    t_class *c = class_new("video~", (method)video_new, (method)video_free, sizeof(t_video), 0L, A_GIMME, 0);

    class_addmethod(c, (method)video_int, "int", A_LONG, 0);
    class_addmethod(c, (method)video_float, "float", A_FLOAT, 0);
    class_addmethod(c, (method)video_path, "path", A_SYM, 0);
    class_addmethod(c, (method)video_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)video_assist, "assist", A_CANT, 0);

    class_dspinit(c);
    class_register(CLASS_BOX, c);
    video_class = c;
}

void get_object_directory(char *dir_out, size_t max_len) {
    HMODULE hModule = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)get_object_directory, &hModule)) {
        char path[MAX_PATH];
        DWORD len = GetModuleFileNameA(hModule, path, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            char *last_sep = strrchr(path, '\\');
            if (!last_sep) {
                last_sep = strrchr(path, '/');
            }
            if (last_sep) {
                *last_sep = '\0';
                strncpy(dir_out, path, max_len);
                dir_out[max_len - 1] = '\0';
                return;
            }
        }
    }
    strncpy(dir_out, ".", max_len);
    dir_out[max_len - 1] = '\0';
}

void *video_new(t_symbol *s, long argc, t_atom *argv) {
    t_video *x = (t_video *)object_alloc(video_class);

    if (x) {
        dsp_setup((t_pxobject *)x, 1);

        // Outlets (Right-to-Left)
        x->log_outlet = outlet_new((t_object *)x, NULL);
        x->status_outlet = outlet_new((t_object *)x, NULL);

        x->sr = 44100.0;
        x->is_recording = 0;
        x->is_thread_active = 0;
        x->total_samples_recorded = 0;
        x->worker_thread = NULL;
        x->hFfmpegProcess = NULL;
        x->hFfmpegStdinWrite = NULL;
        x->wav_file = NULL;

        // Path Parsing & Formatting
        t_symbol *arg_path = NULL;
        if (argc > 0 && argv && atom_gettype(argv) == A_SYM) {
            arg_path = atom_getsym(argv);
        }

        char path_buf[1024];
        if (arg_path && strlen(arg_path->s_name) > 0) {
            strncpy(path_buf, arg_path->s_name, sizeof(path_buf));
            path_buf[sizeof(path_buf) - 1] = '\0';
        } else {
            get_object_directory(path_buf, sizeof(path_buf));
        }

        // Convert Windows backslashes to forward slashes
        for (int i = 0; path_buf[i] != '\0'; i++) {
            if (path_buf[i] == '\\') {
                path_buf[i] = '/';
            }
        }

        // Clean trailing slash if present
        size_t len = strlen(path_buf);
        if (len > 0 && path_buf[len - 1] == '/') {
            path_buf[len - 1] = '\0';
        }

        x->save_path = gensym(path_buf);
        x->pending_save_path = NULL;

        // Ring buffer initialization
        x->ring_size = 44100 * RING_BUFFER_SIZE_SECONDS;
        x->ring_buffer = (double *)sysmem_newptrclear(x->ring_size * sizeof(double));
        x->ring_write_ptr = 0;
        x->ring_read_ptr = 0;
        critical_new(&x->ring_lock);

        x->status_qelem = qelem_new((t_object *)x, (method)video_status_qfn);
        x->q_status_msg[0] = '\0';
        x->q_status_arg[0] = '\0';

        object_post((t_object *)x, "video~: Initialized with save path: %s", x->save_path->s_name);
    }
    return x;
}

void video_free(t_video *x) {
    dsp_free((t_pxobject *)x);

    // Stop recording if active
    if (x->is_recording) {
        x->is_recording = 0;
    }

    // Join worker thread
    if (x->worker_thread) {
        unsigned int exit_code;
        systhread_join(x->worker_thread, &exit_code);
        x->worker_thread = NULL;
    }

    // Free buffers & structures
    if (x->ring_buffer) {
        sysmem_freeptr(x->ring_buffer);
        x->ring_buffer = NULL;
    }
    critical_free(x->ring_lock);

    if (x->status_qelem) {
        qelem_free(x->status_qelem);
        x->status_qelem = NULL;
    }
}

void video_assist(t_video *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "Inlet 1: Signal input & control messages (1 to start, 0 to stop, path [sym] to set save path)");
    } else {
        switch (a) {
            case 0: sprintf(s, "Outlet 1: Status Messages (started, stopped, error)"); break;
            case 1: sprintf(s, "Outlet 2: Diagnostic Logs"); break;
        }
    }
}

void video_int(t_video *x, long n) {
    if (n == 1) {
        if (x->is_recording || x->is_thread_active) {
            object_warn((t_object *)x, "video~: Already recording or processing a previous session. Ignore start command.");
            return;
        }

        // Apply pending save path if set
        if (x->pending_save_path) {
            x->save_path = x->pending_save_path;
            x->pending_save_path = NULL;
            object_post((t_object *)x, "video~: Output path updated to: %s", x->save_path->s_name);
        }

        // Generate timestamp
        time_t t = time(NULL);
        struct tm *tm_info = localtime(&t);
        char timestamp[64];
        strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm_info);

        // Prepare temporary & final file paths
        snprintf(x->temp_audio_path, sizeof(x->temp_audio_path), "%s/temp_audio_%s.wav", x->save_path->s_name, timestamp);
        snprintf(x->temp_video_path, sizeof(x->temp_video_path), "%s/temp_video_%s.mp4", x->save_path->s_name, timestamp);
        snprintf(x->final_video_path, sizeof(x->final_video_path), "%s/video_%s.mp4", x->save_path->s_name, timestamp);

        // Reset pointers
        critical_enter(x->ring_lock);
        x->ring_write_ptr = 0;
        x->ring_read_ptr = 0;
        x->total_samples_recorded = 0;
        critical_exit(x->ring_lock);

        // Create thread
        x->is_recording = 1;
        systhread_create((method)video_worker_thread, x, 0, 0, 0, &x->worker_thread);

    } else if (n == 0) {
        if (!x->is_recording) {
            object_warn((t_object *)x, "video~: Not currently recording.");
            return;
        }
        x->is_recording = 0;
    }
}

void video_float(t_video *x, double f) {
    video_int(x, (long)f);
}

void video_path(t_video *x, t_symbol *s) {
    if (s && strlen(s->s_name) > 0) {
        char path_buf[1024];
        strncpy(path_buf, s->s_name, sizeof(path_buf));
        path_buf[sizeof(path_buf) - 1] = '\0';

        // Convert Windows backslashes to forward slashes
        for (int i = 0; path_buf[i] != '\0'; i++) {
            if (path_buf[i] == '\\') {
                path_buf[i] = '/';
            }
        }

        // Clean trailing slash if present
        size_t len = strlen(path_buf);
        if (len > 0 && path_buf[len - 1] == '/') {
            path_buf[len - 1] = '\0';
        }

        x->pending_save_path = gensym(path_buf);
        object_post((t_object *)x, "video~: Pending save path set to: %s", x->pending_save_path->s_name);
    } else {
        object_error((t_object *)x, "video~: path message requires a symbol argument");
    }
}

void video_dsp64(t_video *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags) {
    x->sr = samplerate;
    dsp_add64(dsp64, (t_object *)x, (t_perfroutine64)video_perform64, 0, NULL);
}

void video_perform64(t_video *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam) {
    double *in = ins[0];

    if (x->is_recording) {
        critical_enter(x->ring_lock);
        for (int i = 0; i < sampleframes; i++) {
            long long write_idx = x->ring_write_ptr;
            x->ring_buffer[write_idx] = in[i];
            x->ring_write_ptr = (write_idx + 1) % x->ring_size;
        }
        critical_exit(x->ring_lock);
    }
}

void video_status_qfn(t_video *x) {
    // Deliver status messages on the main thread safely
    t_atom list_atoms[2];
    atom_setsym(&list_atoms[0], gensym(x->q_status_msg));

    if (strlen(x->q_status_arg) > 0) {
        atom_setsym(&list_atoms[1], gensym(x->q_status_arg));
        outlet_anything(x->status_outlet, gensym(x->q_status_msg), 1, &list_atoms[1]);
    } else {
        outlet_anything(x->status_outlet, gensym(x->q_status_msg), 0, NULL);
    }
}

void write_wav_header(FILE *f, int samplerate, long num_samples) {
    if (!f) return;
    fseek(f, 0, SEEK_SET);

    unsigned char header[44];
    long subchunk2_size = num_samples * 2; // 16-bit Mono PCM
    long chunk_size = 36 + subchunk2_size;
    long byte_rate = samplerate * 2;

    header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
    header[4] = chunk_size & 0xff;
    header[5] = (chunk_size >> 8) & 0xff;
    header[6] = (chunk_size >> 16) & 0xff;
    header[7] = (chunk_size >> 24) & 0xff;

    header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';

    header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
    header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0; // fmt chunk size
    header[20] = 1; header[21] = 0; // AudioFormat = 1 (PCM)
    header[22] = 1; header[23] = 0; // Mono (1 channel)

    header[24] = samplerate & 0xff;
    header[25] = (samplerate >> 8) & 0xff;
    header[26] = (samplerate >> 16) & 0xff;
    header[27] = (samplerate >> 24) & 0xff;

    header[28] = byte_rate & 0xff;
    header[29] = (byte_rate >> 8) & 0xff;
    header[30] = (byte_rate >> 16) & 0xff;
    header[31] = (byte_rate >> 24) & 0xff;

    header[32] = 2; header[33] = 0; // BlockAlign = 2 bytes
    header[34] = 16; header[35] = 0; // BitsPerSample = 16

    header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
    header[40] = subchunk2_size & 0xff;
    header[41] = (subchunk2_size >> 8) & 0xff;
    header[42] = (subchunk2_size >> 16) & 0xff;
    header[43] = (subchunk2_size >> 24) & 0xff;

    fwrite(header, 1, 44, f);
}

int start_ffmpeg_screen_recording(t_video *x, const char *temp_video_path) {
    HANDLE hChildStd_IN_Rd = NULL;
    HANDLE hChildStd_IN_Wr = NULL;
    SECURITY_ATTRIBUTES saAttr;

    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&hChildStd_IN_Rd, &hChildStd_IN_Wr, &saAttr, 0)) {
        return 0;
    }

    if (!SetHandleInformation(hChildStd_IN_Wr, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(hChildStd_IN_Rd);
        CloseHandle(hChildStd_IN_Wr);
        return 0;
    }

    PROCESS_INFORMATION piProcInfo;
    STARTUPINFO siStartInfo;
    BOOL bSuccess = FALSE;

    ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));
    ZeroMemory(&siStartInfo, sizeof(STARTUPINFO));
    siStartInfo.cb = sizeof(STARTUPINFO);
    siStartInfo.hStdInput = hChildStd_IN_Rd;

    // Set valid stdout and stderr handles to comply with Win32 standards when using STARTF_USESTDHANDLES
    siStartInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    if (siStartInfo.hStdOutput == NULL || siStartInfo.hStdOutput == INVALID_HANDLE_VALUE) {
        siStartInfo.hStdOutput = INVALID_HANDLE_VALUE;
    }
    siStartInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (siStartInfo.hStdError == NULL || siStartInfo.hStdError == INVALID_HANDLE_VALUE) {
        siStartInfo.hStdError = INVALID_HANDLE_VALUE;
    }

    siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

    char cmd[2048];
    // Record Windows screen with gdigrab
    snprintf(cmd, sizeof(cmd), "ffmpeg -y -f gdigrab -framerate 30 -i desktop -c:v libx264 -pix_fmt yuv420p \"%s\"", temp_video_path);

    bSuccess = CreateProcessA(
        NULL,
        cmd,
        NULL,
        NULL,
        TRUE,
        CREATE_NO_WINDOW,
        NULL,
        NULL,
        &siStartInfo,
        &piProcInfo
    );

    if (!bSuccess) {
        CloseHandle(hChildStd_IN_Rd);
        CloseHandle(hChildStd_IN_Wr);
        return 0;
    }

    CloseHandle(hChildStd_IN_Rd);

    x->hFfmpegProcess = piProcInfo.hProcess;
    x->hFfmpegStdinWrite = hChildStd_IN_Wr;
    CloseHandle(piProcInfo.hThread);

    return 1;
}

void stop_ffmpeg_screen_recording(t_video *x) {
    if (x->hFfmpegStdinWrite) {
        DWORD written = 0;
        // Signal ffmpeg to quit gracefully
        WriteFile(x->hFfmpegStdinWrite, "q\r\n", 3, &written, NULL);
        CloseHandle(x->hFfmpegStdinWrite);
        x->hFfmpegStdinWrite = NULL;
    }

    if (x->hFfmpegProcess) {
        DWORD waitResult = WaitForSingleObject(x->hFfmpegProcess, 8000);
        if (waitResult == WAIT_TIMEOUT) {
            TerminateProcess(x->hFfmpegProcess, 0);
        }
        CloseHandle(x->hFfmpegProcess);
        x->hFfmpegProcess = NULL;
    }
}

void *video_worker_thread(t_video *x) {
    x->is_thread_active = 1;

    // 1. Open audio WAV file
    x->wav_file = fopen(x->temp_audio_path, "wb");
    if (!x->wav_file) {
        x->is_recording = 0;
        strncpy(x->q_status_msg, "error", sizeof(x->q_status_msg));
        snprintf(x->q_status_arg, sizeof(x->q_status_arg), "Failed to open WAV file: %s", x->temp_audio_path);
        qelem_set(x->status_qelem);
        x->is_thread_active = 0;
        systhread_exit(0);
        return NULL;
    }

    // Write dummy WAV header to be overwritten later
    write_wav_header(x->wav_file, (int)x->sr, 0);

    // 2. Start ffmpeg screen recording
    if (!start_ffmpeg_screen_recording(x, x->temp_video_path)) {
        fclose(x->wav_file);
        x->wav_file = NULL;
        DeleteFileA(x->temp_audio_path);
        x->is_recording = 0;

        strncpy(x->q_status_msg, "error", sizeof(x->q_status_msg));
        strncpy(x->q_status_arg, "Failed to start ffmpeg screen recording. Ensure ffmpeg is in your PATH.", sizeof(x->q_status_arg));
        qelem_set(x->status_qelem);
        x->is_thread_active = 0;
        systhread_exit(0);
        return NULL;
    }

    strncpy(x->q_status_msg, "started", sizeof(x->q_status_msg));
    x->q_status_arg[0] = '\0';
    qelem_set(x->status_qelem);

    // 3. Audio draining loop
    while (x->is_recording) {
        systhread_sleep(10);

        long long write_ptr;
        long long read_ptr;

        critical_enter(x->ring_lock);
        write_ptr = x->ring_write_ptr;
        read_ptr = x->ring_read_ptr;
        critical_exit(x->ring_lock);

        long long available = 0;
        if (write_ptr >= read_ptr) {
            available = write_ptr - read_ptr;
        } else {
            available = (x->ring_size - read_ptr) + write_ptr;
        }

        if (available > 0) {
            short *local_buf = (short *)sysmem_newptr(available * sizeof(short));
            if (local_buf) {
                for (long long i = 0; i < available; i++) {
                    long long idx = (read_ptr + i) % x->ring_size;
                    double s = x->ring_buffer[idx];
                    if (s > 1.0) s = 1.0;
                    if (s < -1.0) s = -1.0;
                    local_buf[i] = (short)(s * 32767.0);
                }

                critical_enter(x->ring_lock);
                x->ring_read_ptr = (x->ring_read_ptr + available) % x->ring_size;
                x->total_samples_recorded += available;
                critical_exit(x->ring_lock);

                fwrite(local_buf, sizeof(short), available, x->wav_file);
                sysmem_freeptr(local_buf);
            }
        }
    }

    // 4. Drain any remaining audio samples
    long long final_write_ptr;
    long long final_read_ptr;

    critical_enter(x->ring_lock);
    final_write_ptr = x->ring_write_ptr;
    final_read_ptr = x->ring_read_ptr;
    critical_exit(x->ring_lock);

    long long final_available = 0;
    if (final_write_ptr >= final_read_ptr) {
        final_available = final_write_ptr - final_read_ptr;
    } else {
        final_available = (x->ring_size - final_read_ptr) + final_write_ptr;
    }

    if (final_available > 0) {
        short *local_buf = (short *)sysmem_newptr(final_available * sizeof(short));
        if (local_buf) {
            for (long long i = 0; i < final_available; i++) {
                long long idx = (final_read_ptr + i) % x->ring_size;
                double s = x->ring_buffer[idx];
                if (s > 1.0) s = 1.0;
                if (s < -1.0) s = -1.0;
                local_buf[i] = (short)(s * 32767.0);
            }

            critical_enter(x->ring_lock);
            x->ring_read_ptr = (x->ring_read_ptr + final_available) % x->ring_size;
            x->total_samples_recorded += final_available;
            critical_exit(x->ring_lock);

            fwrite(local_buf, sizeof(short), final_available, x->wav_file);
            sysmem_freeptr(local_buf);
        }
    }

    // Overwrite the WAV header with actual samples recorded
    write_wav_header(x->wav_file, (int)x->sr, (long)x->total_samples_recorded);
    fclose(x->wav_file);
    x->wav_file = NULL;

    // 5. Stop ffmpeg screen recording gracefully
    stop_ffmpeg_screen_recording(x);

    // 6. Audio-Video Muxing via ffmpeg
    STARTUPINFO siStartInfo;
    PROCESS_INFORMATION piProcInfo;
    ZeroMemory(&siStartInfo, sizeof(STARTUPINFO));
    ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));
    siStartInfo.cb = sizeof(STARTUPINFO);

    char mux_cmd[3072];
    // Copy video directly, encode audio to AAC, truncate to shortest stream
    snprintf(mux_cmd, sizeof(mux_cmd), "ffmpeg -y -i \"%s\" -i \"%s\" -c:v copy -c:a aac -shortest \"%s\"",
             x->temp_video_path, x->temp_audio_path, x->final_video_path);

    BOOL bMuxSuccess = CreateProcessA(
        NULL,
        mux_cmd,
        NULL,
        NULL,
        FALSE,
        CREATE_NO_WINDOW,
        NULL,
        NULL,
        &siStartInfo,
        &piProcInfo
    );

    int muxed = 0;
    if (bMuxSuccess) {
        // Wait up to 10 seconds for muxing to complete
        DWORD waitRes = WaitForSingleObject(piProcInfo.hProcess, 10000);
        if (waitRes == WAIT_OBJECT_0) {
            muxed = 1;
        } else {
            TerminateProcess(piProcInfo.hProcess, 0);
        }
        CloseHandle(piProcInfo.hProcess);
        CloseHandle(piProcInfo.hThread);
    }

    // 7. Cleanup temp files
    DeleteFileA(x->temp_audio_path);
    DeleteFileA(x->temp_video_path);

    if (muxed && GetFileAttributesA(x->final_video_path) != INVALID_FILE_ATTRIBUTES) {
        strncpy(x->q_status_msg, "stopped", sizeof(x->q_status_msg));
        strncpy(x->q_status_arg, x->final_video_path, sizeof(x->q_status_arg));
    } else {
        strncpy(x->q_status_msg, "error", sizeof(x->q_status_msg));
        strncpy(x->q_status_arg, "Failed to mux audio and video streams.", sizeof(x->q_status_arg));
    }

    qelem_set(x->status_qelem);
    x->is_thread_active = 0;
    systhread_exit(0);
    return NULL;
}
