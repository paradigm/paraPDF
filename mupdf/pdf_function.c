#include "fitz.h"
#include "mupdf.h"

enum
{
	MAXN = FZ_MAXCOLORS,
	MAXM = FZ_MAXCOLORS,
};

typedef struct psobj_s psobj;

enum
{
	SAMPLE = 0,
	EXPONENTIAL = 2,
	STITCHING = 3,
	POSTSCRIPT = 4
};

struct pdf_function_s
{
	int refs;
	int type;				/* 0=sample 2=exponential 3=stitching 4=postscript */
	int m;					/* number of input values */
	int n;					/* number of output values */
	float domain[MAXM][2];	/* even index : min value, odd index : max value */
	float range[MAXN][2];	/* even index : min value, odd index : max value */
	int hasrange;

	union
	{
		struct {
			unsigned short bps;
			int size[MAXM];
			float encode[MAXM][2];
			float decode[MAXN][2];
			float *samples;
		} sa;

		struct {
			float n;
			float c0[MAXN];
			float c1[MAXN];
		} e;

		struct {
			int k;
			pdf_function **funcs; /* k */
			float *bounds; /* k - 1 */
			float *encode; /* k * 2 */
		} st;

		struct {
			psobj *code;
			int cap;
		} p;
	} u;
};

#define RADIAN 57.2957795

static inline float LERP(float x, float xmin, float xmax, float ymin, float ymax)
{
	if (xmin == xmax)
		return ymin;
	if (ymin == ymax)
		return ymin;
	return ymin + (x - xmin) * (ymax - ymin) / (xmax - xmin);
}

/*
 * PostScript calculator
 */

enum { PSBOOL, PSINT, PSREAL, PSOPERATOR, PSBLOCK };

enum
{
	PSOABS, PSOADD, PSOAND, PSOATAN, PSOBITSHIFT, PSOCEILING,
	PSOCOPY, PSOCOS, PSOCVI, PSOCVR, PSODIV, PSODUP, PSOEQ,
	PSOEXCH, PSOEXP, PSOFALSE, PSOFLOOR, PSOGE, PSOGT, PSOIDIV,
	PSOINDEX, PSOLE, PSOLN, PSOLOG, PSOLT, PSOMOD, PSOMUL,
	PSONE, PSONEG, PSONOT, PSOOR, PSOPOP, PSOROLL, PSOROUND,
	PSOSIN, PSOSQRT, PSOSUB, PSOTRUE, PSOTRUNCATE, PSOXOR,
	PSOIF, PSOIFELSE, PSORETURN
};

static char *psopnames[] =
{
	"abs", "add", "and", "atan", "bitshift", "ceiling", "copy",
	"cos", "cvi", "cvr", "div", "dup", "eq", "exch", "exp",
	"false", "floor", "ge", "gt", "idiv", "index", "le", "ln",
	"log", "lt", "mod", "mul", "ne", "neg", "not", "or", "pop",
	"roll", "round", "sin", "sqrt", "sub", "true", "truncate",
	"xor", "if", "ifelse", "return"
};

struct psobj_s
{
	int type;
	union
	{
		int b;				/* boolean (stack only) */
		int i;				/* integer (stack and code) */
		float f;			/* real (stack and code) */
		int op;				/* operator (code only) */
		int block;			/* if/ifelse block pointer (code only) */
	} u;
};

typedef struct psstack_s psstack;

struct psstack_s
{
	psobj stack[100];
	int sp;
};

void
pdf_debugpsstack(psstack *st)
{
	int i;

	printf("stack: ");

	for (i = 0; i < st->sp; i++)
	{
		switch (st->stack[i].type)
		{
		case PSBOOL:
			if (st->stack[i].u.b)
				printf("true ");
			else
				printf("false ");
			break;

		case PSINT:
			printf("%d ", st->stack[i].u.i);
			break;

		case PSREAL:
			printf("%g ", st->stack[i].u.f);
			break;
		}
	}
	printf("\n");

}

static void
psinitstack(psstack *st)
{
	memset(st->stack, 0, sizeof(st->stack));
	st->sp = 0;
}

static inline int
psoverflow(psstack *st, int n)
{
	return n < 0 || st->sp + n >= nelem(st->stack);
}

static inline int
psunderflow(psstack *st, int n)
{
	return n < 0 || st->sp - n < 0;
}

static inline int
psistype(psstack *st, int t)
{
	return !psunderflow(st, 1) && st->stack[st->sp - 1].type == t;
}

static inline int
psistype2(psstack *st, int t)
{
	return !psunderflow(st, 2) && st->stack[st->sp - 1].type == t && st->stack[st->sp - 2].type == t;
}

static void
pspushbool(psstack *st, int b)
{
	if (!psoverflow(st, 1))
	{
		st->stack[st->sp].type = PSBOOL;
		st->stack[st->sp].u.b = b;
		st->sp++;
	}
}

static void
pspushint(psstack *st, int n)
{
	if (!psoverflow(st, 1))
	{
		st->stack[st->sp].type = PSINT;
		st->stack[st->sp].u.i = n;
		st->sp++;
	}
}

static void
pspushreal(psstack *st, float n)
{
	if (!psoverflow(st, 1))
	{
		st->stack[st->sp].type = PSREAL;
		st->stack[st->sp].u.f = n;
		st->sp++;
	}
}

static int
pspopbool(psstack *st)
{
	if (!psunderflow(st, 1))
	{
		if (psistype(st, PSBOOL))
			return st->stack[--st->sp].u.b;
	}
	return 0;
}

static int
pspopint(psstack *st)
{
	if (!psunderflow(st, 1))
	{
		if (psistype(st, PSINT))
			return st->stack[--st->sp].u.i;
		if (psistype(st, PSREAL))
			return st->stack[--st->sp].u.f;
	}
	return 0;
}

static float
pspopreal(psstack *st)
{
	if (!psunderflow(st, 1))
	{
		if (psistype(st, PSINT))
			return st->stack[--st->sp].u.i;
		if (psistype(st, PSREAL))
			return st->stack[--st->sp].u.f;
	}
	return 0;
}

static void
pscopy(psstack *st, int n)
{
	if (!psunderflow(st, n) && !psoverflow(st, n))
	{
		memcpy(st->stack + st->sp, st->stack + st->sp - n, n * sizeof(psobj));
		st->sp += n;
	}
}

static void
psroll(psstack *st, int n, int j)
{
	psobj tmp;
	int i;

	if (psunderflow(st, n) || j == 0)
		return;

	if (j >= 0)
	{
		j %= n;
	}
	else
	{
		j = -j % n;
		if (j != 0)
			j = n - j;
	}

	for (i = 0; i < j; i++)
	{
		tmp = st->stack[st->sp - 1];
		memmove(st->stack + st->sp - n + 1, st->stack + st->sp - n, n * sizeof(psobj));
		st->stack[st->sp - n] = tmp;
	}
}

static void
psindex(psstack *st, int n)
{
	if (!psoverflow(st, 1) && !psunderflow(st, n))
	{
		st->stack[st->sp] = st->stack[st->sp - n - 1];
		st->sp++;
	}
}

static void
psrun(psobj *code, psstack *st, int pc)
{
	int i1, i2;
	float r1, r2;
	int b1, b2;

	while (1)
	{
		switch (code[pc].type)
		{
		case PSINT:
			pspushint(st, code[pc++].u.i);
			break;

		case PSREAL:
			pspushreal(st, code[pc++].u.f);
			break;

		case PSOPERATOR:
			switch (code[pc++].u.op)
			{
			case PSOABS:
				if (psistype(st, PSINT))
					pspushint(st, abs(pspopint(st)));
				else
					pspushreal(st, fabsf(pspopreal(st)));
				break;

			case PSOADD:
				if (psistype2(st, PSINT)) {
					i2 = pspopint(st);
					i1 = pspopint(st);
					pspushint(st, i1 + i2);
				}
				else {
					r2 = pspopreal(st);
					r1 = pspopreal(st);
					pspushreal(st, r1 + r2);
				}
				break;

			case PSOAND:
				if (psistype2(st, PSINT)) {
					i2 = pspopint(st);
					i1 = pspopint(st);
					pspushint(st, i1 & i2);
				}
				else {
					b2 = pspopbool(st);
					b1 = pspopbool(st);
					pspushbool(st, b1 && b2);
				}
				break;

			case PSOATAN:
				r2 = pspopreal(st);
				r1 = pspopreal(st);
				r1 = atan2f(r1, r2) * RADIAN;
				if (r1 < 0)
					r1 += 360;
				pspushreal(st, r1);
				break;

			case PSOBITSHIFT:
				i2 = pspopint(st);
				i1 = pspopint(st);
				if (i2 > 0)
					pspushint(st, i1 << i2);
				else if (i2 < 0)
					pspushint(st, (int)((unsigned int)i1 >> i2));
				else
					pspushint(st, i1);
				break;

			case PSOCEILING:
				r1 = pspopreal(st);
				pspushreal(st, ceilf(r1));
				break;

			case PSOCOPY:
				pscopy(st, pspopint(st));
				break;

			case PSOCOS:
				r1 = pspopreal(st);
				pspushreal(st, cosf(r1/RADIAN));
				break;

			case PSOCVI:
				pspushint(st, pspopint(st));
				break;

			case PSOCVR:
				pspushreal(st, pspopreal(st));
				break;

			case PSODIV:
				r2 = pspopreal(st);
				r1 = pspopreal(st);
				pspushreal(st, r1 / r2);
				break;

			case PSODUP:
				pscopy(st, 1);
				break;

			case PSOEQ:
				if (psistype2(st, PSBOOL)) {
					b2 = pspopbool(st);
					b1 = pspopbool(st);
					pspushbool(st, b1 == b2);
				}
				else if (psistype2(st, PSINT)) {
					i2 = pspopint(st);
					i1 = pspopint(st);
					pspushbool(st, i1 == i2);
				}
				else {
					r2 = pspopreal(st);
					r1 = pspopreal(st);
					pspushbool(st, r1 == r2);
				}
				break;

			case PSOEXCH:
				psroll(st, 2, 1);
				break;

			case PSOEXP:
				r2 = pspopreal(st);
				r1 = pspopreal(st);
				pspushreal(st, powf(r1, r2));
				break;

			case PSOFALSE:
				pspushbool(st, 0);
				break;

			case PSOFLOOR:
				r1 = pspopreal(st);
				pspushreal(st, floorf(r1));
				break;

			case PSOGE:
				if (psistype2(st, PSINT)) {
					i2 = pspopint(st);
					i1 = pspopint(st);
					pspushbool(st, i1 >= i2);
				}
				else {
					r2 = pspopreal(st);
					r1 = pspopreal(st);
					pspushbool(st, r1 >= r2);
				}
				break;

			case PSOGT:
				if (psistype2(st, PSINT)) {
					i2 = pspopint(st);
					i1 = pspopint(st);
					pspushbool(st, i1 > i2);
				}
				else {
					r2 = pspopreal(st);
					r1 = pspopreal(st);
					pspushbool(st, r1 > r2);
				}
				break;

			case PSOIDIV:
				i2 = pspopint(st);
				i1 = pspopint(st);
				pspushint(st, i1 / i2);
				break;

			case PSOINDEX:
				psindex(st, pspopint(st));
				break;

			case PSOLE:
				if (psistype2(st, PSINT)) {
					i2 = pspopint(st);
					i1 = pspopint(st);
					pspushbool(st, i1 <= i2);
				}
				else {
					r2 = pspopreal(st);
					r1 = pspopreal(st);
					pspushbool(st, r1 <= r2);
				}
				break;

			case PSOLN:
				r1 = pspopreal(st);
				pspushreal(st, logf(r1));
				break;

			case PSOLOG:
				r1 = pspopreal(st);
				pspushreal(st, log10f(r1));
				break;

			case PSOLT:
				if (psistype2(st, PSINT)) {
					i2 = pspopint(st);
					i1 = pspopint(st);
					pspushbool(st, i1 < i2);
				}
				else {
					r2 = pspopreal(st);
					r1 = pspopreal(st);
					pspushbool(st, r1 < r2);
				}
				break;

			case PSOMOD:
				i2 = pspopint(st);
				i1 = pspopint(st);
				pspushint(st, i1 % i2);
				break;

			case PSOMUL:
				if (psistype2(st, PSINT)) {
					i2 = pspopint(st);
					i1 = pspopint(st);
					pspushint(st, i1 * i2);
				}
				else {
					r2 = pspopreal(st);
					r1 = pspopreal(st);
					pspushreal(st, r1 * r2);
				}
				break;

			case PSONE:
				if (psistype2(st, PSBOOL)) {
					b2 = pspopbool(st);
					b1 = pspopbool(st);
					pspushbool(st, b1 != b2);
				}
				else if (psistype2(st, PSINT)) {
					i2 = pspopint(st);
					i1 = pspopint(st);
					pspushbool(st, i1 != i2);
				}
				else {
					r2 = pspopreal(st);
					r1 = pspopreal(st);
					pspushbool(st, r1 != r2);
				}
				break;

			case PSONEG:
				if (psistype(st, PSINT))
					pspushint(st, -pspopint(st));
				else
					pspushreal(st, -pspopreal(st));
				break;

			case PSONOT:
				if (psistype(st, PSBOOL))
					pspushbool(st, !pspopbool(st));
				else
					pspushint(st, ~pspopint(st));
				break;

			case PSOOR:
				if (psistype2(st, PSBOOL)) {
					b2 = pspopbool(st);
					b1 = pspopbool(st);
					pspushbool(st, b1 || b2);
				}
				else {
					i2 = pspopint(st);
					i1 = pspopint(st);
					pspushint(st, i1 | i2);
				}
				break;

			case PSOPOP:
				if (!psunderflow(st, 1))
					st->sp--;
				break;

			case PSOROLL:
				i2 = pspopint(st);
				i1 = pspopint(st);
				psroll(st, i1, i2);
				break;

			case PSOROUND:
				if (!psistype(st, PSINT)) {
					r1 = pspopreal(st);
					pspushreal(st, (r1 >= 0) ? floorf(r1 + 0.5f) : ceilf(r1 - 0.5f));
				}
				break;

			case PSOSIN:
				r1 = pspopreal(st);
				pspushreal(st, sinf(r1/RADIAN));
				break;

			case PSOSQRT:
				r1 = pspopreal(st);
				pspushreal(st, sqrtf(r1));
				break;

			case PSOSUB:
				if (psistype2(st, PSINT)) {
					i2 = pspopint(st);
					i1 = pspopint(st);
					pspushint(st, i1 - i2);
				}
				else {
					r2 = pspopreal(st);
					r1 = pspopreal(st);
					pspushreal(st, r1 - r2);
				}
				break;

			case PSOTRUE:
				pspushbool(st, 1);
				break;

			case PSOTRUNCATE:
				if (!psistype(st, PSINT)) {
					r1 = pspopreal(st);
					pspushreal(st, (r1 >= 0) ? floorf(r1) : ceilf(r1));
				}
				break;

			case PSOXOR:
				if (psistype2(st, PSBOOL)) {
					b2 = pspopbool(st);
					b1 = pspopbool(st);
					pspushbool(st, b1 ^ b2);
				}
				else {
					i2 = pspopint(st);
					i1 = pspopint(st);
					pspushint(st, i1 ^ i2);
				}
				break;

			case PSOIF:
				b1 = pspopbool(st);
				if (b1)
					psrun(code, st, code[pc + 1].u.block);
				pc = code[pc + 2].u.block;
				break;

			case PSOIFELSE:
				b1 = pspopbool(st);
				if (b1)
					psrun(code, st, code[pc + 1].u.block);
				else
					psrun(code, st, code[pc + 0].u.block);
				pc = code[pc + 2].u.block;
				break;

			case PSORETURN:
				return;

			default:
				fz_warn("foreign operator in calculator function");
				return;
			}
			break;

		default:
			fz_warn("foreign object in calculator function");
			return;
		}
	}
}

static void
resizecode(pdf_function *func, int newsize)
{
	if (newsize >= func->u.p.cap)
	{
		func->u.p.cap = func->u.p.cap + 64;
		func->u.p.code = fz_realloc(func->u.p.code, func->u.p.cap, sizeof(psobj));
	}
}

static fz_error
parsecode(pdf_function *func, fz_stream *stream, int *codeptr)
{
	fz_error error;
	char buf[64];
	int len;
	int tok;
	int opptr, elseptr, ifptr;
	int a, b, mid, cmp;

	memset(buf, 0, sizeof(buf));

	while (1)
	{
		error = pdf_lex(&tok, stream, buf, sizeof buf, &len);
		if (error)
			return fz_rethrow(error, "calculator function lexical error");

		switch(tok)
		{
		case PDF_TEOF:
			return fz_throw("truncated calculator function");

		case PDF_TINT:
			resizecode(func, *codeptr);
			func->u.p.code[*codeptr].type = PSINT;
			func->u.p.code[*codeptr].u.i = atoi(buf);
			++*codeptr;
			break;

		case PDF_TREAL:
			resizecode(func, *codeptr);
			func->u.p.code[*codeptr].type = PSREAL;
			func->u.p.code[*codeptr].u.f = atof(buf);
			++*codeptr;
			break;

		case PDF_TOBRACE:
			opptr = *codeptr;
			*codeptr += 4;

			resizecode(func, *codeptr);

			ifptr = *codeptr;
			error = parsecode(func, stream, codeptr);
			if (error)
				return fz_rethrow(error, "error in 'if' branch");

			error = pdf_lex(&tok, stream, buf, sizeof buf, &len);
			if (error)
				return fz_rethrow(error, "calculator function syntax error");

			if (tok == PDF_TOBRACE)
			{
				elseptr = *codeptr;
				error = parsecode(func, stream, codeptr);
				if (error)
					return fz_rethrow(error, "error in 'else' branch");

				error = pdf_lex(&tok, stream, buf, sizeof buf, &len);
				if (error)
					return fz_rethrow(error, "calculator function syntax error");
			}
			else
			{
				elseptr = -1;
			}

			if (tok == PDF_TKEYWORD)
			{
				if (!strcmp(buf, "if"))
				{
					if (elseptr >= 0)
						return fz_throw("too many branches for 'if'");
					func->u.p.code[opptr].type = PSOPERATOR;
					func->u.p.code[opptr].u.op = PSOIF;
					func->u.p.code[opptr+2].type = PSBLOCK;
					func->u.p.code[opptr+2].u.block = ifptr;
					func->u.p.code[opptr+3].type = PSBLOCK;
					func->u.p.code[opptr+3].u.block = *codeptr;
				}
				else if (!strcmp(buf, "ifelse"))
				{
					if (elseptr < 0)
						return fz_throw("not enough branches for 'ifelse'");
					func->u.p.code[opptr].type = PSOPERATOR;
					func->u.p.code[opptr].u.op = PSOIFELSE;
					func->u.p.code[opptr+1].type = PSBLOCK;
					func->u.p.code[opptr+1].u.block = elseptr;
					func->u.p.code[opptr+2].type = PSBLOCK;
					func->u.p.code[opptr+2].u.block = ifptr;
					func->u.p.code[opptr+3].type = PSBLOCK;
					func->u.p.code[opptr+3].u.block = *codeptr;
				}
				else
				{
					return fz_throw("unknown keyword in 'if-else' context: '%s'", buf);
				}
			}
			else
			{
				return fz_throw("missing keyword in 'if-else' context");
			}
			break;

		case PDF_TCBRACE:
			resizecode(func, *codeptr);
			func->u.p.code[*codeptr].type = PSOPERATOR;
			func->u.p.code[*codeptr].u.op = PSORETURN;
			++*codeptr;
			return fz_okay;

		case PDF_TKEYWORD:
			cmp = -1;
			a = -1;
			b = nelem(psopnames);
			while (b - a > 1)
			{
				mid = (a + b) / 2;
				cmp = strcmp(buf, psopnames[mid]);
				if (cmp > 0)
					a = mid;
				else if (cmp < 0)
					b = mid;
				else
					a = b = mid;
			}
			if (cmp != 0)
				return fz_throw("unknown operator: '%s'", buf);

			resizecode(func, *codeptr);
			func->u.p.code[*codeptr].type = PSOPERATOR;
			func->u.p.code[*codeptr].u.op = a;
			++*codeptr;
			break;

		default:
			return fz_throw("calculator function syntax error");
		}
	}
}

static fz_error
loadpostscriptfunc(pdf_function *func, pdf_xref *xref, fz_obj *dict, int num, int gen)
{
	fz_error error;
	fz_stream *stream;
	int codeptr;
	char buf[64];
	int tok;
	int len;

	pdf_logrsrc("load postscript function (%d %d R)\n", num, gen);

	error = pdf_openstream(&stream, xref, num, gen);
	if (error)
		return fz_rethrow(error, "cannot open calculator function stream");

	error = pdf_lex(&tok, stream, buf, sizeof buf, &len);
	if (error)
	{
		fz_close(stream);
		return fz_rethrow(error, "stream is not a calculator function");
	}

	if (tok != PDF_TOBRACE)
	{
		fz_close(stream);
		return fz_throw("stream is not a calculator function");
	}

	func->u.p.code = nil;
	func->u.p.cap = 0;

	codeptr = 0;
	error = parsecode(func, stream, &codeptr);
	if (error)
	{
		fz_close(stream);
		return fz_rethrow(error, "cannot parse calculator function (%d %d R)", num, gen);
	}

	fz_close(stream);
	return fz_okay;
}

static void
evalpostscriptfunc(pdf_function *func, float *in, float *out)
{
	psstack st;
	float x;
	int i;

	psinitstack(&st);

	for (i = 0; i < func->m; i++)
	{
		x = CLAMP(in[i], func->domain[i][0], func->domain[i][1]);
		pspushreal(&st, x);
	}

	psrun(func->u.p.code, &st, 0);

	for (i = func->n - 1; i >= 0; i--)
	{
		x = pspopreal(&st);
		out[i] = CLAMP(x, func->range[i][0], func->range[i][1]);
	}
}

/*
 * Sample function
 */

static fz_error
loadsamplefunc(pdf_function *func, pdf_xref *xref, fz_obj *dict, int num, int gen)
{
	fz_error error;
	fz_stream *stream;
	fz_obj *obj;
	int samplecount;
	int bps;
	int i;

	pdf_logrsrc("sampled function {\n");

	func->u.sa.samples = nil;

	obj = fz_dictgets(dict, "Size");
	if (!fz_isarray(obj) || fz_arraylen(obj) != func->m)
		return fz_throw("malformed /Size");
	for (i = 0; i < func->m; i++)
		func->u.sa.size[i] = fz_toint(fz_arrayget(obj, i));

	obj = fz_dictgets(dict, "BitsPerSample");
	if (!fz_isint(obj))
		return fz_throw("malformed /BitsPerSample");
	func->u.sa.bps = bps = fz_toint(obj);

	pdf_logrsrc("bps %d\n", bps);

	obj = fz_dictgets(dict, "Encode");
	if (fz_isarray(obj))
	{
		if (fz_arraylen(obj) != func->m * 2)
			return fz_throw("malformed /Encode");
		for (i = 0; i < func->m; i++)
		{
			func->u.sa.encode[i][0] = fz_toreal(fz_arrayget(obj, i*2+0));
			func->u.sa.encode[i][1] = fz_toreal(fz_arrayget(obj, i*2+1));
		}
	}
	else
	{
		for (i = 0; i < func->m; i++)
		{
			func->u.sa.encode[i][0] = 0;
			func->u.sa.encode[i][1] = func->u.sa.size[i] - 1;
		}
	}

	obj = fz_dictgets(dict, "Decode");
	if (fz_isarray(obj))
	{
		if (fz_arraylen(obj) != func->n * 2)
			return fz_throw("malformed /Decode");
		for (i = 0; i < func->n; i++)
		{
			func->u.sa.decode[i][0] = fz_toreal(fz_arrayget(obj, i*2+0));
			func->u.sa.decode[i][1] = fz_toreal(fz_arrayget(obj, i*2+1));
		}
	}
	else
	{
		for (i = 0; i < func->n; i++)
		{
			func->u.sa.decode[i][0] = func->range[i][0];
			func->u.sa.decode[i][1] = func->range[i][1];
		}
	}

	for (i = 0, samplecount = func->n; i < func->m; i++)
		samplecount *= func->u.sa.size[i];

	pdf_logrsrc("samplecount %d\n", samplecount);

	func->u.sa.samples = fz_calloc(samplecount, sizeof(float));

	error = pdf_openstream(&stream, xref, num, gen);
	if (error)
		return fz_rethrow(error, "cannot open samples stream (%d %d R)", num, gen);

	/* read samples */
	for (i = 0; i < samplecount; i++)
	{
		unsigned int x;
		float s;

		if (fz_iseofbits(stream))
		{
			fz_close(stream);
			return fz_throw("truncated sample stream");
		}

		switch (bps)
		{
		case 1: s = fz_readbits(stream, 1); break;
		case 2: s = fz_readbits(stream, 2) / 3.0f; break;
		case 4: s = fz_readbits(stream, 4) / 15.0f; break;
		case 8: s = fz_readbyte(stream) / 255.0f; break;
		case 12: s = fz_readbits(stream, 12) / 4095.0f; break;
		case 16:
			x = fz_readbyte(stream) << 8;
			x |= fz_readbyte(stream);
			s = x / 65535.0f;
			break;
		case 24:
			x = fz_readbyte(stream) << 16;
			x |= fz_readbyte(stream) << 8;
			x |= fz_readbyte(stream);
			s = x / 16777215.0f;
			break;
		case 32:
			x = fz_readbyte(stream) << 24;
			x |= fz_readbyte(stream) << 16;
			x |= fz_readbyte(stream) << 8;
			x |= fz_readbyte(stream);
			s = x / 4294967295.0f;
			break;
		default:
			fz_close(stream);
			return fz_throw("sample stream bit depth %d unsupported", bps);
		}

		func->u.sa.samples[i] = s;
	}

	fz_close(stream);

	pdf_logrsrc("}\n");

	return fz_okay;
}

static float
interpolatesample(pdf_function *func, int *scale, int *e0, int *e1, float *efrac, int dim, int idx)
{
	float a, b;
	int idx0, idx1;

	idx0 = e0[dim] * scale[dim] + idx;
	idx1 = e1[dim] * scale[dim] + idx;

	if (dim == 0)
	{
		a = func->u.sa.samples[idx0];
		b = func->u.sa.samples[idx1];
	}
	else
	{
		a = interpolatesample(func, scale, e0, e1, efrac, dim - 1, idx0);
		b = interpolatesample(func, scale, e0, e1, efrac, dim - 1, idx1);
	}

	return a + (b - a) * efrac[dim];
}

static void
evalsamplefunc(pdf_function *func, float *in, float *out)
{
	int e0[MAXM], e1[MAXM], scale[MAXM];
	float efrac[MAXM];
	float x;
	int i;

	/* encode input coordinates */
	for (i = 0; i < func->m; i++)
	{
		x = CLAMP(in[i], func->domain[i][0], func->domain[i][1]);
		x = LERP(x, func->domain[i][0], func->domain[i][1],
			func->u.sa.encode[i][0], func->u.sa.encode[i][1]);
		x = CLAMP(x, 0, func->u.sa.size[i] - 1);
		e0[i] = floorf(x);
		e1[i] = ceilf(x);
		efrac[i] = x - floorf(x);
	}

	scale[0] = func->n;
	for (i = 1; i < func->m; i++)
		scale[i] = scale[i - 1] * func->u.sa.size[i];

	for (i = 0; i < func->n; i++)
	{
		if (func->m == 1)
		{
			float a = func->u.sa.samples[e0[0] * func->n + i];
			float b = func->u.sa.samples[e1[0] * func->n + i];

			float ab = a + (b - a) * efrac[0];

			out[i] = LERP(ab, 0, 1, func->u.sa.decode[i][0], func->u.sa.decode[i][1]);
			out[i] = CLAMP(out[i], func->range[i][0], func->range[i][1]);
		}

		else if (func->m == 2)
		{
			int s0 = func->n;
			int s1 = s0 * func->u.sa.size[0];

			float a = func->u.sa.samples[e0[0] * s0 + e0[1] * s1 + i];
			float b = func->u.sa.samples[e1[0] * s0 + e0[1] * s1 + i];
			float c = func->u.sa.samples[e0[0] * s0 + e1[1] * s1 + i];
			float d = func->u.sa.samples[e1[0] * s0 + e1[1] * s1 + i];

			float ab = a + (b - a) * efrac[0];
			float cd = c + (d - c) * efrac[0];
			float abcd = ab + (cd - ab) * efrac[1];

			out[i] = LERP(abcd, 0, 1, func->u.sa.decode[i][0], func->u.sa.decode[i][1]);
			out[i] = CLAMP(out[i], func->range[i][0], func->range[i][1]);
		}

		else
		{
			float x = interpolatesample(func, scale, e0, e1, efrac, func->m - 1, i);
			out[i] = LERP(x, 0, 1, func->u.sa.decode[i][0], func->u.sa.decode[i][1]);
			out[i] = CLAMP(out[i], func->range[i][0], func->range[i][1]);
		}
	}
}

/*
 * Exponential function
 */

static fz_error
loadexponentialfunc(pdf_function *func, fz_obj *dict)
{
	fz_obj *obj;
	int i;

	pdf_logrsrc("exponential function {\n");

	if (func->m != 1)
		return fz_throw("/Domain must be one dimension (%d)", func->m);

	obj = fz_dictgets(dict, "N");
	if (!fz_isint(obj) && !fz_isreal(obj))
		return fz_throw("malformed /N");
	func->u.e.n = fz_toreal(obj);
	pdf_logrsrc("n %g\n", func->u.e.n);

	obj = fz_dictgets(dict, "C0");
	if (fz_isarray(obj))
	{
		func->n = fz_arraylen(obj);
		if (func->n >= MAXN)
			return fz_throw("exponential function result array out of range");
		for (i = 0; i < func->n; i++)
			func->u.e.c0[i] = fz_toreal(fz_arrayget(obj, i));
		pdf_logrsrc("c0 %d\n", func->n);
	}
	else
	{
		func->n = 1;
		func->u.e.c0[0] = 0;
	}

	obj = fz_dictgets(dict, "C1");
	if (fz_isarray(obj))
	{
		if (fz_arraylen(obj) != func->n)
			return fz_throw("/C1 must match /C0 length");
		for (i = 0; i < func->n; i++)
			func->u.e.c1[i] = fz_toreal(fz_arrayget(obj, i));
		pdf_logrsrc("c1 %d\n", func->n);
	}
	else
	{
		if (func->n != 1)
			return fz_throw("/C1 must match /C0 length");
		func->u.e.c1[0] = 1;
	}

	pdf_logrsrc("}\n");

	return fz_okay;
}

static void
evalexponentialfunc(pdf_function *func, float in, float *out)
{
	float x = in;
	float tmp;
	int i;

	x = CLAMP(x, func->domain[0][0], func->domain[0][1]);

	/* constraint */
	if ((func->u.e.n != (int)func->u.e.n && x < 0) || (func->u.e.n < 0 && x == 0))
	{
		fz_warn("constraint error");
		return;
	}

	tmp = powf(x, func->u.e.n);
	for (i = 0; i < func->n; i++)
	{
		out[i] = func->u.e.c0[i] + tmp * (func->u.e.c1[i] - func->u.e.c0[i]);
		if (func->hasrange)
			out[i] = CLAMP(out[i], func->range[i][0], func->range[i][1]);
	}
}

/*
 * Stitching function
 */

static fz_error
loadstitchingfunc(pdf_function *func, pdf_xref *xref, fz_obj *dict)
{
	pdf_function **funcs;
	fz_error error;
	fz_obj *obj;
	fz_obj *sub;
	fz_obj *num;
	int k;
	int i;

	pdf_logrsrc("stitching {\n");

	func->u.st.k = 0;

	if (func->m != 1)
		return fz_throw("/Domain must be one dimension (%d)", func->m);

	obj = fz_dictgets(dict, "Functions");
	if (!fz_isarray(obj))
		return fz_throw("stitching function has no input functions");
	{
		k = fz_arraylen(obj);

		pdf_logrsrc("k %d\n", k);

		func->u.st.funcs = fz_calloc(k, sizeof(pdf_function*));
		func->u.st.bounds = fz_calloc(k - 1, sizeof(float));
		func->u.st.encode = fz_calloc(k * 2, sizeof(float));
		funcs = func->u.st.funcs;

		for (i = 0; i < k; i++)
		{
			sub = fz_arrayget(obj, i);
			error = pdf_loadfunction(&funcs[i], xref, sub);
			if (error)
				return fz_rethrow(error, "cannot load sub function %d (%d %d R)", i, fz_tonum(sub), fz_togen(sub));
			if (funcs[i]->m != 1 || funcs[i]->n != funcs[0]->n)
				return fz_throw("sub function %d /Domain or /Range mismatch", i);
			func->u.st.k ++;
		}

		if (!func->n)
			func->n = funcs[0]->n;
		else if (func->n != funcs[0]->n)
			return fz_throw("sub function /Domain or /Range mismatch");
	}

	obj = fz_dictgets(dict, "Bounds");
	if (!fz_isarray(obj))
		return fz_throw("stitching function has no bounds");
	{
		if (!fz_isarray(obj) || fz_arraylen(obj) != k - 1)
			return fz_throw("malformed /Bounds (not array or wrong length)");

		for (i = 0; i < k-1; i++)
		{
			num = fz_arrayget(obj, i);
			if (!fz_isint(num) && !fz_isreal(num))
				return fz_throw("malformed /Bounds (item not real)");
			func->u.st.bounds[i] = fz_toreal(num);
			if (i && func->u.st.bounds[i-1] > func->u.st.bounds[i])
				return fz_throw("malformed /Bounds (item not monotonic)");
		}

		if (k != 1 && (func->domain[0][0] > func->u.st.bounds[0] ||
			func->domain[0][1] < func->u.st.bounds[k-2]))
			fz_warn("malformed shading function bounds (domain mismatch), proceeding anyway.");
	}

	obj = fz_dictgets(dict, "Encode");
	if (!fz_isarray(obj))
		return fz_throw("stitching function is missing encoding");
	{
		if (!fz_isarray(obj) || fz_arraylen(obj) != k * 2)
			return fz_throw("malformed /Encode");
		for (i = 0; i < k; i++)
		{
			func->u.st.encode[i*2+0] = fz_toreal(fz_arrayget(obj, i*2+0));
			func->u.st.encode[i*2+1] = fz_toreal(fz_arrayget(obj, i*2+1));
		}
	}

	pdf_logrsrc("}\n");

	return fz_okay;
}

static void
evalstitchingfunc(pdf_function *func, float in, float *out)
{
	float low, high;
	int k = func->u.st.k;
	float *bounds = func->u.st.bounds;
	int i;

	in = CLAMP(in, func->domain[0][0], func->domain[0][1]);

	for (i = 0; i < k - 1; i++)
	{
		if (in < bounds[i])
			break;
	}

	if (i == 0 && k == 1)
	{
		low = func->domain[0][0];
		high = func->domain[0][1];
	}
	else if (i == 0)
	{
		low = func->domain[0][0];
		high = bounds[0];
	}
	else if (i == k - 1)
	{
		low = bounds[k-2];
		high = func->domain[0][1];
	}
	else
	{
		low = bounds[i-1];
		high = bounds[i];
	}

	in = LERP(in, low, high, func->u.st.encode[i*2+0], func->u.st.encode[i*2+1]);

	pdf_evalfunction(func->u.st.funcs[i], &in, 1, out, func->n);
}

/*
 * Common
 */

pdf_function *
pdf_keepfunction(pdf_function *func)
{
	func->refs ++;
	return func;
}

void
pdf_dropfunction(pdf_function *func)
{
	int i;
	if (--func->refs == 0)
	{
		switch(func->type)
		{
		case SAMPLE:
			fz_free(func->u.sa.samples);
			break;
		case EXPONENTIAL:
			break;
		case STITCHING:
			for (i = 0; i < func->u.st.k; i++)
				pdf_dropfunction(func->u.st.funcs[i]);
			fz_free(func->u.st.funcs);
			fz_free(func->u.st.bounds);
			fz_free(func->u.st.encode);
			break;
		case POSTSCRIPT:
			fz_free(func->u.p.code);
			break;
		}
		fz_free(func);
	}
}

fz_error
pdf_loadfunction(pdf_function **funcp, pdf_xref *xref, fz_obj *dict)
{
	fz_error error;
	pdf_function *func;
	fz_obj *obj;
	int i;

	if ((*funcp = pdf_finditem(xref->store, pdf_dropfunction, dict)))
	{
		pdf_keepfunction(*funcp);
		return fz_okay;
	}

	pdf_logrsrc("load function (%d %d R) {\n", fz_tonum(dict), fz_togen(dict));

	func = fz_malloc(sizeof(pdf_function));
	memset(func, 0, sizeof(pdf_function));
	func->refs = 1;

	obj = fz_dictgets(dict, "FunctionType");
	func->type = fz_toint(obj);

	pdf_logrsrc("type %d\n", func->type);

	/* required for all */
	obj = fz_dictgets(dict, "Domain");
	func->m = fz_arraylen(obj) / 2;
	for (i = 0; i < func->m; i++)
	{
		func->domain[i][0] = fz_toreal(fz_arrayget(obj, i * 2 + 0));
		func->domain[i][1] = fz_toreal(fz_arrayget(obj, i * 2 + 1));
	}
	pdf_logrsrc("domain %d\n", func->m);

	/* required for type0 and type4, optional otherwise */
	obj = fz_dictgets(dict, "Range");
	if (fz_isarray(obj))
	{
		func->hasrange = 1;
		func->n = fz_arraylen(obj) / 2;
		for (i = 0; i < func->n; i++)
		{
			func->range[i][0] = fz_toreal(fz_arrayget(obj, i * 2 + 0));
			func->range[i][1] = fz_toreal(fz_arrayget(obj, i * 2 + 1));
		}
		pdf_logrsrc("range %d\n", func->n);
	}
	else
	{
		func->hasrange = 0;
		func->n = 0;
	}

	if (func->m >= MAXM || func->n >= MAXN)
	{
		fz_free(func);
		return fz_throw("assert: /Domain or /Range too big");
	}

	switch(func->type)
	{
	case SAMPLE:
		error = loadsamplefunc(func, xref, dict, fz_tonum(dict), fz_togen(dict));
		if (error)
		{
			pdf_dropfunction(func);
			return fz_rethrow(error, "cannot load sampled function (%d %d R)", fz_tonum(dict), fz_togen(dict));
		}
		break;

	case EXPONENTIAL:
		error = loadexponentialfunc(func, dict);
		if (error)
		{
			pdf_dropfunction(func);
			return fz_rethrow(error, "cannot load exponential function (%d %d R)", fz_tonum(dict), fz_togen(dict));
		}
		break;

	case STITCHING:
		error = loadstitchingfunc(func, xref, dict);
		if (error)
		{
			pdf_dropfunction(func);
			return fz_rethrow(error, "cannot load stitching function (%d %d R)", fz_tonum(dict), fz_togen(dict));
		}
		break;

	case POSTSCRIPT:
		error = loadpostscriptfunc(func, xref, dict, fz_tonum(dict), fz_togen(dict));
		if (error)
		{
			pdf_dropfunction(func);
			return fz_rethrow(error, "cannot load calculator function (%d %d R)", fz_tonum(dict), fz_togen(dict));
		}
		break;

	default:
		fz_free(func);
		return fz_throw("unknown function type (%d %d R)", fz_tonum(dict), fz_togen(dict));
	}

	pdf_logrsrc("}\n");

	pdf_storeitem(xref->store, pdf_keepfunction, pdf_dropfunction, dict, func);

	*funcp = func;
	return fz_okay;
}

void
pdf_evalfunction(pdf_function *func, float *in, int inlen, float *out, int outlen)
{
	memset(out, 0, sizeof(float) * outlen);

	if (inlen != func->m)
	{
		fz_warn("tried to evaluate function with wrong number of inputs");
		return;
	}
	if (func->n != outlen)
	{
		fz_warn("tried to evaluate function with wrong number of outputs");
		return;
	}

	switch(func->type)
	{
	case SAMPLE: evalsamplefunc(func, in, out); break;
	case EXPONENTIAL: evalexponentialfunc(func, *in, out); break;
	case STITCHING: evalstitchingfunc(func, *in, out); break;
	case POSTSCRIPT: evalpostscriptfunc(func, in, out); break;
	}
}

/*
 * Debugging prints
 */

static void
pdf_debugindent(char *prefix, int level, char *suffix)
{
	int i;

	printf("%s", prefix);

	for (i = 0; i < level; i++)
		printf("\t");

	printf("%s", suffix);
}

static void
pdf_debugpsfunccode(psobj *funccode, psobj *code, int level)
{
	int eof, wasop;

	pdf_debugindent("", level, "{");

	/* Print empty blocks as { }, instead of separating braces on different lines. */
	if (code->type == PSOPERATOR && code->u.op == PSORETURN)
	{
		printf(" } ");
		return;
	}

	pdf_debugindent("\n", ++level, "");

	eof = 0;
	wasop = 0;
	while (!eof)
	{
		switch (code->type)
		{
		case PSINT:
			if (wasop)
				pdf_debugindent("\n", level, "");

			printf("%d ", code->u.i);
			wasop = 0;
			code++;
			break;

		case PSREAL:
			if (wasop)
				pdf_debugindent("\n", level, "");

			printf("%g ", code->u.f);
			wasop = 0;
			code++;
			break;

		case PSOPERATOR:
			if (code->u.op == PSORETURN)
			{
				printf("\n");
				eof = 1;
			}
			else if (code->u.op == PSOIF)
			{
				printf("\n");
				pdf_debugpsfunccode(funccode, &funccode[(code + 2)->u.block], level);

				printf("%s", psopnames[code->u.op]);
				code = &funccode[(code + 3)->u.block];
				if (code->type != PSOPERATOR || code->u.op != PSORETURN)
					pdf_debugindent("\n", level, "");

				wasop = 0;
			}
			else if (code->u.op == PSOIFELSE)
			{
				printf("\n");
				pdf_debugpsfunccode(funccode, &funccode[(code + 2)->u.block], level);

				printf("\n");
				pdf_debugpsfunccode(funccode, &funccode[(code + 1)->u.block], level);

				printf("%s", psopnames[code->u.op]);
				code = &funccode[(code + 3)->u.block];
				if (code->type != PSOPERATOR || code->u.op != PSORETURN)
					pdf_debugindent("\n", level, "");

				wasop = 0;
			}
			else
			{
				printf("%s ", psopnames[code->u.op]);
				code++;
				wasop = 1;
			}
			break;
		}
	}

	pdf_debugindent("", --level, "} ");
}

static void
pdf_debugfunctionimp(pdf_function *func, int level)
{
	int i;

	pdf_debugindent("", level, "function {\n");

	pdf_debugindent("", ++level, "");
	switch (func->type)
	{
	case SAMPLE:
		printf("sampled");
		break;
	case EXPONENTIAL:
		printf("exponential");
		break;
	case STITCHING:
		printf("stitching");
		break;
	case POSTSCRIPT:
		printf("postscript");
		break;
	}

	pdf_debugindent("\n", level, "");
	printf("%d input -> %d output\n", func->m, func->n);

	pdf_debugindent("", level, "domain ");
	for (i = 0; i < func->m; i++)
		printf("%g %g ", func->domain[i][0], func->domain[i][1]);
	printf("\n");

	if (func->hasrange)
	{
		pdf_debugindent("", level, "range ");
		for (i = 0; i < func->n; i++)
			printf("%g %g ", func->range[i][0], func->range[i][1]);
		printf("\n");
	}

	switch (func->type)
	{
	case SAMPLE:
		pdf_debugindent("", level, "");
		printf("bps: %d\n", func->u.sa.bps);

		pdf_debugindent("", level, "");
		printf("size: [ ");
		for (i = 0; i < func->m; i++)
			printf("%d ", func->u.sa.size[i]);
		printf("]\n");

		pdf_debugindent("", level, "");
		printf("encode: [ ");
		for (i = 0; i < func->m; i++)
			printf("%g %g ", func->u.sa.encode[i][0], func->u.sa.encode[i][1]);
		printf("]\n");

		pdf_debugindent("", level, "");
		printf("decode: [ ");
		for (i = 0; i < func->m; i++)
			printf("%g %g ", func->u.sa.decode[i][0], func->u.sa.decode[i][1]);
		printf("]\n");
		break;

	case EXPONENTIAL:
		pdf_debugindent("", level, "");
		printf("n: %g\n", func->u.e.n);

		pdf_debugindent("", level, "");
		printf("c0: [ ");
		for (i = 0; i < func->n; i++)
			printf("%g ", func->u.e.c0[i]);
		printf("]\n");

		pdf_debugindent("", level, "");
		printf("c1: [ ");
		for (i = 0; i < func->n; i++)
			printf("%g ", func->u.e.c1[i]);
		printf("]\n");
		break;

	case STITCHING:
		pdf_debugindent("", level, "");
		printf("%d functions\n", func->u.st.k);

		pdf_debugindent("", level, "");
		printf("bounds: [ ");
		for (i = 0; i < func->u.st.k - 1; i++)
			printf("%g ", func->u.st.bounds[i]);
		printf("]\n");

		pdf_debugindent("", level, "");
		printf("encode: [ ");
		for (i = 0; i < func->u.st.k * 2; i++)
			printf("%g ", func->u.st.encode[i]);
		printf("]\n");

		for (i = 0; i < func->u.st.k; i++)
			pdf_debugfunctionimp(func->u.st.funcs[i], level);
		break;

	case POSTSCRIPT:
		pdf_debugpsfunccode(func->u.p.code, func->u.p.code, level);
		printf("\n");
		break;
	}

	pdf_debugindent("", --level, "}\n");
}

void
pdf_debugfunction(pdf_function *func)
{
	pdf_debugfunctionimp(func, 0);
}
