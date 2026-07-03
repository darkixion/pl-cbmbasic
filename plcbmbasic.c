/*-------------------------------------------------------------------------
 *
 * plcbmbasic.c
 *      PL/CBM-BASIC: Commodore 64 BASIC V2 as a PostgreSQL procedural
 *      language.
 *
 * The interpreter (Michael Steil's cbmbasic, a static recompilation of
 * the original 6502 BASIC ROM into plain C, not an emulator,
 * https://github.com/mist64/cbmbasic) is compiled directly into this
 * shared library.  Each function call is an in-memory power cycle --
 * zero the 64KB RAM array, reset the CPU registers, point the keyboard
 * at an fmemopen()ed buffer containing the injected argument
 * assignments plus the function source, and re-enter the recompiled ROM
 * at BASIC's $E394 entry point.  Everything the program PRINTs becomes
 * the return value.  No fork, no exec, no temp files: ~15-20us per
 * call.
 *
 * NAMED PARAMETERS become named BASIC variables, with the type spelled
 * the way Commodore intended:
 *
 *     CREATE FUNCTION greet(who text, age smallint, score float8) ...
 *
 * gives the program WHO$ (string), AGE% (16-bit integer variable), and
 * SCORE (float).  Unnamed parameters fall back to positional letters
 * (argument 1 -> A/A$/A%, argument 2 -> B..., up to 26).
 *
 * BASIC V2 places real constraints on names, all enforced at CREATE
 * FUNCTION time by plcbmbasic_validator():
 *
 *   - Only the first TWO characters of a variable name are significant,
 *     so (username text, userid int) would silently collide as US$/US.
 *     Colliding significant prefixes are rejected.
 *   - The tokenizer crunches keywords ANYWHERE, including inside
 *     identifiers: TOTAL contains TO, BUDGET contains GET.  Names
 *     containing a keyword are rejected.
 *   - TI and ST are the system clock and I/O status variables; names
 *     shadowing them are rejected.
 *   - Names must be a letter followed by letters/digits (sorry, no
 *     underscores in 1982).
 *
 * Argument assignments are packed into BASIC lines 0-9 (the input
 * buffer holds only 88 characters per line); long text arguments are
 * built up across lines by concatenation.  User code starts at line 10.
 *
 * The STOP KERNAL routine (polled by BASIC before every statement) is
 * patched to CHECK_FOR_INTERRUPTS(), so runaway programs respond to
 * query cancellation and statement_timeout.
 *
 * THE DATABASE IS DEVICE 8.  On a Commodore 64 your data lives on the
 * disk drive, device 8, and you talk to it with OPEN/INPUT#/GET#/
 * PRINT#/CLOSE and the ST status variable.  Here the same protocol
 * reaches PostgreSQL through SPI:
 *
 *     10 OPEN 1,8,0,"SELECT NAME,SCORE FROM HISCORES ORDER BY 2 DESC"
 *     20 INPUT#1,N$,S
 *     30 PRINT N$;S
 *     40 IF ST=0 THEN 20
 *     50 CLOSE 1
 *
 * Each column value arrives as its own CR-terminated record (so INPUT#
 * never overruns BASIC's 88-byte input buffer); ST acquires the EOF
 * bit (64) on the last byte, exactly like a 1541.  Secondary address
 * 15 is the DOS command channel: PRINT#15 a statement of any length
 * (INSERT/UPDATE/DDL/whatever) and INPUT#15 reads back a DOS-style
 * "0,OK,<rows>,0" status line.  PRINT# to a data channel also works,
 * replacing that channel's result set.  The plumbing: OPEN on device 8
 * hands the KERNAL a fopencookie() stream backed by an SPI call, so
 * INPUT#/GET#/ST all work through the vendored KERNAL code unchanged.
 *
 * BASIC runtime errors are trapped through the ERROR vector at $0300
 * (the same plugin facility Simons' BASIC used): the handler receives
 * the real error number and line, rather than scraping the screen for
 * "?SYNTAX  ERROR" text.
 *
 * Still an UNTRUSTED language: LOAD/SAVE/OPEN-to-a-file touch the
 * server filesystem as the postgres OS user.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "access/htup_details.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "commands/event_trigger.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(plcbmbasic_call_handler);
PG_FUNCTION_INFO_V1(plcbmbasic_inline_handler);
PG_FUNCTION_INFO_V1(plcbmbasic_validator);

/* provided by the vendored cbmbasic sources */
extern int	cbm_main(int argc, char **argv);
extern void plc_runtime_reset(FILE *program);
extern void plc_cpu_reset(void);
extern void plc_sql_stream_invalidate(FILE *f);

/* the emulated machine's memory, for reading variables back out */
extern unsigned char RAM[65536];

/* CBM 40-bit float limits: 8-bit exponent, 32-bit mantissa */
#define PLC_CBM_FLOAT_MAX	1.70141183e38
/* BASIC zero-page pointers to the simple-variable table */
#define PLC_VARTAB	0x2D
#define PLC_ARYTAB	0x2F

/* exit codes carried through the longjmp */
#define PLC_JMP_DONE		1	/* program finished normally */
#define PLC_JMP_ABORTED		2	/* interpreter called exit(nonzero) */
#define PLC_JMP_WANTS_INPUT 3	/* program executed INPUT or GET */

/* the 88-byte BASIC line input buffer, minus room for "9 " and slack */
#define PLC_LINE_BUDGET		72
/* lines 0..9 are reserved for argument assignments */
#define PLC_MAX_ARG_LINES	10
/* BASIC V2 strings hold at most 255 characters */
#define PLC_MAX_STRING		255
/* longest accepted parameter name (only 2 chars are significant anyway) */
#define PLC_MAX_NAME		24
/* device-8 column values are clipped so a quoted field fits INPUT#'s
 * 88-byte input buffer with room to spare */
#define PLC_SQL_FIELD_MAX	78
/* highest line number BASIC V2 accepts */
#define PLC_MAX_LINENO		63999

static sigjmp_buf plc_escape;
static int	plc_exit_status = 0;
static bool plc_machine_on = false; /* inside cbm_main()? */
static StringInfoData plc_output;

/* error trapped through the BASIC ERROR vector; -1 = none */
static volatile int plc_basic_errcode = -1;
static volatile unsigned int plc_basic_errline = 0;

/* the ROM's error messages, indexed by error number */
static const char *const basic_error_names[] = {
	NULL,
	"TOO MANY FILES", "FILE OPEN", "FILE NOT OPEN", "FILE NOT FOUND",
	"DEVICE NOT PRESENT", "NOT INPUT FILE", "NOT OUTPUT FILE",
	"MISSING FILE NAME", "ILLEGAL DEVICE NUMBER", "NEXT WITHOUT FOR",
	"SYNTAX", "RETURN WITHOUT GOSUB", "OUT OF DATA", "ILLEGAL QUANTITY",
	"OVERFLOW", "OUT OF MEMORY", "UNDEF'D STATEMENT", "BAD SUBSCRIPT",
	"REDIM'D ARRAY", "DIVISION BY ZERO", "ILLEGAL DIRECT",
	"TYPE MISMATCH", "STRING TOO LONG", "FILE DATA",
	"FORMULA TOO COMPLEX", "CAN'T CONTINUE", "UNDEF'D FUNCTION",
	"VERIFY", "LOAD", "BREAK"
};

#define PLC_N_BASIC_ERRORS	30

/* one argument's BASIC identity */
typedef struct plc_var
{
	char		name[PLC_MAX_NAME + 1]; /* uppercase, no suffix */
	char		suffix;			/* '$', '%', or 0 for float */
	char		sigkey[4];		/* significant prefix + suffix */
} plc_var;

/*
 * BASIC V2 keywords as the cruncher spells them (alphabetic tokens only;
 * TAB(, SPC(, STR$ and friends can't appear inside a parameter name
 * because names can't contain '(' or '$').  A variable name containing
 * any of these as a substring is tokenized into garbage.
 */
static const char *const basic_keywords[] = {
	"ABS", "AND", "ASC", "ATN", "CLOSE", "CLR", "CMD", "CONT", "COS",
	"DATA", "DEF", "DIM", "END", "EXP", "FN", "FOR", "FRE", "GET", "GO",
	"IF", "INPUT", "INT", "LEN", "LET", "LIST", "LOAD", "LOG", "NEW",
	"NEXT", "NOT", "ON", "OPEN", "OR", "PEEK", "POKE", "POS", "PRINT",
	"READ", "REM", "RESTORE", "RETURN", "RND", "RUN", "SAVE", "SGN",
	"SIN", "SQR", "STEP", "STOP", "SYS", "TAN", "THEN", "TO", "USR",
	"VAL", "VERIFY", "WAIT",
	NULL
};

/*
 * I/O replacements.  The cbmbasic translation units are compiled with
 *   -Dputchar=plc_putchar -Dgetchar=plc_getchar
 *   -Dprintf=plc_printf   -Dexit=plc_exit
 * (plus -D__NO_INLINE__ so glibc's extern inlines don't reroute them
 * back to the real stdio), so every screen write, keyboard read,
 * diagnostic message, and process exit in the emulated machine arrives
 * here instead.
 */

int			plc_putchar(int c);
int			plc_getchar(void);
int			plc_printf(const char *fmt,...) pg_attribute_printf(1, 2);
pg_noreturn void plc_exit(int status);
void		plc_stop_hook(void);
void		plc_error_hook(unsigned char errcode, unsigned short line);
FILE	   *plc_sql_open(const unsigned char *name, int namelen, int secaddr);

int
plc_putchar(int c)
{
	if (plc_machine_on)
		appendStringInfoChar(&plc_output, (char) c);
	return c;
}

int
plc_getchar(void)
{
	siglongjmp(plc_escape, PLC_JMP_WANTS_INPUT);
	return -1;					/* not reached */
}

int
plc_printf(const char *fmt,...)
{
	char		buf[1024];
	va_list		args;
	int			n;

	va_start(args, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	if (plc_machine_on)
		appendStringInfoString(&plc_output, buf);
	return n;
}

void
plc_exit(int status)
{
	plc_exit_status = status;
	siglongjmp(plc_escape, status == 0 ? PLC_JMP_DONE : PLC_JMP_ABORTED);
	for (;;)
		;						/* not reached; satisfies noreturn */
}

void
plc_stop_hook(void)
{
	/* called by BASIC before every statement (STOP key scan) */
	CHECK_FOR_INTERRUPTS();
}

/*
 * Called through BASIC's ERROR vector at $0300 whenever the interpreter
 * raises an error.  Error numbers >= $80 are the ROM's "no message"
 * codes (READY/BREAK paths), not errors.  Only the first error is kept;
 * BASIC then prints its message and falls back to direct mode, which
 * ends the program normally.
 */
void
plc_error_hook(unsigned char errcode, unsigned short line)
{
	if (errcode < 0x80 && plc_basic_errcode < 0)
	{
		plc_basic_errcode = errcode;
		plc_basic_errline = line;
	}
}

/*
 * Uppercase ASCII in place.  BASIC V2 tokenizes only uppercase keywords;
 * on a real C64 the default character set is uppercase anyway, so string
 * literals are uppercased too.  It's 1982.  Deal with it.
 */
static void
basic_toupper(char *s)
{
	for (; *s; s++)
	{
		if (*s >= 'a' && *s <= 'z')
			*s -= 32;
	}
}

/*
 * Decide the BASIC variable suffix for a PostgreSQL type:
 * smallint -> '%' (an authentic 16-bit integer variable), other numerics
 * and booleans -> 0 (float variable), everything else -> '$' (string).
 */
static char
suffix_for_type(Oid argtype)
{
	char		typcategory;
	bool		typispreferred;

	if (argtype == INT2OID)
		return '%';
	if (argtype == BOOLOID)
		return 0;
	get_type_category_preferred(argtype, &typcategory, &typispreferred);
	if (typcategory == TYPCATEGORY_NUMERIC)
		return 0;
	return '$';
}

/*
 * Map one function argument to its BASIC variable, validating the name
 * against BASIC V2's rules.  argname may be NULL or "" for unnamed
 * parameters, which fall back to a positional letter.
 */
static void
map_argument(int argno, const char *argname, Oid argtype, plc_var *var)
{
	int			i;
	size_t		siglen;

	var->suffix = suffix_for_type(argtype);

	if (argname == NULL || argname[0] == '\0')
	{
		/* positional fallback: argument 1 -> A, 2 -> B, ... */
		if (argno >= 26)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
					 errmsg("PL/CBM-BASIC supports at most 26 unnamed arguments"),
					 errdetail("Positional variables are named A through Z."),
					 errhint("Name your parameters to escape the alphabet.")));
		var->name[0] = 'A' + argno;
		var->name[1] = '\0';
	}
	else
	{
		if (strlen(argname) > PLC_MAX_NAME)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_NAME),
					 errmsg("parameter name \"%s\" is too long for PL/CBM-BASIC",
							argname),
					 errdetail("Names may be at most %d characters "
							   "(and only the first 2 are significant anyway).",
							   PLC_MAX_NAME)));

		strcpy(var->name, argname);
		basic_toupper(var->name);

		if (!(var->name[0] >= 'A' && var->name[0] <= 'Z'))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_NAME),
					 errmsg("parameter name \"%s\" is not a valid BASIC variable name",
							argname),
					 errdetail("BASIC V2 variable names must start with a letter.")));

		for (i = 1; var->name[i]; i++)
		{
			char		c = var->name[i];

			if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_NAME),
						 errmsg("parameter name \"%s\" is not a valid BASIC variable name",
								argname),
						 errdetail("BASIC V2 variable names may contain only "
								   "letters and digits."),
						 errhint("Underscores were not invented until later.")));
		}

		/*
		 * The tokenizer crunches keywords anywhere, including inside
		 * identifiers: TOTAL contains TO and becomes token garbage.
		 */
		for (i = 0; basic_keywords[i]; i++)
		{
			if (strstr(var->name, basic_keywords[i]) != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_NAME),
						 errmsg("parameter name \"%s\" contains the BASIC keyword %s",
								argname, basic_keywords[i]),
						 errdetail("The BASIC V2 tokenizer crunches keywords even "
								   "inside variable names, so this name cannot be "
								   "used in a program."),
						 errhint("This is why nobody could ever have a variable "
								 "called TOTAL on the Commodore 64.")));
		}
	}

	/* significant part: first two characters (plus the type suffix) */
	siglen = Min(strlen(var->name), 2);
	memcpy(var->sigkey, var->name, siglen);
	var->sigkey[siglen] = var->suffix ? var->suffix : '#';
	var->sigkey[siglen + 1] = '\0';

	if (strcmp(var->sigkey, "TI#") == 0 || strcmp(var->sigkey, "ST#") == 0 ||
		strncmp(var->name, "TI", 2) == 0 || strncmp(var->name, "ST", 2) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("parameter name \"%s\" shadows a BASIC system variable",
						argname ? argname : var->name),
				 errdetail("TI (the jiffy clock) and ST (I/O status) are "
						   "built into BASIC V2, and only the first two "
						   "characters of a name are significant.")));
}

/*
 * Map all arguments of a function and check for significant-prefix
 * collisions.  Shared by the call handler and the validator.
 * IN, OUT, and INOUT parameters all receive a BASIC variable; VARIADIC
 * and TABLE parameters are rejected.  Returns the total arg count;
 * *vars_out receives a palloc'd array, and *argmodes_out (may be NULL
 * on return, meaning all-IN) the mode array.
 */
static int
map_all_arguments(HeapTuple proctup, plc_var **vars_out, Oid **argtypes_out,
				  char **argmodes_out)
{
	Oid		   *argtypes;
	char	  **argnames;
	char	   *argmodes;
	int			nargs;
	plc_var    *vars;
	int			i,
				j;

	nargs = get_func_arg_info(proctup, &argtypes, &argnames, &argmodes);

	if (argmodes != NULL)
	{
		for (i = 0; i < nargs; i++)
		{
			if (argmodes[i] != PROARGMODE_IN &&
				argmodes[i] != PROARGMODE_OUT &&
				argmodes[i] != PROARGMODE_INOUT)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("PL/CBM-BASIC functions cannot have VARIADIC "
								"or TABLE parameters")));
		}
	}

	vars = palloc(sizeof(plc_var) * Max(nargs, 1));

	for (i = 0; i < nargs; i++)
	{
		map_argument(i, argnames ? argnames[i] : NULL, argtypes[i], &vars[i]);

		for (j = 0; j < i; j++)
		{
			if (strcmp(vars[i].sigkey, vars[j].sigkey) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("parameters \"%s\" and \"%s\" collide in BASIC",
								vars[j].name, vars[i].name),
						 errdetail("Only the first two characters of a BASIC V2 "
								   "variable name are significant, so both become "
								   "\"%.2s%c\".",
								   vars[i].name,
								   vars[i].suffix ? vars[i].suffix : ' ')));
		}
	}

	*vars_out = vars;
	if (argtypes_out)
		*argtypes_out = argtypes;
	if (argmodes_out)
		*argmodes_out = argmodes;
	return nargs;
}

/* ----------------------------------------------------------------
 * Assignment emission: pack "NAME=VALUE" fragments into BASIC lines
 * 0..9, splitting long strings across lines via concatenation.
 * ----------------------------------------------------------------
 */
typedef struct line_packer
{
	StringInfo	prog;			/* program being built */
	int			lineno;			/* next line number to emit */
	int			curlen;			/* chars used on the open line, -1 if none */
} line_packer;

static void
packer_add(line_packer *lp, const char *fragment)
{
	int			fraglen = strlen(fragment);

	Assert(fraglen + 2 <= PLC_LINE_BUDGET);

	if (lp->curlen >= 0 && lp->curlen + 1 + fraglen <= PLC_LINE_BUDGET)
	{
		appendStringInfoChar(lp->prog, ':');
		appendStringInfoString(lp->prog, fragment);
		lp->curlen += 1 + fraglen;
		return;
	}

	if (lp->curlen >= 0)
	{
		appendStringInfoChar(lp->prog, '\n');
		lp->lineno++;
	}
	if (lp->lineno >= PLC_MAX_ARG_LINES)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("argument assignments do not fit in BASIC lines 0 through 9"),
				 errdetail("The BASIC input buffer holds 88 characters per line, "
						   "and PL/CBM-BASIC reserves only lines 0-9 for arguments."),
				 errhint("Pass fewer or shorter arguments. It's 1982; RAM is precious.")));

	appendStringInfo(lp->prog, "%d %s", lp->lineno, fragment);
	lp->curlen = 2 + fraglen;	/* "N " + fragment; fine for one digit */
}

/*
 * Append the assignment fragment(s) for one argument.
 */
static void
emit_assignment(line_packer *lp, const plc_var *var, Oid argtype,
				Datum value, bool isnull)
{
	StringInfoData frag;
	Oid			typoutput;
	bool		typisvarlena;
	char	   *str;

	if (isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("PL/CBM-BASIC cannot pass NULL for parameter \"%s\"",
						var->name),
				 errhint("NULL was not invented until later. "
						 "Declare the function STRICT.")));

	initStringInfo(&frag);

	if (var->suffix == 0 && argtype == BOOLOID)
	{
		/* BASIC convention: true = -1, false = 0 */
		appendStringInfo(&frag, "%s=%d", var->name,
						 DatumGetBool(value) ? -1 : 0);
		packer_add(lp, frag.data);
		return;
	}

	getTypeOutputInfo(argtype, &typoutput, &typisvarlena);
	str = OidOutputFunctionCall(typoutput, value);

	/*
	 * A CBM float cannot represent Infinity or NaN, and PostgreSQL's
	 * text form of them ("Infinity") would silently parse in BASIC as a
	 * never-assigned variable named IN..., i.e. zero.  Reject them, and
	 * reject magnitudes beyond the 40-bit float's ceiling with a nicer
	 * message than BASIC's ?OVERFLOW.  (Values below ~2.9e-39 silently
	 * underflow to zero, exactly as on the hardware.)
	 */
	if (argtype == FLOAT4OID || argtype == FLOAT8OID)
	{
		double		dv = (argtype == FLOAT4OID) ?
			(double) DatumGetFloat4(value) : DatumGetFloat8(value);

		if (isnan(dv) || isinf(dv))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("PL/CBM-BASIC cannot pass %s for parameter \"%s\"",
							isnan(dv) ? "NaN" : "Infinity", var->name),
					 errdetail("The CBM 40-bit float format has no "
							   "representation for it.")));
		if (fabs(dv) > PLC_CBM_FLOAT_MAX)
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("value %s of parameter \"%s\" exceeds the CBM float range",
							str, var->name),
					 errdetail("BASIC V2 floats hold roughly "
							   "\xc2\xb1""2.93873588e-39 through "
							   "\xc2\xb1""1.70141183e38.")));
	}

	if (var->suffix == '$')
	{
		const char *p;
		size_t		len = strlen(str);
		size_t		pos = 0;

		for (p = str; *p; p++)
		{
			if (*p == '"' || (unsigned char) *p < 32)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("PL/CBM-BASIC string arguments may not contain "
								"double quotes or control characters"),
						 errdetail("BASIC V2 string literals have no escape "
								   "mechanism.")));
			if ((unsigned char) *p > 126)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("PL/CBM-BASIC string arguments must be ASCII"),
						 errdetail("The Commodore 64 speaks PETSCII, "
								   "not UTF-8.")));
		}

		if (len > PLC_MAX_STRING)
			ereport(ERROR,
					(errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
					 errmsg("string argument for \"%s$\" exceeds 255 characters",
							var->name),
					 errdetail("BASIC V2 strings hold at most 255 characters.")));

		/*
		 * NAME$="chunk", then NAME$=NAME$+"chunk" continuations, each
		 * fragment sized to fit a line.
		 */
		do
		{
			size_t		overhead;
			size_t		room;
			size_t		take;

			resetStringInfo(&frag);
			if (pos == 0)
				overhead = strlen(var->name) + 1 + 3;	/* NAME$="" */
			else
				overhead = 2 * strlen(var->name) + 2 + 1 + 4;	/* N$=N$+"" */

			room = (PLC_LINE_BUDGET - 2) - overhead;
			take = Min(len - pos, room);

			if (pos == 0)
				appendStringInfo(&frag, "%s$=\"%.*s\"",
								 var->name, (int) take, str + pos);
			else
				appendStringInfo(&frag, "%s$=%s$+\"%.*s\"",
								 var->name, var->name, (int) take, str + pos);
			packer_add(lp, frag.data);
			pos += take;
		} while (pos < len);
	}
	else
	{
		if (var->suffix)
			appendStringInfo(&frag, "%s%c=%s", var->name, var->suffix, str);
		else
			appendStringInfo(&frag, "%s=%s", var->name, str);
		packer_add(lp, frag.data);
	}

	pfree(str);
}

/* ----------------------------------------------------------------
 * OUT parameter extraction: after the program ends, its variables are
 * still sitting in the emulated 64KB RAM.  BASIC V2 keeps simple
 * variables between VARTAB ($2D/$2E) and ARYTAB ($2F/$30) as 7-byte
 * entries: two name bytes (high bits encode the type) plus five data
 * bytes.  We walk that table and decode the values back into Datums.
 * ----------------------------------------------------------------
 */

/*
 * Find a simple variable's 5 data bytes.  Name encoding of the two name
 * bytes (second byte is 0 for one-character names):
 *    float:    (c1,        c2       )
 *    string:   (c1,        c2 | $80 )
 *    integer:  (c1 | $80,  c2 | $80 )
 * (FN definitions are (c1|$80, c2) and thus never match.)
 */
static bool
find_variable(const plc_var *var, const unsigned char **data)
{
	unsigned	vartab = RAM[PLC_VARTAB] | (RAM[PLC_VARTAB + 1] << 8);
	unsigned	arytab = RAM[PLC_ARYTAB] | (RAM[PLC_ARYTAB + 1] << 8);
	unsigned char n1 = var->name[0];
	unsigned char n2 = (strlen(var->name) >= 2) ? var->name[1] : 0;
	unsigned	p;

	if (var->suffix == '%')
	{
		n1 |= 0x80;
		n2 |= 0x80;
	}
	else if (var->suffix == '$')
		n2 |= 0x80;

	for (p = vartab; p + 7 <= arytab && p + 7 <= 65536; p += 7)
	{
		if (RAM[p] == n1 && RAM[p + 1] == n2)
		{
			*data = &RAM[p + 2];
			return true;
		}
	}
	return false;
}

/*
 * Decode a 5-byte CBM float: excess-128 exponent byte, then a 32-bit
 * mantissa in [0.5, 1) with an implied leading 1 whose bit position
 * doubles as the sign.
 */
static double
decode_cbm_float(const unsigned char *d)
{
	uint32		mant;
	double		v;

	if (d[0] == 0)
		return 0.0;
	mant = ((uint32) (d[1] | 0x80) << 24) |
		((uint32) d[2] << 16) |
		((uint32) d[3] << 8) |
		(uint32) d[4];
	v = ldexp((double) mant, (int) d[0] - 128 - 32);
	return (d[1] & 0x80) ? -v : v;
}

/*
 * Convert one OUT parameter's BASIC variable into a Datum of atttype.
 * A variable the program never assigned comes back as SQL NULL.
 */
static Datum
out_param_datum(const plc_var *var, Oid atttype, bool *isnull)
{
	const unsigned char *d;
	char	   *str;
	Oid			typinput;
	Oid			typioparam;
	char		typcategory;
	bool		typispreferred;

	*isnull = false;
	if (!find_variable(var, &d))
	{
		*isnull = true;
		return (Datum) 0;
	}

	if (var->suffix == '%')
	{
		int16		v = (int16) ((d[0] << 8) | d[1]);

		str = psprintf("%d", (int) v);
	}
	else if (var->suffix == '$')
	{
		int			len = d[0];
		unsigned	ptr = d[1] | (d[2] << 8);
		int			i;

		if (ptr + len > 65536)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("string descriptor for \"%s$\" points outside RAM",
							var->name)));
		str = palloc(len + 1);
		for (i = 0; i < len; i++)
		{
			unsigned char c = RAM[ptr + i];

			/* keep it 7-bit clean; PETSCII graphics become '?' */
			str[i] = (c >= 32 && c <= 126) ? (char) c : '?';
		}
		str[len] = '\0';
	}
	else
	{
		double		v = decode_cbm_float(d);

		if (atttype == BOOLOID)
			return BoolGetDatum(v != 0);

		get_type_category_preferred(atttype, &typcategory, &typispreferred);
		if (typcategory == TYPCATEGORY_NUMERIC &&
			(atttype == INT2OID || atttype == INT4OID || atttype == INT8OID))
			str = psprintf("%.0f", rint(v));
		else
			str = psprintf("%.10g", v);
	}

	getTypeInputInfo(atttype, &typinput, &typioparam);
	return OidInputFunctionCall(typinput, str, typioparam, -1);
}

/*
 * Strip ANSI CSI escape sequences (the console layer emits them for
 * PETSCII cursor/colour control codes) and normalise CR/CRLF to LF.
 */
static char *
sanitise_output(const char *raw)
{
	StringInfoData out;
	const char *p = raw;

	initStringInfo(&out);
	while (*p)
	{
		if (*p == '\x1b' && p[1] == '[')
		{
			p += 2;
			while (*p && (*p < 0x40 || *p > 0x7e))
				p++;
			if (*p)
				p++;
		}
		else if (*p == '\r')
		{
			appendStringInfoChar(&out, '\n');
			if (p[1] == '\n')
				p++;
			p++;
		}
		else
			appendStringInfoChar(&out, *p++);
	}

	/*
	 * CHR$() can emit arbitrary bytes, which must not escape into a text
	 * datum as invalid multibyte data.  If the captured output doesn't
	 * verify in the server encoding, flatten the non-ASCII bytes.
	 */
	if (!pg_verifymbstr(out.data, out.len, true))
	{
		char	   *q;

		for (q = out.data; *q; q++)
		{
			if ((unsigned char) *q > 126)
				*q = '?';
		}
	}

	return out.data;
}

/* ----------------------------------------------------------------
 * Device 8: the database.
 *
 * OPEN lfn,8,sec,"SQL" executes the statement immediately; OPEN with
 * no filename (e.g. OPEN 15,8,15) opens an idle channel.  PRINT# to
 * any device-8 channel accumulates bytes and executes them as SQL at
 * the terminating CR (so string concatenation gets you past BASIC's
 * 255-character string limit).  Reading the channel with INPUT#/GET#
 * yields the current result set, one column value per CR-terminated
 * record; ST picks up the EOF bit from the KERNAL exactly as it would
 * from a 1541.  On the command channel (secondary address 15) results
 * are replaced by a DOS-style "0,OK,<rows>,0" status line.
 *
 * The stream handed back to the KERNAL is a fopencookie() wrapper
 * around the struct below, so the vendored CHRIN/GETIN/CHROUT code
 * needs no device-8 knowledge at all.  Everything is palloc'd in the
 * per-call context and the machine is power-cycled between calls, so
 * cleanup is automatic; the FILE itself is closed by CLOSE/CLALL or by
 * the next call's plc_runtime_reset(), with no close callback that
 * could touch already-freed memory after an error.
 * ----------------------------------------------------------------
 */
typedef struct plc_sql_channel
{
	FILE	   *fp;				/* the stream registered with the KERNAL */
	bool		status_channel; /* secondary address 15? */
	StringInfoData sql;			/* statement being PRINT#ed, up to CR */
	StringInfoData result;		/* current readable result stream */
	size_t		readpos;		/* how much of result has been read */
} plc_sql_channel;

/*
 * Append one column value to the result stream as a CR-terminated
 * record.  Values are uppercased and forced to ASCII (this machine
 * speaks PETSCII), clipped to fit INPUT#'s input buffer, and quoted
 * when they contain a character INPUT# would treat as a separator.
 * NULL and the empty string both become a quoted empty string: a bare
 * CR record would be skipped outright by INPUT#, silently shifting
 * every later field by one.  (NULL had not been invented in 1982;
 * COALESCE in the query if the distinction matters.)
 */
static void
append_sql_field(StringInfo buf, const char *value)
{
	char		field[PLC_SQL_FIELD_MAX + 1];
	int			n = 0;
	bool		need_quotes = (value == NULL || value[0] == '\0');
	const char *p;

	if (value != NULL)
	{
		for (p = value; *p && n < PLC_SQL_FIELD_MAX; p++)
		{
			unsigned char c = (unsigned char) *p;

			if (c >= 'a' && c <= 'z')
				c -= 32;
			else if (c == '"')
				c = '\'';		/* BASIC strings cannot contain a quote */
			else if (c < 32)
				c = ' ';
			else if (c > 126)
				c = '?';
			if (c == ',' || c == ';' || c == ':' || c == ' ')
				need_quotes = true;
			field[n++] = (char) c;
		}
	}
	field[n] = '\0';

	if (need_quotes)
		appendStringInfo(buf, "\"%s\"\r", field);
	else
	{
		appendStringInfoString(buf, field);
		appendStringInfoChar(buf, '\r');
	}
}

/*
 * Execute SQL through SPI and rebuild the channel's result stream.
 * SELECT (or anything with a result set) streams every column of every
 * row; other statements yield a single record holding the row count;
 * the command channel yields "0,OK,<rows>,0" DOS status instead.  SQL
 * errors become PostgreSQL errors and abort the function.
 */
static void
plc_sql_execute(plc_sql_channel *ch, const char *sql)
{
	int			rc;
	uint64		processed;

	resetStringInfo(&ch->result);
	ch->readpos = 0;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	rc = SPI_execute(sql, false, 0);
	if (rc < 0)
		elog(ERROR, "SPI_execute failed: %s", SPI_result_code_string(rc));
	processed = SPI_processed;

	if (ch->status_channel)
		appendStringInfo(&ch->result, "0,OK," UINT64_FORMAT ",0\r", processed);
	else if (SPI_tuptable != NULL)
	{
		SPITupleTable *tuptable = SPI_tuptable;
		TupleDesc	tupdesc = tuptable->tupdesc;
		uint64		row;
		int			col;

		/*
		 * appendStringInfo repallocs in the buffer's original (per-call)
		 * context, so building the stream inside the SPI context is safe.
		 */
		for (row = 0; row < tuptable->numvals; row++)
		{
			for (col = 1; col <= tupdesc->natts; col++)
				append_sql_field(&ch->result,
								 SPI_getvalue(tuptable->vals[row],
											  tupdesc, col));
		}
	}
	else
		appendStringInfo(&ch->result, UINT64_FORMAT "\r", processed);

	SPI_finish();

	/* the KERNAL may have seen EOF on the old result; revive the stream */
	clearerr(ch->fp);
	plc_sql_stream_invalidate(ch->fp);
}

static ssize_t
plc_sql_read(void *cookie, char *buf, size_t size)
{
	plc_sql_channel *ch = (plc_sql_channel *) cookie;
	size_t		avail = ch->result.len - ch->readpos;

	if (size > avail)
		size = avail;
	memcpy(buf, ch->result.data + ch->readpos, size);
	ch->readpos += size;
	return size;
}

static ssize_t
plc_sql_write(void *cookie, const char *buf, size_t size)
{
	plc_sql_channel *ch = (plc_sql_channel *) cookie;
	size_t		i;

	for (i = 0; i < size; i++)
	{
		char		c = buf[i];

		if (c == '\r' || c == '\n')
		{
			if (ch->sql.len > 0)
			{
				plc_sql_execute(ch, ch->sql.data);
				resetStringInfo(&ch->sql);
			}
		}
		else
			appendStringInfoChar(&ch->sql, c);
	}
	return size;
}

static int
plc_sql_seek(void *cookie, off64_t *offset, int whence)
{
	/* humour stdio's read/write switching; position is meaningless here */
	*offset = 0;
	return 0;
}

/*
 * OPEN on device 8, called from the vendored KERNAL.  The filename (not
 * NUL-terminated; may be empty) is executed immediately as SQL.
 */
FILE *
plc_sql_open(const unsigned char *name, int namelen, int secaddr)
{
	plc_sql_channel *ch;
	cookie_io_functions_t io = {
		plc_sql_read, plc_sql_write, plc_sql_seek, NULL
	};

	ch = (plc_sql_channel *) palloc0(sizeof(plc_sql_channel));
	ch->status_channel = (secaddr == 15);
	initStringInfo(&ch->sql);
	initStringInfo(&ch->result);

	ch->fp = fopencookie(ch, "r+", io);
	if (ch->fp == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open database channel: %m")));
	setvbuf(ch->fp, NULL, _IONBF, 0);

	if (namelen > 0)
	{
		char	   *sql = pnstrdup((const char *) name, namelen);

		plc_sql_execute(ch, sql);
		pfree(sql);
	}

	return ch->fp;
}

/*
 * Power-cycle the in-process Commodore 64, feed it a complete BASIC
 * program, return sanitised captured output.  Raises an error on
 * interpreter failure, INPUT/GET, or a BASIC runtime error.
 */
static char *
run_basic_program(const char *program)
{
	static FILE *progfile = NULL;
	static bool prng_seeded = false;
	volatile int jmpval = 0;
	char	   *clean;

	/*
	 * The emulated machine (64KB RAM, CPU registers, KERNAL state) is
	 * process-global, so a plcbmbasic function reached through device 8
	 * of another plcbmbasic function would power-cycle its caller.
	 */
	if (plc_machine_on)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("PL/CBM-BASIC functions cannot be called recursively"),
				 errdetail("The emulated Commodore 64 is already running a "
						   "program, and there is only one machine per "
						   "backend.")));

	/* the ROM's RND() draws entropy from rand() via the emulated CIA */
	if (!prng_seeded)
	{
		srand((unsigned int) (MyProcPid ^ time(NULL)));
		prng_seeded = true;
	}

	/* clean up a program file leaked by a previous error/cancel */
	if (progfile != NULL)
	{
		fclose(progfile);
		progfile = NULL;
	}

	progfile = fmemopen(unconstify(char *, program), strlen(program), "r");
	if (progfile == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open in-memory BASIC program: %m")));

	/* power cycle the machine */
	plc_runtime_reset(progfile);
	plc_cpu_reset();
	initStringInfo(&plc_output);
	plc_exit_status = 0;
	plc_basic_errcode = -1;
	plc_basic_errline = 0;

	/*
	 * An ereport from inside the machine (query cancel via the STOP
	 * hook, or an SPI failure on device 8) escapes past the sigsetjmp,
	 * so the busy flag must be cleared on that path too.
	 */
	PG_TRY();
	{
		jmpval = sigsetjmp(plc_escape, 0);
		if (jmpval == 0)
		{
			plc_machine_on = true;
			cbm_main(0, NULL);	/* returns only via siglongjmp */
		}
	}
	PG_FINALLY();
	{
		plc_machine_on = false;
	}
	PG_END_TRY();

	fclose(progfile);
	progfile = NULL;

	if (jmpval == PLC_JMP_WANTS_INPUT)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("BASIC program executed INPUT or GET"),
				 errdetail("There is no keyboard attached to your database."),
				 errhint("Pass data as function arguments instead.")));

	if (jmpval == PLC_JMP_ABORTED)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("cbmbasic interpreter aborted with status %d",
						plc_exit_status),
				 errdetail("Output: %s", plc_output.data)));

	/*
	 * Runtime errors were trapped through the ERROR vector at $0300,
	 * carrying the ROM's error number and the line it happened on.
	 */
	if (plc_basic_errcode >= 0)
	{
		const char *errname =
			(plc_basic_errcode >= 1 && plc_basic_errcode <= PLC_N_BASIC_ERRORS)
			? basic_error_names[plc_basic_errcode] : "UNKNOWN";

		if (plc_basic_errline >= 0xFF00)	/* CURLIN high byte $FF: direct mode */
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("BASIC error: ?%s  ERROR", errname),
					 errdetail("Program was:\n%s", program)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("BASIC error: ?%s  ERROR IN %u",
							errname, plc_basic_errline),
					 errdetail("Program was:\n%s", program)));
	}

	clean = sanitise_output(plc_output.data);

	return clean;
}

/*
 * The call handler: invoked by the fmgr for every call of a plcbmbasic
 * function.
 */
Datum
plcbmbasic_call_handler(PG_FUNCTION_ARGS)
{
	Oid			funcoid = fcinfo->flinfo->fn_oid;
	HeapTuple	proctup;
	Form_pg_proc procstruct;
	Datum		prosrcdatum;
	bool		isnull;
	char	   *prosrc;
	plc_var    *vars;
	Oid		   *argtypes;
	char	   *argmodes;
	int			nargs;
	int			ninputs;
	int			nout;
	line_packer lp;
	StringInfoData prog;
	Oid			rettype;
	char	   *output;
	int			len;
	Datum		result = (Datum) 0;
	int			i;

	if (CALLED_AS_TRIGGER(fcinfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("PL/CBM-BASIC does not support triggers"),
				 errhint("PRESS PLAY ON TAPE.")));
	if (CALLED_AS_EVENT_TRIGGER(fcinfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("PL/CBM-BASIC does not support event triggers")));

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcoid));
	if (!HeapTupleIsValid(proctup))
		elog(ERROR, "cache lookup failed for function %u", funcoid);
	procstruct = (Form_pg_proc) GETSTRUCT(proctup);
	rettype = procstruct->prorettype;

	prosrcdatum = SysCacheGetAttr(PROCOID, proctup,
								  Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		elog(ERROR, "null prosrc for function %u", funcoid);
	prosrc = TextDatumGetCString(prosrcdatum);

	nargs = map_all_arguments(proctup, &vars, &argtypes, &argmodes);
	ReleaseSysCache(proctup);

	ninputs = 0;
	nout = 0;
	for (i = 0; i < nargs; i++)
	{
		char		mode = argmodes ? argmodes[i] : PROARGMODE_IN;

		if (mode == PROARGMODE_IN || mode == PROARGMODE_INOUT)
			ninputs++;
		if (mode == PROARGMODE_OUT || mode == PROARGMODE_INOUT)
			nout++;
	}
	if (ninputs != PG_NARGS())
		elog(ERROR, "argument count mismatch");

	/* assemble the full program: assignments on lines 0..9, then source */
	initStringInfo(&prog);
	lp.prog = &prog;
	lp.lineno = 0;
	lp.curlen = -1;

	ninputs = 0;
	for (i = 0; i < nargs; i++)
	{
		char		mode = argmodes ? argmodes[i] : PROARGMODE_IN;

		if (mode == PROARGMODE_OUT)
			continue;			/* no incoming value; program will set it */
		emit_assignment(&lp, &vars[i], argtypes[i],
						PG_ARGISNULL(ninputs) ? (Datum) 0 : PG_GETARG_DATUM(ninputs),
						PG_ARGISNULL(ninputs));
		ninputs++;
	}

	if (lp.curlen >= 0)
		appendStringInfoChar(&prog, '\n');
	appendStringInfoString(&prog, prosrc);
	appendStringInfoChar(&prog, '\n');
	basic_toupper(prog.data);

	output = run_basic_program(prog.data);

	/*
	 * With OUT/INOUT parameters, the result comes from the emulated
	 * machine's variable table, not from PRINTed output (which is
	 * scanned for errors above but otherwise discarded).
	 */
	if (nout > 0)
	{
		TupleDesc	tupdesc;
		Oid			restype;

		switch (get_call_result_type(fcinfo, &restype, &tupdesc))
		{
			case TYPEFUNC_COMPOSITE:
				{
					Datum	   *values = palloc(sizeof(Datum) * nout);
					bool	   *nulls = palloc(sizeof(bool) * nout);
					int			attno = 0;
					HeapTuple	tup;

					for (i = 0; i < nargs; i++)
					{
						char		mode = argmodes ? argmodes[i] : PROARGMODE_IN;

						if (mode != PROARGMODE_OUT && mode != PROARGMODE_INOUT)
							continue;
						values[attno] =
							out_param_datum(&vars[i],
											TupleDescAttr(tupdesc, attno)->atttypid,
											&nulls[attno]);
						attno++;
					}
					tupdesc = BlessTupleDesc(tupdesc);
					tup = heap_form_tuple(tupdesc, values, nulls);
					PG_RETURN_DATUM(HeapTupleGetDatum(tup));
				}

			case TYPEFUNC_SCALAR:
				{
					bool		outnull = true;

					for (i = 0; i < nargs; i++)
					{
						char		mode = argmodes ? argmodes[i] : PROARGMODE_IN;

						if (mode == PROARGMODE_OUT || mode == PROARGMODE_INOUT)
						{
							result = out_param_datum(&vars[i], restype, &outnull);
							break;
						}
					}
					if (outnull)
					{
						fcinfo->isnull = true;
						return (Datum) 0;
					}
					PG_RETURN_DATUM(result);
				}

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("could not determine result type for OUT parameters")));
		}
	}

	if (rettype == VOIDOID)
		PG_RETURN_VOID();

	/* trim trailing newlines */
	len = strlen(output);
	while (len > 0 && output[len - 1] == '\n')
		output[--len] = '\0';

	if (len == 0)
	{
		fcinfo->isnull = true;
		return (Datum) 0;
	}

	if (rettype == TEXTOID)
		result = PointerGetDatum(cstring_to_text_with_len(output, len));
	else
	{
		/*
		 * For non-text types, take the last non-empty line, strip the
		 * spaces BASIC pads numbers with, and run it through the return
		 * type's input function.
		 */
		char	   *lastline = output;
		char	   *nl;
		char	   *end;
		Oid			typinput;
		Oid			typioparam;

		while ((nl = strchr(lastline, '\n')) != NULL && nl[1] != '\0')
			lastline = nl + 1;
		while (*lastline == ' ')
			lastline++;
		end = lastline + strlen(lastline);
		while (end > lastline && (end[-1] == ' ' || end[-1] == '\n'))
			*--end = '\0';

		getTypeInputInfo(rettype, &typinput, &typioparam);
		result = OidInputFunctionCall(typinput, lastline, typioparam, -1);
	}

	PG_RETURN_DATUM(result);
}

/*
 * Inline handler: DO $$ 10 PRINT "HI" $$ LANGUAGE plcbmbasic;
 * Output is raised as a NOTICE.
 */
Datum
plcbmbasic_inline_handler(PG_FUNCTION_ARGS)
{
	InlineCodeBlock *codeblock = (InlineCodeBlock *) PG_GETARG_POINTER(0);
	StringInfoData prog;
	char	   *output;
	int			len;

	initStringInfo(&prog);
	appendStringInfoString(&prog, codeblock->source_text);
	appendStringInfoChar(&prog, '\n');
	basic_toupper(prog.data);

	output = run_basic_program(prog.data);

	len = strlen(output);
	while (len > 0 && output[len - 1] == '\n')
		output[--len] = '\0';

	if (len > 0)
		ereport(NOTICE, (errmsg("%s", output)));

	PG_RETURN_VOID();
}

/*
 * Validator: runs at CREATE FUNCTION time, so BASIC-incompatible
 * parameter names (keywords inside identifiers, colliding two-character
 * prefixes, system variables, underscores) are rejected before anyone
 * gets a mystifying ?SYNTAX ERROR at runtime.
 */
Datum
plcbmbasic_validator(PG_FUNCTION_ARGS)
{
	Oid			funcoid = PG_GETARG_OID(0);
	HeapTuple	proctup;
	Form_pg_proc procstruct;
	Oid			rettype;
	plc_var    *vars;

	if (!CheckFunctionValidatorAccess(fcinfo->flinfo->fn_oid, funcoid))
		PG_RETURN_VOID();

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcoid));
	if (!HeapTupleIsValid(proctup))
		elog(ERROR, "cache lookup failed for function %u", funcoid);
	procstruct = (Form_pg_proc) GETSTRUCT(proctup);
	rettype = procstruct->prorettype;

	if (procstruct->proretset)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("PL/CBM-BASIC functions cannot return sets"),
				 errhint("One value at a time, please. It's 1982.")));

	if (rettype == TRIGGEROID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("PL/CBM-BASIC does not support triggers"),
				 errhint("PRESS PLAY ON TAPE.")));
	if (rettype == EVENT_TRIGGEROID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("PL/CBM-BASIC does not support event triggers")));
	if (get_typtype(rettype) == TYPTYPE_PSEUDO &&
		rettype != VOIDOID && rettype != RECORDOID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("PL/CBM-BASIC functions cannot return type %s",
						format_type_be(rettype))));

	(void) map_all_arguments(proctup, &vars, NULL, NULL);

	/*
	 * Check that every line of the body carries a line number in the
	 * range the language can honour: lines 0-9 are reserved for
	 * argument assignments (a user line there would silently replace
	 * them), and BASIC V2 tops out at 63999.
	 */
	if (check_function_bodies)
	{
		Datum		prosrcdatum;
		bool		isnull;
		char	   *prosrc;
		char	   *line;
		char	   *saveptr = NULL;

		prosrcdatum = SysCacheGetAttr(PROCOID, proctup,
									  Anum_pg_proc_prosrc, &isnull);
		if (isnull)
			elog(ERROR, "null prosrc for function %u", funcoid);
		prosrc = TextDatumGetCString(prosrcdatum);

		for (line = strtok_r(prosrc, "\n", &saveptr);
			 line != NULL;
			 line = strtok_r(NULL, "\n", &saveptr))
		{
			char	   *p = line;
			long		lineno;

			while (*p == ' ' || *p == '\t' || *p == '\r')
				p++;
			if (*p == '\0')
				continue;
			if (*p < '0' || *p > '9')
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("every line of a PL/CBM-BASIC function must "
								"begin with a line number"),
						 errdetail("Offending line: %s", line)));
			lineno = strtol(p, NULL, 10);
			if (lineno < PLC_MAX_ARG_LINES)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("BASIC line number %ld is reserved", lineno),
						 errdetail("Lines 0 through 9 hold the generated "
								   "argument assignments; user code starts "
								   "at line 10.")));
			if (lineno > PLC_MAX_LINENO)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("BASIC line number %ld is too large", lineno),
						 errdetail("BASIC V2 line numbers stop at %d.",
								   PLC_MAX_LINENO)));
		}
	}

	ReleaseSysCache(proctup);
	PG_RETURN_VOID();
}
