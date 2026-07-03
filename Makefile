MODULE_big = plcbmbasic
OBJS = plcbmbasic.o cbm_rom.o cbm_runtime.o cbm_plugin.o cbm_console.o
EXTENSION = plcbmbasic
DATA = plcbmbasic--1.0.sql
REGRESS = plcbmbasic

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Redirect the emulated machine's I/O and exit into our handler
# (plc_* replacements in plcbmbasic.c). -U_FORTIFY_SOURCE prevents
# glibc from macro-wrapping printf before our rename can take effect.
CBM_WRAP = -U_FORTIFY_SOURCE -D__NO_INLINE__ -Dexit=plc_exit -Dgetchar=plc_getchar \
           -Dputchar=plc_putchar -Dprintf=plc_printf

cbm_rom.o: CPPFLAGS += -Dmain=cbm_main $(CBM_WRAP)
cbm_rom.o: CFLAGS += -w
cbm_runtime.o cbm_plugin.o cbm_console.o: CPPFLAGS += $(CBM_WRAP)
cbm_runtime.o cbm_plugin.o cbm_console.o: CFLAGS += -w
