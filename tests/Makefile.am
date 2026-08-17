AM_CPPFLAGS = -I$(top_srcdir)/src $(GAUDERE_CFLAGS)

check_PROGRAMS = task_executor_test task_dispatcher_test
TESTS = smoke.sh $(check_PROGRAMS)
EXTRA_DIST = smoke.sh

task_executor_test_SOURCES = \
	task_executor_test.cpp \
	../src/TaskExecutor.cpp \
	../src/TaskExecutor.hpp
task_executor_test_LDADD = $(GAUDERE_LIBS)

task_dispatcher_test_SOURCES = \
	task_dispatcher_test.cpp \
	../src/TaskDispatcher.cpp \
	../src/TaskDispatcher.hpp \
	../src/TaskExecutor.cpp \
	../src/TaskExecutor.hpp
task_dispatcher_test_LDADD = $(GAUDERE_LIBS)
