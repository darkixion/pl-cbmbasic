# PL/CBM-BASIC

Commodore 64 BASIC V2 as a PostgreSQL procedural language. Function bodies
are executed by [cbmbasic](https://github.com/mist64/cbmbasic) - a static
recompilation of the original 6502 BASIC ROM - so this is the real
Microsoft/Commodore BASIC from 1982, not an imitation.

cbmbasic is not an emulator reading a ROM file: the 6502 ROM is
statically recompiled into plain C, which is compiled directly into the
extension's shared library and runs inside the backend process. Each
function call is an in-memory power cycle - zero the 64KB RAM array,
reset the CPU registers, re-enter the recompiled ROM at BASIC's $E394
entry point - so it amounts to a memset and a function call. There are
no subprocesses, temp files, or pipes, and a call costs on the order of
15-20 microseconds instead of the 1-2 milliseconds of a fork/exec
design; per-row use over large tables is practical. The ROM's I/O and exit
routines are redirected at compile time
(-Dputchar=... -Dexit=... plus -D__NO_INLINE__ so glibc's extern inlines
don't reroute them back to the real stdout) into buffer-based
replacements, and the STOP KERNAL call - which BASIC polls before every
statement - is patched to CHECK_FOR_INTERRUPTS(), so runaway programs
like 10 GOTO 10 respond to query cancellation and statement_timeout.

Tested against PostgreSQL 20devel on Linux.

## Build and install

The cbmbasic sources are vendored (cbm_rom.c is the recompiled ROM,
cbm_runtime.c the KERNAL, plus plugin/console), so there is nothing to
install besides the extension itself:

```sh
# needs postgresql-server-dev-XX / pgxs
make && sudo make install
make installcheck   # regression tests, needs a running server

# in your database (superuser)
CREATE EXTENSION plcbmbasic;
```

## Usage

```sql
CREATE FUNCTION hello(who text) RETURNS text AS $$
10 PRINT "HELLO, ";WHO$;"!"
$$ LANGUAGE plcbmbasic;

SELECT hello('WORLD');   -- HELLO, WORLD!

CREATE FUNCTION scoreboard(player text, lives smallint, mult float8)
RETURNS text AS $$
10 PRINT "PLAYER: ";PLAYER$
20 PRINT "LIVES:";LIVES%
30 FOR I=1 TO LIVES%
40 P=P+100*MULT
50 NEXT
60 PRINT "BONUS:";P
$$ LANGUAGE plcbmbasic;

CREATE FUNCTION fact(num int) RETURNS bigint AS $$
10 F=1
20 FOR I=1 TO NUM
30 F=F*I
40 NEXT I
50 PRINT F
$$ LANGUAGE plcbmbasic;

SELECT n, fact(n) FROM generate_series(1,5) n;

DO $$
10 FOR I=1 TO 3
20 PRINT "COMMODORE";I
30 NEXT
$$ LANGUAGE plcbmbasic;   -- output arrives as a NOTICE
```

## Talking to the database: device 8

On a Commodore 64 your data lives on the disk drive, device 8, and you
talk to it with OPEN, INPUT#, GET#, PRINT#, CLOSE, and the ST status
variable. Here, device 8 is the database. The "filename" you OPEN is a
SQL statement, executed through SPI inside your transaction:

```sql
CREATE FUNCTION top_scores() RETURNS text AS $$
10 OPEN 1,8,0,"SELECT NAME, SCORE FROM HISCORES ORDER BY SCORE DESC"
20 INPUT#1,N$,S
30 PRINT N$;" ";S
40 IF ST=0 THEN 20
50 CLOSE 1
$$ LANGUAGE plcbmbasic;
```

Result sets stream one column value per CR-terminated record, so a row
of N columns is one INPUT# with N variables (or N separate INPUT#s, or
GET# a byte at a time if that is how you like to live). ST picks up the
EOF bit (64) on the final byte, exactly as it would from a 1541, so the
classic read-until-done loop works unchanged. Data channel details:

- Values are uppercased and forced to ASCII on the way out (this machine
  speaks PETSCII), and clipped to 78 characters so a record always fits
  INPUT#'s 88-byte input buffer.
- Values containing a comma, colon, semicolon, space, or quote arrive
  wrapped in quotes (embedded `"` becomes `'`), which INPUT# consumes
  natively. Numbers arrive bare, so INPUT# into numeric variables works.
- NULL and the empty string both arrive as `""` - a bare empty record
  would be skipped outright by INPUT#, silently shifting every later
  field by one. NULL had not been invented in 1982; COALESCE in the
  query if the distinction matters.
- A statement without a result set (INSERT, UPDATE, DELETE, DDL) yields
  a single record holding the number of rows affected.
- SQL errors are raised as ordinary PostgreSQL errors and abort the
  function; there is no ON ERROR GOTO. Query text is uppercased with the
  rest of the source, which is harmless for identifiers (PostgreSQL
  folds them to lower case) but means string literals in your SQL are
  uppercase too. Naturally your data is already uppercase.

Secondary address 15 is the DOS command channel, just like a real drive:
PRINT# statements to it (of any length - each PRINT# appends, and the
terminating CR executes, so string concatenation gets you past both the
88-character line and 255-character string limits) and read back a
DOS-style status:

```sql
CREATE FUNCTION prune() RETURNS text AS $$
10 OPEN 15,8,15
20 PRINT#15,"DELETE FROM HISCORES ";
30 PRINT#15,"WHERE SCORE < 1000"
40 INPUT#15,EN,EM$,RC,ES
50 PRINT "STATUS: ";EN;EM$;RC
60 CLOSE 15
$$ LANGUAGE plcbmbasic;
```

The status record is `0,OK,<rows>,0`, in honour of `00, OK,00,00`.
PRINT# works on data channels too: the executed statement replaces that
channel's result set.

One machine per backend: a query on device 8 that calls another
plcbmbasic function is refused (the inner call would power-cycle the
outer program's RAM), and a function that queries the database should be
declared VOLATILE, which is the default.

## Calling conventions

**Arguments** are injected as assignments on BASIC lines 0-9 before your
code runs (user code starts at line 10, like nature intended; the
validator enforces this).

Named parameters become named BASIC variables, with the type spelled the
way Commodore intended:

| Parameter            | BASIC variable | Notes                              |
|----------------------|----------------|------------------------------------|
| `who text`           | `WHO$`         | string (max 255 chars, ASCII only) |
| `age smallint`       | `AGE%`         | a real 16-bit integer variable     |
| `pts float8` / `int` | `PTS`          | float variable                     |
| `flag boolean`       | `FLAG`         | `-1` true / `0` false, per custom  |

Unnamed parameters fall back to positional letters: argument 1 is
`A`/`A$`/`A%`, argument 2 is `B`..., up to 26.

The suffix-less float variables are the CBM 40-bit format (8-bit
excess-128 exponent, 32-bit mantissa): about 9 significant digits,
magnitudes from ±2.93873588e-39 to ±1.70141183e38. PL/CBM-BASIC polices
that range on the way in: NaN and Infinity are rejected (BASIC would
silently read `V=INFINITY` as an unset variable, i.e. zero), magnitudes
above 1.70141183e38 are rejected with the range in the message, and
values below the floor silently underflow to zero, exactly as on the
hardware.

**OUT and INOUT parameters** work, and the mechanism is the best part:
when the program ends, its variables are still sitting in the emulated
64KB RAM, so the handler walks BASIC's own variable table (VARTAB at
$2D/$2E through ARYTAB at $2F/$30, 7-byte entries: two type-encoded name
bytes plus five data bytes) and decodes the values back into SQL -
5-byte CBM floats, 16-bit `%` integers, and `$` string descriptors
pointing into string memory. So:

```sql
CREATE FUNCTION divmod(num int, den int, OUT quot int, OUT rmd int) AS $$
10 QUOT=INT(NUM/DEN)
20 RMD=NUM-QUOT*DEN
$$ LANGUAGE plcbmbasic;

SELECT * FROM divmod(47, 5);   --  quot | rmd
                               -- ------+-----
                               --     9 |   2

CREATE FUNCTION swap(INOUT x int, INOUT y int) AS $$
10 T=X:X=Y:Y=T
$$ LANGUAGE plcbmbasic;
```

Multiple OUT parameters come back as a record, a single OUT as a plain
scalar, and an OUT variable the program never assigned comes back as SQL
NULL. When a function has OUT parameters, PRINTed output is discarded.
VARIADIC, RETURNS TABLE, RETURNS SETOF, and triggers are not supported,
and the validator says so at CREATE FUNCTION time.

BASIC V2 has opinions about names, and the extension ships a VALIDATOR so
you hear those opinions at `CREATE FUNCTION` time rather than as a runtime
`?SYNTAX ERROR`:

- **Keywords crunch anywhere.** The tokenizer replaces keyword text even
  inside identifiers, so `total` (contains `TO`), `score` (`OR`), and
  `budget` (`GET`) are all rejected. This is why nobody on a real C64
  ever had a variable called TOTAL.
- **Only two characters are significant.** `alpha text, alps text` both
  become `AL$` and are rejected as a collision. (`username text,
  userid int` is fine: `US$` and `US` are different variables.)
- **`TI` and `ST` are taken** (jiffy clock and I/O status), so any name
  whose first two letters are TI or ST is rejected.
- **Letters and digits only**, starting with a letter. Underscores were
  not invented until later.

The validator also checks the body: every line must carry a line number,
lines 0-9 are reserved (a user line there would silently replace an
argument assignment), and 63999 is the biggest number BASIC will take.

Long text arguments are automatically built up across lines 0-9 by string
concatenation (the BASIC input buffer holds only 88 characters per line);
if the combined assignments don't fit in ten lines, you get an error.
Other fine print:

- String arguments may not contain `"` or control characters (BASIC V2
  has no string escapes), must be ASCII (the C64 speaks PETSCII, not
  UTF-8), and must fit in 255 characters (the BASIC string maximum).
- NULL arguments raise an error - declare your functions STRICT.

**Return values** come from whatever the program PRINTs:

- `RETURNS text` - the entire captured output, trailing newlines trimmed.
- `RETURNS void` - output is discarded.
- Any other type - the last non-empty line of output, trimmed of the
  padding spaces BASIC puts around numbers, fed through the type's input
  function.
- No output at all - NULL.

Bytes that PRINT CHR$() emits outside ASCII are flattened to `?` rather
than being allowed to corrupt a text datum.

**Errors**: BASIC runtime errors are trapped through the interpreter's
own ERROR vector at $0300 (the same plugin facility Simons' BASIC used),
so the handler receives the genuine error number and line and reports
`?DIVISION BY ZERO  ERROR IN 20` as a PostgreSQL error with your program
attached as the DETAIL. Output is not scraped for error text, so you may
PRINT strings that look like errors to your heart's content.

**Source is uppercased** before execution, string literals included,
because BASIC V2 only tokenizes uppercase keywords and the C64's default
character set is uppercase anyway.

## Limitations (a non-exhaustive list, obviously)

- **Untrusted language.** BASIC V2 has `LOAD`, `SAVE`, and `OPEN` (to
  devices other than 8), and the interpreter runs as the postgres OS
  user, so only superusers may create plcbmbasic functions. Do not
  install this anywhere that matters. (The vendored SYSTEM shell-escape
  keyword has been disabled, and TI$ assignment now sets an emulated
  clock offset instead of calling settimeofday() on the host, which you
  would probably not have enjoyed.)
- Numbers are 40-bit CBM floats: about 9 significant digits. `bigint`
  arguments beyond that lose precision, and large factorials come back in
  scientific notation that may not parse as `bigint`.
- Each call power-cycles the machine in memory, so there is no state
  between calls and no triggers. The interpreter's 64KB RAM and CPU
  state are process globals: fine for PostgreSQL's process-per-backend
  model, but not thread-safe, and the reason recursive calls are
  refused.
- Column values longer than 78 characters are clipped on their way
  through device 8; SUBSTR in the query if you need more, or reconsider
  whether a C64 is the right ETL tool.
- `INPUT` and `GET` from the keyboard raise an error (there is no
  keyboard attached to your database); pass data as function arguments
  or read it from device 8 instead.
- At most 26 *unnamed* arguments (positional variables are A through Z);
  named parameters are limited only by distinct two-character prefixes.
- `POKE` and `SYS` do work against the emulated 64KB address space, if
  you enjoy danger. A wild POKE can at worst confuse the emulated
  machine until the next call's power cycle; it cannot touch backend
  memory outside the RAM array.

## Files

- `plcbmbasic.c` - call handler, inline (DO block) handler, validator,
  the buffer-based I/O replacements the emulated machine is wired to,
  and the device-8 SQL channel (SPI behind a fopencookie() stream)
- `cbm_rom.c` - the recompiled BASIC ROM (vendored, unmodified except
  `main` is renamed to `cbm_main` at compile time)
- `cbm_runtime.c` - the KERNAL (vendored; patched to hook
  CHECK_FOR_INTERRUPTS into STOP, route device 8 to SQL, trap the ERROR
  vector, keep TI$ off the host clock, and add power-cycle resets)
- `cbm_plugin.c`, `cbm_console.c` - vendored support code (SYSTEM
  disabled, error hook added)
- `plcbmbasic--1.0.sql` - creates the handlers, the validator, and the
  language
- `plcbmbasic.control`, `Makefile` - PGXS plumbing, including the
  -D macro injection that redirects the machine's I/O
- `sql/`, `expected/` - regression tests (`make installcheck`)
