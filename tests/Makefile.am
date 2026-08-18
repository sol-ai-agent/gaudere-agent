AM_CPPFLAGS = -I$(top_srcdir)/src $(GAUDERE_CFLAGS)

check_PROGRAMS = \
	state_lock_test \
	task_dispatcher_test \
	task_executor_test \
	task_report_test \
	work_controller_test
TESTS = smoke.sh $(check_PROGRAMS)
EXTRA_DIST = smoke.sh

state_lock_test_SOURCES = \
	state_lock_test.cpp \
	../src/StateLock.cpp \
	../src/StateLock.hpp
state_lock_test_LDADD = $(GAUDERE_LIBS)

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

task_report_test_SOURCES = \
	task_report_test.cpp \
	../src/TaskReport.cpp \
	../src/TaskReport.hpp
task_report_test_LDADD = $(GAUDERE_LIBS)

work_controller_test_SOURCES = \
	work_controller_test.cpp \
	../src/WorkController.cpp \
	../src/WorkController.hpp \
	../src/TaskDispatcher.cpp \
	../src/TaskDispatcher.hpp \
	../src/TaskExecutor.cpp \
	../src/TaskExecutor.hpp
work_controller_test_LDADD = $(GAUDERE_LIBS)
