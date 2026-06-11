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
#define DEFAULT_SUSTAIN_DURATION 10.0
#define RELEASE_EXTRA_TIME 2.0

typedef struct {
    char type[16];
    int note;
    int velocity;
    double time;
} MidiMessage;

typedef double* (*render_midi_ptr)(MidiMessage* midi_messages, int num_messages, double duration, int sample_rate, int* num_samples_out);

typedef struct {
    HINSTANCE hLib;
    render_midi_ptr render_midi;
    char name[256];
} t_sound_module;

typedef struct {
    double* buffer;
    int length;
    int pos;
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
void sounds_preset(t_sounds* x, long n);
void sounds_random(t_sounds* x);
void sounds_assist(t_sounds* x, void* b, long m, long a, char* s);

static t_class* sounds_class;

void ext_main(void* r) {
    t_class* c = class_new("sounds~", (method)sounds_new, (method)sounds_free, sizeof(t_sounds), 0L, A_GIMME, 0);

    class_addmethod(c, (method)sounds_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)sounds_list, "list", A_GIMME, 0);
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
        // Outlets created right-to-left
        outlet_new(x, "signal"); // Right (Index 1)
        outlet_new(x, "signal"); // Left (Index 0)

        critical_new(&x->lock);
        x->num_modules = 0;
        x->current_module = 0;
        x->sample_rate = 44100.0;

        for (int i = 0; i < MAX_VOICES; i++) {
            x->voices[i].active = 0;
            x->voices[i].buffer = NULL;
        }

        // Scan for modules
        char module_dir_abs[MAX_PATH_CHARS];
        char module_dir_rel[] = "modules";
        short pathid = 0;
        t_fourcc type = 0;

        // Find the object's path to locate modules/ relative to it
        char object_filename[MAX_FILENAME_CHARS];
        strncpy(object_filename, "sounds~.mxe64", MAX_FILENAME_CHARS);

        if (locatefile_extended(object_filename, &pathid, &type, NULL, 0) == 0) {
            char object_path[MAX_PATH_CHARS];
            path_toabsolutesystempath(pathid, object_filename, object_path);

            // Get directory of the object
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
                    render_midi_ptr ptr = (render_midi_ptr)GetProcAddress(h, "render_midi");
                    if (ptr) {
                        x->modules[x->num_modules].hLib = h;
                        x->modules[x->num_modules].render_midi = ptr;
                        strncpy(x->modules[x->num_modules].name, findData.cFileName, 256);
                        x->num_modules++;
                        object_post((t_object*)x, "Loaded module %d: %s", x->num_modules - 1, findData.cFileName);
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
        if (x->voices[i].buffer) {
            free(x->voices[i].buffer);
            x->voices[i].buffer = NULL;
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
    x->current_module = (int)n % x->num_modules;
    if (x->current_module < 0) x->current_module += x->num_modules;
    object_post((t_object*)x, "Switched to preset %d: %s", x->current_module, x->modules[x->current_module].name);
}

void sounds_random(t_sounds* x) {
    if (x->num_modules <= 1) return;
    int next = rand() % x->num_modules;
    while (next == x->current_module) next = rand() % x->num_modules;
    sounds_preset(x, next);
}

void sounds_list(t_sounds* x, t_symbol* s, short argc, t_atom* argv) {
    if (argc < 2) return;
    int note = atom_getlong(argv);
    int velocity = atom_getlong(argv + 1);

    if (x->num_modules == 0) return;
    render_midi_ptr render = x->modules[x->current_module].render_midi;

    if (velocity > 0) {
        // Note On
        MidiMessage msg;
        strcpy(msg.type, "note_on");
        msg.note = note;
        msg.velocity = velocity;
        msg.time = 0.0;

        int num_samples = 0;
        // Perform expensive rendering OUTSIDE of the lock
        double* rendered = render(&msg, 1, DEFAULT_SUSTAIN_DURATION, (int)x->sample_rate, &num_samples);

        if (rendered) {
            int voice_idx = -1;
            critical_enter(x->lock);
            // Find existing voice for this note to re-trigger or find free
            for (int i = 0; i < MAX_VOICES; i++) {
                if (x->voices[i].active && x->voices[i].note == note) {
                    voice_idx = i;
                    break;
                }
            }
            if (voice_idx == -1) {
                for (int i = 0; i < MAX_VOICES; i++) {
                    if (!x->voices[i].active) {
                        voice_idx = i;
                        break;
                    }
                }
            }

            if (voice_idx != -1) {
                if (x->voices[voice_idx].buffer) free(x->voices[voice_idx].buffer);
                x->voices[voice_idx].buffer = rendered;
                x->voices[voice_idx].length = num_samples;
                x->voices[voice_idx].pos = 0;
                x->voices[voice_idx].note = note;
                x->voices[voice_idx].velocity = velocity;
                x->voices[voice_idx].releasing = 0;
                x->voices[voice_idx].active = 1;
            } else {
                free(rendered);
            }
            critical_exit(x->lock);
        }
    } else {
        // Note Off
        int voice_to_render = -1;
        int original_velocity = 0;
        int current_pos = 0;

        critical_enter(x->lock);
        for (int i = 0; i < MAX_VOICES; i++) {
            if (x->voices[i].active && x->voices[i].note == note && !x->voices[i].releasing) {
                voice_to_render = i;
                original_velocity = x->voices[i].velocity;
                current_pos = x->voices[i].pos;
                break;
            }
        }
        critical_exit(x->lock);

        if (voice_to_render != -1) {
            double off_time = (double)current_pos / x->sample_rate;
            MidiMessage msgs[2];
            strcpy(msgs[0].type, "note_on");
            msgs[0].note = note;
            msgs[0].velocity = original_velocity;
            msgs[0].time = 0.0;

            strcpy(msgs[1].type, "note_off");
            msgs[1].note = note;
            msgs[1].velocity = 0;
            msgs[1].time = off_time;

            int num_samples = 0;
            // Perform expensive rendering OUTSIDE of the lock
            double* rendered = render(msgs, 2, off_time + RELEASE_EXTRA_TIME, (int)x->sample_rate, &num_samples);

            if (rendered) {
                critical_enter(x->lock);
                // Check if the voice is still the same note and not already re-triggered
                if (x->voices[voice_to_render].active && x->voices[voice_to_render].note == note && !x->voices[voice_to_render].releasing) {
                    double* old_buffer = x->voices[voice_to_render].buffer;
                    x->voices[voice_to_render].buffer = rendered;
                    x->voices[voice_to_render].length = num_samples;
                    x->voices[voice_to_render].releasing = 1;
                    if (old_buffer) free(old_buffer);
                } else {
                    free(rendered);
                }
                critical_exit(x->lock);
            }
        }
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
        for (int v = 0; v < MAX_VOICES; v++) {
            if (x->voices[v].active) {
                for (int i = 0; i < sampleframes; i++) {
                    if (x->voices[v].pos < x->voices[v].length) {
                        double val = x->voices[v].buffer[x->voices[v].pos];
                        outL[i] += val;
                        outR[i] += val;
                        x->voices[v].pos++;
                    } else {
                        x->voices[v].active = 0;
                        break;
                    }
                }
            }
        }
        critical_exit(x->lock);
    }
}

void sounds_assist(t_sounds* x, void* b, long m, long a, char* s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "MIDI (list), messages");
    } else {
        if (a == 0) sprintf(s, "(signal) Left Output");
        else sprintf(s, "(signal) Right Output");
    }
}
