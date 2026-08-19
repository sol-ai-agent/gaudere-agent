AM_CPPFLAGS = $(GAUDERE_CFLAGS)

bin_PROGRAMS = gaudere-agent
gaudere_agent_SOURCES = \
	main.cpp \
	FileSecretSource.cpp \
	FileSecretSource.hpp \
	LocalEchoHandler.cpp \
	LocalEchoHandler.hpp \
	LocalWaitHandler.cpp \
	LocalWaitHandler.hpp \
	Provider.hpp \
	ProviderTaskHandler.cpp \
	ProviderTaskHandler.hpp \
	SecretSource.hpp \
	StateLock.cpp \
	StateLock.hpp \
	TaskDispatcher.cpp \
	TaskDispatcher.hpp \
	TaskExecutor.cpp \
	TaskExecutor.hpp \
	TaskReport.cpp \
	TaskReport.hpp \
	WorkController.cpp \
	WorkController.hpp
gaudere_agent_LDADD = $(GAUDERE_LIBS)
