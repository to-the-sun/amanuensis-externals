#include "ext.h"
#include "ext_obex.h"
#include "ext_buffer.h"
#include <string.h>
#include <math.h>

typedef struct _growbuffer {
	t_object b_obj;
	t_symbol *b_name;
	t_buffer_ref *b_ref;
	void *b_proxy;
	long b_inletnum;
} t_growbuffer;

void *growbuffer_new(t_symbol *s, long argc, t_atom *argv);
void growbuffer_free(t_growbuffer *x);
void growbuffer_bang(t_growbuffer *x);
void growbuffer_int(t_growbuffer *x, long n);
void growbuffer_float(t_growbuffer *x, double f);

void growbuffer_do_bang(t_growbuffer *x, t_buffer_obj *b, t_symbol *name);
void growbuffer_do_resize(t_growbuffer *x, t_buffer_obj *b, t_symbol *name, double ms);
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

	class_register(CLASS_BOX, c);
	growbuffer_class = c;
}

void *growbuffer_new(t_symbol *s, long argc, t_atom *argv) {
	t_growbuffer *x = (t_growbuffer *)object_alloc(growbuffer_class);

	if (x) {
		x->b_name = _sym_nothing;
		if (argc > 0 && atom_gettype(argv) == A_SYM) {
			x->b_name = atom_getsym(argv);
		}

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
	post("growbuffer~: buffer set to %s", s->s_name);
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
		object_error((t_object *)x, "growbuffer~: %s: message not understood", s->s_name);
	}
}

void growbuffer_do_bang(t_growbuffer *x, t_buffer_obj *b, t_symbol *name) {
	double frames = (double)buffer_getframecount(b);
	double sr = buffer_getsamplerate(b);
	double ms = (sr > 0) ? (frames * 1000.0 / sr) : 0;
	post("growbuffer~: buffer %s length is %f ms", name->s_name, ms);
}

void growbuffer_do_resize(t_growbuffer *x, t_buffer_obj *b, t_symbol *name, double ms) {
	double sr = buffer_getsamplerate(b);
	if (sr <= 0) {
		object_error((t_object *)x, "growbuffer~: buffer %s has invalid sample rate", name->s_name);
		return;
	}

	long new_frames = (long)ceil((ms * sr) / 1000.0);
	if (new_frames < 0) new_frames = 0;

	t_buffer_info info;
	buffer_getinfo(b, &info);
	long old_frames = info.b_frames;
	long chans = info.b_nchans;

	if (new_frames == old_frames) return;

	float *backup = NULL;
	long frames_to_copy = (old_frames < new_frames) ? old_frames : new_frames;

	if (frames_to_copy > 0 && chans > 0) {
		backup = (float *)sysmem_newptr(frames_to_copy * chans * sizeof(float));
		if (backup) {
			float *samples = buffer_locksamples(b);
			if (samples) {
				memcpy(backup, samples, frames_to_copy * chans * sizeof(float));
				buffer_unlocksamples(b);
			} else {
				sysmem_freeptr(backup);
				backup = NULL;
			}
		}
	}

	buffer_edit_begin(b);
	t_atom av;
	atom_setlong(&av, new_frames);
	object_method_typed(b, gensym("sizeinsamps"), 1, &av, NULL);

	if (backup) {
		float *samples = buffer_locksamples(b);
		if (samples) {
			t_buffer_info new_info;
			buffer_getinfo(b, &new_info);
			if (new_info.b_nchans == chans) {
				memcpy(samples, backup, frames_to_copy * chans * sizeof(float));
			}
			buffer_unlocksamples(b);
		}
		sysmem_freeptr(backup);
	}
	buffer_edit_end(b, 1);

	buffer_setdirty(b);
	post("growbuffer~: resized %s to %f ms (%ld frames)", name->s_name, ms, new_frames);
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
			object_error((t_object *)x, "growbuffer~: no buffer %s found", x->b_name->s_name);
		}
		object_free(temp_ref);
	}
}

void growbuffer_assist(t_growbuffer *x, void *b, long m, long a, char *s) {
	if (m == ASSIST_INLET) {
		switch (a) {
			case 0:
				sprintf(s, "(bang) report length, (number) resize, (set) set buffer name");
				break;
			case 1:
				sprintf(s, "(symbol) set buffer name");
				break;
		}
	}
}
