\echo Use "CREATE EXTENSION plcbmbasic" to load this file. \quit

CREATE FUNCTION plcbmbasic_call_handler()
RETURNS language_handler
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION plcbmbasic_inline_handler(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION plcbmbasic_validator(oid)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

-- Untrusted: BASIC V2 has LOAD/SAVE/OPEN, so it can touch the server
-- filesystem. Only superusers may create plcbmbasic functions.
CREATE LANGUAGE plcbmbasic
    HANDLER plcbmbasic_call_handler
    INLINE plcbmbasic_inline_handler
    VALIDATOR plcbmbasic_validator;

COMMENT ON LANGUAGE plcbmbasic IS
    'Commodore 64 BASIC V2 (READY.)';
