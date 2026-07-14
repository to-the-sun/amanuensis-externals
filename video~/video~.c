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

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#define RING_BUFFER_SIZE_SECONDS 10
#define CONTROL_PORT 9099
#define AUDIO_PORT 9100

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

    // Thread handle
    t_systhread worker_thread;

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

int connect_socket(const char *host, int port, SOCKET *sock_out);
void start_recorder_process(t_video *x);
int ensure_recorder_running(t_video *x);
int send_control_command(t_video *x, const char *cmd, char *response_out, int response_len);

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

int connect_socket(const char *host, int port, SOCKET *sock_out) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        return 0;
    }
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sock);
        return 0;
    }
    *sock_out = sock;
    return 1;
}

void start_recorder_process(t_video *x) {
    char obj_dir[1024];
    get_object_directory(obj_dir, sizeof(obj_dir));

    char cmd[2048];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    BOOL success = FALSE;

    // Try 1: "py" (Standard Windows Python Launcher)
    snprintf(cmd, sizeof(cmd), "py \"%s/recorder.py\"", obj_dir);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWMINIMIZED;
    ZeroMemory(&pi, sizeof(pi));
    success = CreateProcessA(
        NULL,
        cmd,
        NULL,
        NULL,
        FALSE,
        CREATE_NEW_CONSOLE,
        NULL,
        NULL,
        &si,
        &pi
    );

    // Try 2: "python"
    if (!success) {
        snprintf(cmd, sizeof(cmd), "python \"%s/recorder.py\"", obj_dir);
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags |= STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOWMINIMIZED;
        ZeroMemory(&pi, sizeof(pi));
        success = CreateProcessA(
            NULL,
            cmd,
            NULL,
            NULL,
            FALSE,
            CREATE_NEW_CONSOLE,
            NULL,
            NULL,
            &si,
            &pi
        );
    }

    // Try 3: "python3"
    if (!success) {
        snprintf(cmd, sizeof(cmd), "python3 \"%s/recorder.py\"", obj_dir);
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags |= STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOWMINIMIZED;
        ZeroMemory(&pi, sizeof(pi));
        success = CreateProcessA(
            NULL,
            cmd,
            NULL,
            NULL,
            FALSE,
            CREATE_NEW_CONSOLE,
            NULL,
            NULL,
            &si,
            &pi
        );
    }

    if (success) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        object_post((t_object *)x, "video~: Launched external recorder process.");
        // Sleep a short bit to allow the Python script to start listening on the sockets
        Sleep(1000);
    } else {
        object_error((t_object *)x, "video~: Failed to launch external recorder process (tried py, python, and python3).");
    }
}

int ensure_recorder_running(t_video *x) {
    SOCKET test_sock = INVALID_SOCKET;
    if (connect_socket("127.0.0.1", CONTROL_PORT, &test_sock)) {
        closesocket(test_sock);
        return 1; // Already running
    }

    start_recorder_process(x);

    // Try to connect again with retry loop
    for (int i = 0; i < 5; i++) {
        Sleep(500);
        if (connect_socket("127.0.0.1", CONTROL_PORT, &test_sock)) {
            closesocket(test_sock);
            return 1;
        }
    }
    return 0;
}

int send_control_command(t_video *x, const char *cmd, char *response_out, int response_len) {
    SOCKET sock = INVALID_SOCKET;
    if (!connect_socket("127.0.0.1", CONTROL_PORT, &sock)) {
        return 0;
    }

    int len = (int)strlen(cmd);
    if (send(sock, cmd, len, 0) == SOCKET_ERROR) {
        closesocket(sock);
        return 0;
    }

    if (response_out && response_len > 0) {
        int received = recv(sock, response_out, response_len - 1, 0);
        if (received > 0) {
            response_out[received] = '\0';
        } else {
            response_out[0] = '\0';
        }
    }

    closesocket(sock);
    return 1;
}

void *video_new(t_symbol *s, long argc, t_atom *argv) {
    t_video *x = (t_video *)object_alloc(video_class);

    if (x) {
        // Initialize Winsock
        WSADATA wsaData;
        int wsa_err = WSAStartup(MAKEWORD(2,2), &wsaData);
        if (wsa_err != 0) {
            object_error((t_object *)x, "video~: WSAStartup failed with error: %d", wsa_err);
        }

        dsp_setup((t_pxobject *)x, 1);

        // Outlets (Right-to-Left)
        x->log_outlet = outlet_new((t_object *)x, NULL);
        x->status_outlet = outlet_new((t_object *)x, NULL);

        x->sr = 44100.0;
        x->is_recording = 0;
        x->is_thread_active = 0;
        x->total_samples_recorded = 0;
        x->worker_thread = NULL;

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

    WSACleanup();
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

        // Try sending it to the recorder immediately if running
        if (ensure_recorder_running(x)) {
            char cmd[2048];
            char resp[1024];
            snprintf(cmd, sizeof(cmd), "PATH %s\n", x->pending_save_path->s_name);
            if (send_control_command(x, cmd, resp, sizeof(resp))) {
                object_post((t_object *)x, "video~: Recorder path set response: %s", resp);
            }
        }
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

void *video_worker_thread(t_video *x) {
    x->is_thread_active = 1;

    // 1. Ensure recorder is running
    if (!ensure_recorder_running(x)) {
        x->is_recording = 0;
        strncpy(x->q_status_msg, "error", sizeof(x->q_status_msg));
        strncpy(x->q_status_arg, "External recorder is not running and could not be started.", sizeof(x->q_status_arg));
        qelem_set(x->status_qelem);
        x->is_thread_active = 0;
        systhread_exit(0);
        return NULL;
    }

    // Send path immediately to be sure the current path is active
    char path_cmd[2048];
    char path_resp[1024];
    snprintf(path_cmd, sizeof(path_cmd), "PATH %s\n", x->save_path->s_name);
    send_control_command(x, path_cmd, path_resp, sizeof(path_resp));

    // 2. Connect control socket
    SOCKET ctrl_sock = INVALID_SOCKET;
    if (!connect_socket("127.0.0.1", CONTROL_PORT, &ctrl_sock)) {
        x->is_recording = 0;
        strncpy(x->q_status_msg, "error", sizeof(x->q_status_msg));
        strncpy(x->q_status_arg, "Failed to connect to recorder control port 9099.", sizeof(x->q_status_arg));
        qelem_set(x->status_qelem);
        x->is_thread_active = 0;
        systhread_exit(0);
        return NULL;
    }

    // 3. Send START command
    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm_info);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "START %s %d\n", timestamp, (int)x->sr);

    if (send(ctrl_sock, cmd, (int)strlen(cmd), 0) == SOCKET_ERROR) {
        closesocket(ctrl_sock);
        x->is_recording = 0;
        strncpy(x->q_status_msg, "error", sizeof(x->q_status_msg));
        strncpy(x->q_status_arg, "Failed to send START command to recorder.", sizeof(x->q_status_arg));
        qelem_set(x->status_qelem);
        x->is_thread_active = 0;
        systhread_exit(0);
        return NULL;
    }

    // Read response
    char response[1024];
    int recvd = recv(ctrl_sock, response, sizeof(response) - 1, 0);
    if (recvd <= 0) {
        closesocket(ctrl_sock);
        x->is_recording = 0;
        strncpy(x->q_status_msg, "error", sizeof(x->q_status_msg));
        strncpy(x->q_status_arg, "Did not receive response from recorder for START command.", sizeof(x->q_status_arg));
        qelem_set(x->status_qelem);
        x->is_thread_active = 0;
        systhread_exit(0);
        return NULL;
    }
    response[recvd] = '\0';

    if (strstr(response, "STARTED") == NULL) {
        closesocket(ctrl_sock);
        x->is_recording = 0;
        strncpy(x->q_status_msg, "error", sizeof(x->q_status_msg));
        snprintf(x->q_status_arg, sizeof(x->q_status_arg), "Recorder failed to start: %s", response);
        qelem_set(x->status_qelem);
        x->is_thread_active = 0;
        systhread_exit(0);
        return NULL;
    }

    // Sleep 100ms to allow recorder to bind and listen on audio port
    Sleep(100);

    // 4. Connect audio socket
    SOCKET audio_sock = INVALID_SOCKET;
    if (!connect_socket("127.0.0.1", AUDIO_PORT, &audio_sock)) {
        // Stop recorder
        send(ctrl_sock, "STOP\n", 5, 0);
        closesocket(ctrl_sock);
        x->is_recording = 0;
        strncpy(x->q_status_msg, "error", sizeof(x->q_status_msg));
        strncpy(x->q_status_arg, "Failed to connect to recorder audio port 9100.", sizeof(x->q_status_arg));
        qelem_set(x->status_qelem);
        x->is_thread_active = 0;
        systhread_exit(0);
        return NULL;
    }

    strncpy(x->q_status_msg, "started", sizeof(x->q_status_msg));
    x->q_status_arg[0] = '\0';
    qelem_set(x->status_qelem);

    // 5. Audio draining loop
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

                // Send to audio socket
                int bytes_to_send = (int)(available * sizeof(short));
                int sent = send(audio_sock, (char *)local_buf, bytes_to_send, 0);
                if (sent == SOCKET_ERROR) {
                    object_error((t_object *)x, "video~: Failed sending audio samples over TCP.");
                }
                sysmem_freeptr(local_buf);
            }
        }
    }

    // 6. Drain any remaining audio samples
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

            // Send remaining to audio socket
            int bytes_to_send = (int)(final_available * sizeof(short));
            send(audio_sock, (char *)local_buf, bytes_to_send, 0);
            sysmem_freeptr(local_buf);
        }
    }

    // Close audio socket to signal EOF to recorder's receiver
    closesocket(audio_sock);
    audio_sock = INVALID_SOCKET;

    // 7. Send STOP command over control socket and wait for final status & path
    if (send(ctrl_sock, "STOP\n", 5, 0) == SOCKET_ERROR) {
        closesocket(ctrl_sock);
        strncpy(x->q_status_msg, "error", sizeof(x->q_status_msg));
        strncpy(x->q_status_arg, "Failed to send STOP command to recorder.", sizeof(x->q_status_arg));
        qelem_set(x->status_qelem);
        x->is_thread_active = 0;
        systhread_exit(0);
        return NULL;
    }

    // Read response for STOP command
    recvd = recv(ctrl_sock, response, sizeof(response) - 1, 0);
    if (recvd <= 0) {
        closesocket(ctrl_sock);
        strncpy(x->q_status_msg, "error", sizeof(x->q_status_msg));
        strncpy(x->q_status_arg, "Did not receive STOP response from recorder.", sizeof(x->q_status_arg));
        qelem_set(x->status_qelem);
        x->is_thread_active = 0;
        systhread_exit(0);
        return NULL;
    }
    response[recvd] = '\0';
    closesocket(ctrl_sock);

    // Parse response, e.g., "STOPPED C:/path/to/final_video.mp4"
    if (strncmp(response, "STOPPED", 7) == 0) {
        char *final_path = response + 7;
        while (*final_path == ' ' || *final_path == '\t') {
            final_path++;
        }
        // Trim trailing newlines/spaces
        int len = (int)strlen(final_path);
        while (len > 0 && (final_path[len - 1] == '\r' || final_path[len - 1] == '\n' || final_path[len - 1] == ' ' || final_path[len - 1] == '\t')) {
            final_path[len - 1] = '\0';
            len--;
        }

        strncpy(x->q_status_msg, "stopped", sizeof(x->q_status_msg));
        strncpy(x->q_status_arg, final_path, sizeof(x->q_status_arg));
    } else {
        strncpy(x->q_status_msg, "error", sizeof(x->q_status_msg));
        strncpy(x->q_status_arg, response, sizeof(x->q_status_arg));
    }

    qelem_set(x->status_qelem);
    x->is_thread_active = 0;
    systhread_exit(0);
    return NULL;
}
