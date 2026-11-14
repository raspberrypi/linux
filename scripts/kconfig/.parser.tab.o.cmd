savedcmd_scripts/kconfig/parser.tab.o := gcc -Wp,-MMD,scripts/kconfig/.parser.tab.o.d -Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu11   -I ./scripts/include  -I scripts/kconfig -c -o scripts/kconfig/parser.tab.o scripts/kconfig/parser.tab.c

source_scripts/kconfig/parser.tab.o := scripts/kconfig/parser.tab.c

deps_scripts/kconfig/parser.tab.o := \
  scripts/include/xalloc.h \
  scripts/kconfig/lkc.h \
    $(wildcard include/config/prefix) \
  scripts/kconfig/expr.h \
  scripts/include/list_types.h \
  scripts/kconfig/lkc_proto.h \
  scripts/kconfig/internal.h \
  scripts/include/hashtable.h \
  scripts/include/array_size.h \
  scripts/include/list.h \
  scripts/kconfig/preprocess.h \
  scripts/kconfig/parser.tab.h \

scripts/kconfig/parser.tab.o: $(deps_scripts/kconfig/parser.tab.o)

$(deps_scripts/kconfig/parser.tab.o):
