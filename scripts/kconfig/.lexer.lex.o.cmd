savedcmd_scripts/kconfig/lexer.lex.o := gcc -Wp,-MMD,scripts/kconfig/.lexer.lex.o.d -Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu11   -I ./scripts/include  -I scripts/kconfig -c -o scripts/kconfig/lexer.lex.o scripts/kconfig/lexer.lex.c

source_scripts/kconfig/lexer.lex.o := scripts/kconfig/lexer.lex.c

deps_scripts/kconfig/lexer.lex.o := \
  scripts/include/xalloc.h \
  scripts/kconfig/lkc.h \
    $(wildcard include/config/prefix) \
  scripts/kconfig/expr.h \
  scripts/include/list_types.h \
  scripts/kconfig/lkc_proto.h \
  scripts/kconfig/preprocess.h \
  scripts/kconfig/parser.tab.h \

scripts/kconfig/lexer.lex.o: $(deps_scripts/kconfig/lexer.lex.o)

$(deps_scripts/kconfig/lexer.lex.o):
