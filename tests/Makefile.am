AM_CPPFLAGS = -I$(top_srcdir)/src $(GAUDERE_CFLAGS)

check_PROGRAMS = task_executor_test
TESTS = smoke.sh $(check_PROGRAMS)
EXTRA_DIST = smoke.sh

task_executor_test_SOURCES = \
	task_executor_test.cpp \
	../src/TaskExecutor.cpp \
	../src/TaskExecutor.hpp
task_executor_test_LDADD = $(GAUDERE_LIBS)
