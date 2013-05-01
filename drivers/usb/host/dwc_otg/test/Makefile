
PERL=/usr/bin/perl
PL_TESTS=test_sysfs.pl test_mod_param.pl

.PHONY : test
test : perl_tests

perl_tests :
	@echo
	@echo Running perl tests
	@for test in $(PL_TESTS); do \
	  if $(PERL) ./$$test ; then \
	    echo "=======> $$test, PASSED" ; \
	  else echo "=======> $$test, FAILED" ; \
	  fi \
	done
