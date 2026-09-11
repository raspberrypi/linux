savedcmd_scripts/kconfig/confdata.o := gcc -Wp,-MMD,scripts/kconfig/.confdata.o.d -Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu11   -I ./scripts/include   -c -o scripts/kconfig/confdata.o scripts/kconfig/confdata.c

source_scripts/kconfig/confdata.o := scripts/kconfig/confdata.c

deps_scripts/kconfig/confdata.o := \
    $(wildcard include/config/FOO) \
    $(wildcard include/config/X) \
  scripts/include/xalloc.h \
  scripts/kconfig/internal.h \
  scripts/include/hashtable.h \
  scripts/include/array_size.h \
  scripts/include/list.h \
  scripts/include/list_types.h \
  scripts/kconfig/lkc.h \
    $(wildcard include/config/prefix) \
  scripts/kconfig/expr.h \
  scripts/kconfig/lkc_proto.h \

scripts/kconfig/confdata.o: $(deps_scripts/kconfig/confdata.o)

$(deps_scripts/kconfig/confdata.o):
