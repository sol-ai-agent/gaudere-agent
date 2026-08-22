AM_CPPFLAGS = $(GAUDERE_CFLAGS) $(JSON_CFLAGS) $(CURL_CFLAGS)

bin_PROGRAMS = gaudere-agent gaudere-control

gaudere_agent_SOURCES = \
	main.cpp \
	BoundedReflection.cpp \
	BoundedReflection.hpp \
	CurlHttpTransport.cpp \
	CurlHttpTransport.hpp \
	ExplicitWake.cpp \
	ExplicitWake.hpp \
	FileSecretSource.cpp \
	FileSecretSource.hpp \
	HttpTransport.hpp \
	LiveControl.cpp \
	LiveControl.hpp \
	LiveControlProcessor.cpp \
	LiveControlProcessor.hpp \
	LocalEchoHandler.cpp \
	LocalEchoHandler.hpp \
	LocalWaitHandler.cpp \
	LocalWaitHandler.hpp \
	OpenAIActivation.cpp \
	OpenAIActivation.hpp \
	OpenAIBudget.hpp \
	OpenAIOneShot.cpp \
	OpenAIOneShot.hpp \
	OpenAIResponsesProvider.cpp \
	OpenAIResponsesProvider.hpp \
	OpenAITask.cpp \
	OpenAITask.hpp \
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

gaudere_control_SOURCES = \
	control_main.cpp \
	LiveControl.cpp \
	LiveControl.hpp
gaudere_control_LDADD = $(JSON_LIBS)
