#include "ext.h"
#include "ext_obex.h"
#include "ext_critical.h"
#include "ext_path.h"
#include "z_dsp.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_VOICES 32
#define MAX_MODULES 64

typedef struct {
    char type[16];
    int note;
    int velocity;
    double time;
} MidiMessage;

typedef void* (*create_voice_ptr)(int note, int velocity, int sample_rate);
typedef void (*note_off_voice_ptr)(void* voice_ptr);
typedef int (*process_voice_ptr)(void* voice_ptr, double* buffer, int num_samples);
typedef void (*free_voice_ptr)(void* voice_ptr);
typedef double* (*render_midi_ptr)(MidiMessage* midi_messages, int num_messages, double duration, int sample_rate, int* num_samples_out);

typedef struct {
    HINSTANCE hLib;
    create_voice_ptr create_voice;
    note_off_voice_ptr note_off_voice;
    process_voice_ptr process_voice;
    free_voice_ptr free_voice;
    render_midi_ptr render_midi;
    char name[256];
} t_sound_module;

typedef struct {
    void* voice_instance;
    create_voice_ptr create_voice;
    note_off_voice_ptr note_off_voice;
    process_voice_ptr process_voice;
    free_voice_ptr free_voice;
    int note;
    int velocity;
    int active;
    int releasing;
} t_voice;

typedef struct _sounds {
    t_pxobject obj;
    t_sound_module modules[MAX_MODULES];
    int num_modules;
    int current_module;
    t_voice voices[MAX_VOICES];
    t_critical lock;
    double sample_rate;
} t_sounds;

void* sounds_new(t_symbol* s, long argc, t_atom* argv);
void sounds_free(t_sounds* x);
void sounds_dsp64(t_sounds* x, t_object* dsp64, short* count, double samplerate, long maxvectorsize, long flags);
void sounds_perform64(t_sounds* x, t_object* dsp64, double** ins, long numins, double** outs, long numouts, long sampleframes, long flags, void* userparam);
void sounds_list(t_sounds* x, t_symbol* s, short argc, t_atom* argv);
void sounds_midievent(t_sounds* x, t_symbol* s, short argc, t_atom* argv);
void sounds_preset(t_sounds* x, long n);
void sounds_random(t_sounds* x);
void sounds_assist(t_sounds* x, void* b, long m, long a, char* s);

static t_class* sounds_class;

void ext_main(void* r) {
    t_class* c = class_new("sounds~", (method)sounds_new, (method)sounds_free, sizeof(t_sounds), 0L, A_GIMME, 0);

    class_addmethod(c, (method)sounds_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)sounds_list, "list", A_GIMME, 0);
    class_addmethod(c, (method)sounds_midievent, "midievent", A_GIMME, 0);
    class_addmethod(c, (method)sounds_preset, "preset", A_LONG, 0);
    class_addmethod(c, (method)sounds_random, "random", 0);
    class_addmethod(c, (method)sounds_assist, "assist", A_CANT, 0);

    class_dspinit(c);
    class_register(CLASS_BOX, c);
    sounds_class = c;

    common_symbols_init();
    srand((unsigned int)time(NULL));
}

void* sounds_new(t_symbol* s, long argc, t_atom* argv) {
    t_sounds* x = (t_sounds*)object_alloc(sounds_class);

    if (x) {
        dsp_setup((t_pxobject*)x, 1);
        outlet_new(x, "signal"); // Right
        outlet_new(x, "signal"); // Left

        critical_new(&x->lock);
        x->num_modules = 0;
        x->current_module = 0;
        x->sample_rate = 44100.0;

        for (int i = 0; i < MAX_VOICES; i++) {
            x->voices[i].active = 0;
            x->voices[i].voice_instance = NULL;
        }

        char module_dir_abs[MAX_PATH_CHARS];
        char module_dir_rel[] = "modules";
        short pathid = 0;
        t_fourcc type = 0;

        char object_filename[MAX_FILENAME_CHARS];
        strncpy(object_filename, "sounds~.mxe64", MAX_FILENAME_CHARS);

        if (locatefile_extended(object_filename, &pathid, &type, NULL, 0) == 0) {
            char object_path[MAX_PATH_CHARS];
            path_toabsolutesystempath(pathid, object_filename, object_path);

            char* last_slash = strrchr(object_path, '\\');
            if (!last_slash) last_slash = strrchr(object_path, '/');
            if (last_slash) {
                *last_slash = '\0';
                snprintf(module_dir_abs, MAX_PATH_CHARS, "%s\\modules", object_path);
            } else {
                strncpy(module_dir_abs, module_dir_rel, MAX_PATH_CHARS);
            }
        } else {
            strncpy(module_dir_abs, module_dir_rel, MAX_PATH_CHARS);
        }

        char search_path[MAX_PATH_CHARS];
        snprintf(search_path, MAX_PATH_CHARS, "%s\\*.dll", module_dir_abs);

        WIN32_FIND_DATA findData;
        HANDLE hFind = FindFirstFile(search_path, &findData);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (x->num_modules >= MAX_MODULES) break;

                char dllPath[MAX_PATH_CHARS];
                snprintf(dllPath, MAX_PATH_CHARS, "%s\\%s", module_dir_abs, findData.cFileName);

                HINSTANCE h = LoadLibrary(dllPath);
                if (h) {
                    create_voice_ptr create_fn = (create_voice_ptr)GetProcAddress(h, "create_voice");
                    note_off_voice_ptr off_fn = (note_off_voice_ptr)GetProcAddress(h, "note_off_voice");
                    process_voice_ptr proc_fn = (process_voice_ptr)GetProcAddress(h, "process_voice");
                    free_voice_ptr free_fn = (free_voice_ptr)GetProcAddress(h, "free_voice");
                    render_midi_ptr render_fn = (render_midi_ptr)GetProcAddress(h, "render_midi");

                    if (create_fn && off_fn && proc_fn && free_fn) {
                        x->modules[x->num_modules].hLib = h;
                        x->modules[x->num_modules].create_voice = create_fn;
                        x->modules[x->num_modules].note_off_voice = off_fn;
                        x->modules[x->num_modules].process_voice = proc_fn;
                        x->modules[x->num_modules].free_voice = free_fn;
                        x->modules[x->num_modules].render_midi = render_fn;
                        strncpy(x->modules[x->num_modules].name, findData.cFileName, 256);
                        x->num_modules++;
                        object_post((t_object*)x, "Loaded module %d: %s", x->num_modules, findData.cFileName);
                    } else {
                        FreeLibrary(h);
                    }
                }
            } while (FindNextFile(hFind, &findData));
            FindClose(hFind);
        }

        if (x->num_modules == 0) {
            object_error((t_object*)x, "No sound modules found in %s directory!", module_dir_abs);
        }
    }
    return x;
}

void sounds_free(t_sounds* x) {
    dsp_free((t_pxobject*)x);
    critical_enter(x->lock);
    for (int i = 0; i < MAX_VOICES; i++) {
        if (x->voices[i].active && x->voices[i].voice_instance && x->voices[i].free_voice) {
            x->voices[i].free_voice(x->voices[i].voice_instance);
            x->voices[i].voice_instance = NULL;
            x->voices[i].active = 0;
        }
    }
    for (int i = 0; i < x->num_modules; i++) {
        FreeLibrary(x->modules[i].hLib);
    }
    critical_exit(x->lock);
    critical_free(x->lock);
}

void sounds_preset(t_sounds* x, long n) {
    if (x->num_modules == 0) return;
    long zero_based = (n - 1) % x->num_modules;
    if (zero_based < 0) zero_based += x->num_modules;
    x->current_module = (int)zero_based;
    object_post((t_object*)x, "Switched to preset %d: %s", x->current_module + 1, x->modules[x->current_module].name);
}

void sounds_random(t_sounds* x) {
    if (x->num_modules <= 1) return;
    int next = rand() % x->num_modules;
    while (next == x->current_module) next = rand() % x->num_modules;
    sounds_preset(x, next + 1);
}

void sounds_midievent(t_sounds* x, t_symbol* s, short argc, t_atom* argv) {
    if (argc < 1) return;

    long status = atom_getlong(&argv[0]);
    long cmd = status & 0xF0;

    switch (cmd) {
        case 0x80: { // Note Off
            if (argc >= 2) {
                long note = atom_getlong(&argv[1]);
                t_atom list_argv[2];
                atom_setlong(&list_argv[0], note);
                atom_setlong(&list_argv[1], 0);
                sounds_list(x, NULL, 2, list_argv);
            }
            break;
        }
        case 0x90: { // Note On
            if (argc >= 3) {
                long note = atom_getlong(&argv[1]);
                long vel = atom_getlong(&argv[2]);
                t_atom list_argv[2];
                atom_setlong(&list_argv[0], note);
                atom_setlong(&list_argv[1], vel);
                sounds_list(x, NULL, 2, list_argv);
            } else if (argc >= 2) {
                long note = atom_getlong(&argv[1]);
                t_atom list_argv[2];
                atom_setlong(&list_argv[0], note);
                atom_setlong(&list_argv[1], 64);
                sounds_list(x, NULL, 2, list_argv);
            }
            break;
        }
        case 0xC0: { // Program Change
            if (argc >= 2) {
                long pgm = atom_getlong(&argv[1]);
                sounds_preset(x, pgm + 1);
            }
            break;
        }
        default:
            break;
    }
}

void sounds_list(t_sounds* x, t_symbol* s, short argc, t_atom* argv) {
    if (argc < 2) return;
    int note = atom_getlong(argv);
    int velocity = atom_getlong(argv + 1);

    if (x->num_modules == 0) return;
    t_sound_module* mod = &x->modules[x->current_module];

    if (velocity > 0) {
        // Note On
        if (!mod->create_voice) return;
        void* new_inst = mod->create_voice(note, velocity, (int)x->sample_rate);
        if (!new_inst) return;

        critical_enter(x->lock);
        int voice_idx = -1;
        // Re-use voice playing same note
        for (int i = 0; i < MAX_VOICES; i++) {
            if (x->voices[i].active && x->voices[i].note == note) {
                voice_idx = i;
                break;
            }
        }
        // Find free voice
        if (voice_idx == -1) {
            for (int i = 0; i < MAX_VOICES; i++) {
                if (!x->voices[i].active) {
                    voice_idx = i;
                    break;
                }
            }
        }

        if (voice_idx != -1) {
            if (x->voices[voice_idx].active && x->voices[voice_idx].voice_instance && x->voices[voice_idx].free_voice) {
                x->voices[voice_idx].free_voice(x->voices[voice_idx].voice_instance);
            }
            x->voices[voice_idx].voice_instance = new_inst;
            x->voices[voice_idx].create_voice = mod->create_voice;
            x->voices[voice_idx].note_off_voice = mod->note_off_voice;
            x->voices[voice_idx].process_voice = mod->process_voice;
            x->voices[voice_idx].free_voice = mod->free_voice;
            x->voices[voice_idx].note = note;
            x->voices[voice_idx].velocity = velocity;
            x->voices[voice_idx].releasing = 0;
            x->voices[voice_idx].active = 1;
        } else {
            mod->free_voice(new_inst);
        }
        critical_exit(x->lock);
    } else {
        // Note Off
        critical_enter(x->lock);
        for (int i = 0; i < MAX_VOICES; i++) {
            if (x->voices[i].active && x->voices[i].note == note && !x->voices[i].releasing) {
                x->voices[i].releasing = 1;
                if (x->voices[i].note_off_voice && x->voices[i].voice_instance) {
                    x->voices[i].note_off_voice(x->voices[i].voice_instance);
                }
                break;
            }
        }
        critical_exit(x->lock);
    }
}

void sounds_dsp64(t_sounds* x, t_object* dsp64, short* count, double samplerate, long maxvectorsize, long flags) {
    x->sample_rate = samplerate;
    dsp_add64(dsp64, (t_object*)x, (t_perfroutine64)sounds_perform64, 0, NULL);
}

void sounds_perform64(t_sounds* x, t_object* dsp64, double** ins, long numins, double** outs, long numouts, long sampleframes, long flags, void* userparam) {
    double* outL = outs[0];
    double* outR = outs[1];

    for (int i = 0; i < sampleframes; i++) {
        outL[i] = 0.0;
        outR[i] = 0.0;
    }

    if (critical_tryenter(x->lock) == MAX_ERR_NONE) {
        double temp_buf[2048];
        int frames_to_process = (sampleframes > 2048) ? 2048 : (int)sampleframes;

        for (int v = 0; v < MAX_VOICES; v++) {
            if (x->voices[v].active && x->voices[v].voice_instance && x->voices[v].process_voice) {
                memset(temp_buf, 0, frames_to_process * sizeof(double));
                int still_active = x->voices[v].process_voice(x->voices[v].voice_instance, temp_buf, frames_to_process);

                for (int i = 0; i < frames_to_process; i++) {
                    outL[i] += temp_buf[i];
                    outR[i] += temp_buf[i];
                }

                if (!still_active) {
                    if (x->voices[v].free_voice) {
                        x->voices[v].free_voice(x->voices[v].voice_instance);
                    }
                    x->voices[v].voice_instance = NULL;
                    x->voices[v].active = 0;
                    x->voices[v].releasing = 0;
                }
            }
        }
        critical_exit(x->lock);
    }
}

void sounds_assist(t_sounds* x, void* b, long m, long a, char* s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "MIDI (list), midievent, messages");
    } else {
        if (a == 0) sprintf(s, "(signal) Left Output");
        else sprintf(s, "(signal) Right Output");
    }
}
