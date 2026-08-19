AM_CPPFLAGS = $(GAUDERE_CFLAGS) $(JSON_CFLAGS) $(CURL_CFLAGS)

bin_PROGRAMS = gaudere-agent
gaudere_agent_SOURCES = \
	main.cpp \
	CurlHttpTransport.cpp \
	CurlHttpTransport.hpp \
	FileSecretSource.cpp \
	FileSecretSource.hpp \
	HttpTransport.hpp \
	LocalEchoHandler.cpp \
	LocalEchoHandler.hpp \
	LocalWaitHandler.cpp \
	LocalWaitHandler.hpp \
	OpenAIResponsesProvider.cpp \
	OpenAIResponsesProvider.hpp \
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
gaudere_agent_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS) $(CURL_LIBS)
