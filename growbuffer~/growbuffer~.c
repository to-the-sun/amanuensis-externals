#include "ext.h"
#include "ext_obex.h"
#include "ext_buffer.h"

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
void growbuffer_set(t_growbuffer *x, t_symbol *s);
void growbuffer_symbol(t_growbuffer *x, t_symbol *s);
void growbuffer_assist(t_growbuffer *x, void *b, long m, long a, char *s);

static t_class *growbuffer_class;

void ext_main(void *r) {
	t_class *c = class_new("growbuffer~", (method)growbuffer_new, (method)growbuffer_free, sizeof(t_growbuffer), 0L, A_GIMME, 0);

	class_addmethod(c, (method)growbuffer_bang, "bang", 0);
	class_addmethod(c, (method)growbuffer_set, "set", A_SYM, 0);
	class_addmethod(c, (method)growbuffer_symbol, "symbol", A_SYM, 0);
	class_addmethod(c, (method)growbuffer_assist, "assist", A_CANT, 0);

	class_register(CLASS_BOX, c);
	growbuffer_class = c;
	common_symbols_init();
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
	t_buffer_obj *b = buffer_ref_getobject(x->b_ref);
	if (b) {
		double frames = (double)buffer_getframecount(b);
		double sr = buffer_getsamplerate(b);
		double ms = (sr > 0) ? (frames * 1000.0 / sr) : 0;
		post("growbuffer~: buffer %s length is %f ms", x->b_name->s_name, ms);
	} else {
		post("growbuffer~: no buffer %s found", x->b_name->s_name);
	}
}

void growbuffer_assist(t_growbuffer *x, void *b, long m, long a, char *s) {
	if (m == ASSIST_INLET) {
		switch (a) {
			case 0:
				sprintf(s, "(bang) report length, (set) set buffer name");
				break;
			case 1:
				sprintf(s, "(symbol) set buffer name");
				break;
		}
	}
}
