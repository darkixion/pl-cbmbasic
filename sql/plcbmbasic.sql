CREATE EXTENSION plcbmbasic;

-- basic call with a text argument
CREATE FUNCTION hello(who text) RETURNS text AS $$
10 PRINT "HELLO, ";WHO$;"!"
$$ LANGUAGE plcbmbasic;
SELECT hello('WORLD');

-- numeric types: smallint becomes a % variable, float a plain one
CREATE FUNCTION scoreboard(player text, lives smallint, mult float8)
RETURNS text AS $$
10 PRINT "PLAYER: ";PLAYER$
20 PRINT "LIVES:";LIVES%
30 FOR I=1 TO LIVES%
40 P=P+100*MULT
50 NEXT
60 PRINT "BONUS:";P
$$ LANGUAGE plcbmbasic;
SELECT scoreboard('BLASTER', 3::smallint, 1.5);

-- non-text return types parse the last PRINTed line
CREATE FUNCTION fact(num int) RETURNS bigint AS $$
10 F=1
20 FOR I=1 TO NUM
30 F=F*I
40 NEXT I
50 PRINT F
$$ LANGUAGE plcbmbasic;
SELECT n, fact(n) FROM generate_series(1, 5) n;

-- OUT parameters come back from the machine's variable table
CREATE FUNCTION divmod(num int, den int, OUT quot int, OUT rmd int) AS $$
10 QUOT=INT(NUM/DEN)
20 RMD=NUM-QUOT*DEN
$$ LANGUAGE plcbmbasic;
SELECT * FROM divmod(47, 5);

CREATE FUNCTION swap(INOUT x int, INOUT y int) AS $$
10 T=X:X=Y:Y=T
$$ LANGUAGE plcbmbasic;
SELECT * FROM swap(1, 2);

-- DO blocks report PRINTed output as a NOTICE
DO $$
10 FOR I=1 TO 3
20 PRINT "COMMODORE";I
30 NEXT
$$ LANGUAGE plcbmbasic;

-- device 8 is the database: SELECT through OPEN, rows via INPUT#, EOF in ST
CREATE TABLE hiscores(name text, score int);
INSERT INTO hiscores VALUES ('JEFF', 8500), ('AVRIL', 12000), ('BOB', 3200);

CREATE FUNCTION top_scores() RETURNS text AS $$
10 OPEN 1,8,0,"SELECT NAME, SCORE FROM HISCORES ORDER BY SCORE DESC"
20 INPUT#1,N$,S
30 PRINT N$;" ";S
40 IF ST=0 THEN 20
50 CLOSE 1
$$ LANGUAGE plcbmbasic;
SELECT top_scores();

-- the command channel executes PRINT#ed SQL and answers in DOS style
CREATE FUNCTION bump_scores() RETURNS text AS $$
10 OPEN 15,8,15
20 PRINT#15,"UPDATE HISCORES SET SCORE=SCORE+1 ";
30 PRINT#15,"WHERE SCORE > 5000"
40 INPUT#15,EN,EM$,RC,ES
50 PRINT "STATUS: ";EN;EM$;RC
60 CLOSE 15
$$ LANGUAGE plcbmbasic;
SELECT bump_scores();

-- DML on a data channel yields the row count
DO $$
10 OPEN 1,8,0,"DELETE FROM HISCORES WHERE SCORE < 4000"
20 INPUT#1,N
30 PRINT "DELETED";N;"ROWS"
40 CLOSE 1
$$ LANGUAGE plcbmbasic;

-- values with separators come back quoted; NULLs as empty strings
CREATE TABLE oddities(a text, b int);
INSERT INTO oddities VALUES ('HELLO, WORLD', 1), (NULL, 2);
CREATE FUNCTION read_oddities() RETURNS text AS $$
10 OPEN 1,8,0,"SELECT A, B FROM ODDITIES ORDER BY B"
20 INPUT#1,A$,B
30 PRINT "[";A$;"]";B
40 IF ST=0 THEN 20
$$ LANGUAGE plcbmbasic;
SELECT read_oddities();

-- GET# reads the stream byte by byte
CREATE FUNCTION firstbyte() RETURNS text AS $$
10 OPEN 1,8,0,"SELECT 'HELLO'"
20 GET#1,C$
30 PRINT "FIRST BYTE: ";C$
40 CLOSE 1
$$ LANGUAGE plcbmbasic;
SELECT firstbyte();

-- runtime errors arrive through the ERROR vector with code and line
CREATE FUNCTION broken() RETURNS text AS $$
10 X=1
20 PRINT 1/0
$$ LANGUAGE plcbmbasic;
SELECT broken();

CREATE FUNCTION lost() RETURNS text AS $$
10 GOTO 999
$$ LANGUAGE plcbmbasic;
SELECT lost();

-- printing something that merely looks like an error is fine
CREATE FUNCTION fake_error() RETURNS text AS $$
10 PRINT "?FAKE  ERROR IN 99"
$$ LANGUAGE plcbmbasic;
SELECT fake_error();

-- SQL errors from device 8 surface as PostgreSQL errors
DO $$
10 OPEN 1,8,0,"SELECT * FROM NO_SUCH_TABLE"
$$ LANGUAGE plcbmbasic;

-- one machine per backend: no recursion through device 8
CREATE FUNCTION inner_fn() RETURNS int AS $$
10 PRINT 42
$$ LANGUAGE plcbmbasic;
CREATE FUNCTION outer_fn() RETURNS text AS $$
10 OPEN 1,8,0,"SELECT INNER_FN()"
$$ LANGUAGE plcbmbasic;
SELECT outer_fn();

-- there is no keyboard attached to your database
DO $$
10 INPUT A$
$$ LANGUAGE plcbmbasic;

-- CHR$ cannot smuggle invalid bytes into a text datum
CREATE FUNCTION highbytes() RETURNS text AS $$
10 PRINT "A";CHR$(200);"B"
$$ LANGUAGE plcbmbasic;
SELECT highbytes();

-- validator: BASIC's opinions on names are delivered at CREATE time
CREATE FUNCTION bad1(total int) RETURNS int AS $$
10 PRINT TOTAL
$$ LANGUAGE plcbmbasic;
CREATE FUNCTION bad2(alpha text, alps text) RETURNS int AS $$
10 PRINT ALPHA$
$$ LANGUAGE plcbmbasic;
CREATE FUNCTION bad3(times int) RETURNS int AS $$
10 PRINT TIMES
$$ LANGUAGE plcbmbasic;
CREATE FUNCTION bad4(my_var int) RETURNS int AS $$
10 PRINT MYVAR
$$ LANGUAGE plcbmbasic;

-- validator: line numbers below 10 are reserved, and all lines need one
CREATE FUNCTION bad5(x int) RETURNS int AS $$
5 PRINT X
$$ LANGUAGE plcbmbasic;
CREATE FUNCTION bad6() RETURNS int AS $$
PRINT 42
$$ LANGUAGE plcbmbasic;

-- validator: no sets, no triggers
CREATE FUNCTION bad7() RETURNS SETOF int AS $$
10 PRINT 1
$$ LANGUAGE plcbmbasic;
CREATE FUNCTION bad8() RETURNS trigger AS $$
10 PRINT 1
$$ LANGUAGE plcbmbasic;

-- NULL arguments are rejected
SELECT hello(NULL);

DROP TABLE hiscores, oddities;
