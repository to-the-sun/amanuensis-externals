#include "ext.h"
#include "ext_obex.h"
#include "ext_buffer.h"
#include "ext_critical.h"
#include "ext_systhread.h"
#include <string.h>
#include <math.h>

typedef struct _growbuffer {
	t_object b_obj;
	t_symbol *b_name;
	t_buffer_ref *b_ref;
	void *b_proxy;
	long b_inletnum;
	void *b_outlet;
	long log;
	void *log_outlet;
} t_growbuffer;

void *growbuffer_new(t_symbol *s, long argc, t_atom *argv);
void growbuffer_free(t_growbuffer *x);
void growbuffer_bang(t_growbuffer *x);
void growbuffer_int(t_growbuffer *x, long n);
void growbuffer_float(t_growbuffer *x, double f);

void growbuffer_log(t_growbuffer *x, const char *fmt, ...);
void growbuffer_do_bang(t_growbuffer *x, t_buffer_obj *b, t_symbol *name);
void growbuffer_do_resize(t_growbuffer *x, t_buffer_obj *b, t_symbol *name, double ms);

// Helper function to send verbose log messages with prefix
void growbuffer_log(t_growbuffer *x, const char *fmt, ...) {
	if (x->log && x->log_outlet) {
		char buf[1024];
		char final_buf[1100];
		va_list args;
		va_start(args, fmt);
		vsnprintf(buf, 1024, fmt, args);
		va_end(args);
		snprintf(final_buf, 1100, "growbuffer~: %s", buf);
		outlet_anything(x->log_outlet, gensym(final_buf), 0, NULL);
	}
}
void growbuffer_execute(t_growbuffer *x, double ms, int is_resize);

void growbuffer_set(t_growbuffer *x, t_symbol *s);
void growbuffer_symbol(t_growbuffer *x, t_symbol *s);
void growbuffer_anything(t_growbuffer *x, t_symbol *s, long argc, t_atom *argv);
void growbuffer_assist(t_growbuffer *x, void *b, long m, long a, char *s);

static t_class *growbuffer_class;

void ext_main(void *r) {
	common_symbols_init();

	t_class *c = class_new("growbuffer~", (method)growbuffer_new, (method)growbuffer_free, sizeof(t_growbuffer), 0L, A_GIMME, 0);

	class_addmethod(c, (method)growbuffer_bang, "bang", 0);
	class_addmethod(c, (method)growbuffer_int, "int", A_LONG, 0);
	class_addmethod(c, (method)growbuffer_float, "float", A_FLOAT, 0);
	class_addmethod(c, (method)growbuffer_set, "set", A_SYM, 0);
	class_addmethod(c, (method)growbuffer_symbol, "symbol", A_SYM, 0);
	class_addmethod(c, (method)growbuffer_anything, "anything", A_GIMME, 0);
	class_addmethod(c, (method)growbuffer_assist, "assist", A_CANT, 0);

	CLASS_ATTR_LONG(c, "log", 0, t_growbuffer, log);
	CLASS_ATTR_STYLE_LABEL(c, "log", 0, "onoff", "Enable Logging");
	CLASS_ATTR_DEFAULT(c, "log", 0, "0");

	class_register(CLASS_BOX, c);
	growbuffer_class = c;
}

void *growbuffer_new(t_symbol *s, long argc, t_atom *argv) {
	t_growbuffer *x = (t_growbuffer *)object_alloc(growbuffer_class);

	if (x) {
		x->b_name = _sym_nothing;
		x->log = 0;
		x->log_outlet = NULL;

		if (argc > 0 && atom_gettype(argv) == A_SYM && atom_getsym(argv)->s_name[0] != '@') {
			x->b_name = atom_getsym(argv);
			argc--;
			argv++;
		}

		attr_args_process(x, argc, argv);

		// Outlets are created from right to left
		if (x->log) {
			x->log_outlet = outlet_new((t_object *)x, NULL);
		}
		x->b_outlet = outlet_new((t_object *)x, NULL);

		x->b_proxy = proxy_new(x, 1, &x->b_inletnum);
		x->b_ref = buffer_ref_new((t_object *)x, x->b_name);
	}
	return x;
}

void growbuffer_free(t_growbuffer *x) {
	object_free(x->b_ref);
	object_free(x->b_proxy);
}

void growbuffer_set(t_growbuffer *x, t_symbol *s) {
	x->b_name = s;
	buffer_ref_set(x->b_ref, x->b_name);

	t_atom av[2];
	atom_setsym(&av[0], gensym("set"));
	atom_setsym(&av[1], s);
	outlet_anything(x->b_outlet, gensym("buffer"), 2, av);
}

void growbuffer_symbol(t_growbuffer *x, t_symbol *s) {
	if (proxy_getinlet((t_object *)x) == 1) {
		growbuffer_set(x, s);
	}
}

void growbuffer_bang(t_growbuffer *x) {
	if (proxy_getinlet((t_object *)x) == 0) {
		growbuffer_execute(x, 0, 0);
	}
}

void growbuffer_int(t_growbuffer *x, long n) {
	if (proxy_getinlet((t_object *)x) == 0) {
		growbuffer_execute(x, (double)n, 1);
	}
}

void growbuffer_float(t_growbuffer *x, double f) {
	if (proxy_getinlet((t_object *)x) == 0) {
		growbuffer_execute(x, f, 1);
	}
}

void growbuffer_anything(t_growbuffer *x, t_symbol *s, long argc, t_atom *argv) {
	if (proxy_getinlet((t_object *)x) == 1) {
		growbuffer_set(x, s);
	} else {
		t_atom av[3];
		atom_setsym(&av[0], gensym("error"));
		atom_setsym(&av[1], s);
		atom_setsym(&av[2], gensym("message_not_understood"));
		outlet_anything(x->b_outlet, gensym("buffer"), 3, av);
	}
}

void growbuffer_do_bang(t_growbuffer *x, t_buffer_obj *b, t_symbol *name) {
	double frames = (double)buffer_getframecount(b);
	double sr = buffer_getsamplerate(b);
	double ms = (sr > 0) ? (frames * 1000.0 / sr) : 0;

	t_atom av[3];
	atom_setsym(&av[0], name);
	atom_setsym(&av[1], gensym("length"));
	atom_setfloat(&av[2], ms);
	outlet_anything(x->b_outlet, gensym("buffer"), 3, av);
}

void growbuffer_do_resize(t_growbuffer *x, t_buffer_obj *b, t_symbol *name, double ms) {
	double sr = buffer_getsamplerate(b);
	if (sr <= 0) {
		t_atom av[3];
		atom_setsym(&av[0], name);
		atom_setsym(&av[1], gensym("error"));
		atom_setsym(&av[2], gensym("invalid_sample_rate"));
		outlet_anything(x->b_outlet, gensym("buffer"), 3, av);
		return;
	}

	long new_frames = (long)ceil((ms * sr) / 1000.0);
	if (new_frames < 0) new_frames = 0;

	t_buffer_info info;
	buffer_getinfo(b, &info);
	long old_frames = info.b_frames;
	long chans = info.b_nchans;

	growbuffer_log(x, "RESIZE START: Buffer '%s', Current Frames: %lld, New Frames: %lld, Channels: %lld", name->s_name, (long long)old_frames, (long long)new_frames, (long long)chans);

	if (new_frames == old_frames) {
		growbuffer_log(x, "RESIZE SKIPPED: New size matches current size");
		return;
	}

	critical_enter(0);

	float *backup = NULL;
	long frames_to_copy = (old_frames < new_frames) ? old_frames : new_frames;
	int retries = 0;

	if (frames_to_copy > 0 && chans > 0) {
		backup = (float *)sysmem_newptr(frames_to_copy * chans * sizeof(float));
		if (backup) {
			float *samples = NULL;
			for (retries = 0; retries < 10; retries++) {
				samples = buffer_locksamples(b);
				if (samples) break;
				systhread_sleep(1);
			}

			if (samples) {
				memcpy(backup, samples, frames_to_copy * chans * sizeof(float));
				buffer_unlocksamples(b);
				growbuffer_log(x, "BACKUP SUCCESS: %lld frames backed up (Retries: %d)", (long long)frames_to_copy, retries);
			} else {
				sysmem_freeptr(backup);
				backup = NULL;
				growbuffer_log(x, "BACKUP FAILED: Could not lock samples for buffer %s after %d retries", name->s_name, retries);
			}
		} else {
			growbuffer_log(x, "BACKUP FAILED: Could not allocate memory for backup");
		}
	}

	growbuffer_log(x, "RESIZE CALL: Sending 'sizeinsamps' %lld to buffer %s", (long long)new_frames, name->s_name);
	buffer_edit_begin(b);
	t_atom av;
	atom_setlong(&av, new_frames);
	object_method_typed(b, gensym("sizeinsamps"), 1, &av, NULL);

	if (backup) {
		t_buffer_info new_info;
		buffer_getinfo(b, &new_info);
		float *samples = new_info.b_samples;
		if (samples) {
			if (new_info.b_nchans == chans) {
				memcpy(samples, backup, frames_to_copy * chans * sizeof(float));
				growbuffer_log(x, "RESTORE SUCCESS: %lld frames copied back to resized buffer %s", (long long)frames_to_copy, name->s_name);
			} else {
				growbuffer_log(x, "RESTORE SKIPPED: Resized buffer %s has %lld channels (expected %lld)", name->s_name, (long long)new_info.b_nchans, (long long)chans);
			}
		} else {
			growbuffer_log(x, "RESTORE FAILED: Resized buffer %s has NULL samples pointer", name->s_name);
		}
		sysmem_freeptr(backup);
	}
	buffer_edit_end(b, 1);

	critical_exit(0);

	buffer_setdirty(b);

	growbuffer_log(x, "RESIZE END: Buffer '%s' resize complete", name->s_name);

	t_atom av_out[4];
	atom_setsym(&av_out[0], name);
	atom_setsym(&av_out[1], gensym("resized"));
	atom_setfloat(&av_out[2], ms);
	atom_setlong(&av_out[3], new_frames);
	outlet_anything(x->b_outlet, gensym("buffer"), 4, av_out);
}

void growbuffer_execute(t_growbuffer *x, double ms, int is_resize) {
	t_buffer_obj *b = buffer_ref_getobject(x->b_ref);
	if (b) {
		if (is_resize) growbuffer_do_resize(x, b, x->b_name, ms);
		else growbuffer_do_bang(x, b, x->b_name);
	} else {
		// Try polybuffer logic
		char bufname[256];
		snprintf(bufname, 256, "%s.1", x->b_name->s_name);
		t_symbol *s_member = gensym(bufname);

		t_buffer_ref *temp_ref = buffer_ref_new((t_object *)x, s_member);
		t_buffer_obj *b_member = buffer_ref_getobject(temp_ref);

		if (b_member) {
			int i = 1;
			while (b_member) {
				if (is_resize) growbuffer_do_resize(x, b_member, s_member, ms);
				else growbuffer_do_bang(x, b_member, s_member);

				i++;
				snprintf(bufname, 256, "%s.%d", x->b_name->s_name, i);
				s_member = gensym(bufname);
				buffer_ref_set(temp_ref, s_member);
				b_member = buffer_ref_getobject(temp_ref);
			}
		} else {
			t_atom av[2];
			atom_setsym(&av[0], gensym("buffer"));
			atom_setsym(&av[1], gensym("found"));
			outlet_anything(x->b_outlet, gensym("no"), 2, av);
		}
		object_free(temp_ref);
	}
}

void growbuffer_assist(t_growbuffer *x, void *b, long m, long a, char *s) {
	if (m == ASSIST_INLET) {
		switch (a) {
			case 0:
				sprintf(s, "Inlet 1: (bang) report length, (number) resize, (set) set buffer name");
				break;
			case 1:
				sprintf(s, "Inlet 2: (symbol) set buffer name");
				break;
		}
	} else {
		if (x->log) {
			switch (a) {
				case 0:
					sprintf(s, "Outlet 1: Status and Error Messages");
					break;
				case 1:
					sprintf(s, "Outlet 2: Logging Outlet");
					break;
			}
		} else {
			switch (a) {
				case 0:
					sprintf(s, "Outlet 1: Status and Error Messages");
					break;
			}
		}
	}
}
