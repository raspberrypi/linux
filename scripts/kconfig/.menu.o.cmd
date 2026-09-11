savedcmd_scripts/kconfig/menu.o := gcc -Wp,-MMD,scripts/kconfig/.menu.o.d -Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu11   -I ./scripts/include   -c -o scripts/kconfig/menu.o scripts/kconfig/menu.c

source_scripts/kconfig/menu.o := scripts/kconfig/menu.c

deps_scripts/kconfig/menu.o := \
  scripts/include/list.h \
  scripts/include/list_types.h \
  scripts/include/xalloc.h \
  scripts/kconfig/lkc.h \
    $(wildcard include/config/prefix) \
  scripts/kconfig/expr.h \
  scripts/kconfig/lkc_proto.h \
  scripts/kconfig/internal.h \
  scripts/include/hashtable.h \
  scripts/include/array_size.h \

scripts/kconfig/menu.o: $(deps_scripts/kconfig/menu.o)

$(deps_scripts/kconfig/menu.o):
