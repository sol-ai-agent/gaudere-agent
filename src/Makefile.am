AM_CPPFLAGS = $(GAUDERE_CFLAGS) $(JSON_CFLAGS) $(CURL_CFLAGS)

bin_PROGRAMS = gaudere-agent gaudere-control gaudere-resume-after-wake gaudere-resume-after-wake-v1-prepare gaudere-resume-after-wake-v1

gaudere_agent_SOURCES = \
	main.cpp \
	BoundedReflection.cpp \
	BoundedReflection.hpp \
	CurlHttpTransport.cpp \
	CurlHttpTransport.hpp \
	ExplicitWake.cpp \
	ExplicitWake.hpp \
	WakeSourceDecision.cpp \
	WakeSourceDecision.hpp \
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

gaudere_resume_after_wake_SOURCES = \
	resume_after_wake_main.cpp \
	ResumeAfterWake.cpp \
	ResumeAfterWake.hpp \
	ResumeAfterWakeCognition.cpp \
	ResumeAfterWakeCognition.hpp \
	WakeSourceDecision.cpp \
	WakeSourceDecision.hpp \
	CurlHttpTransport.cpp \
	CurlHttpTransport.hpp \
	FileSecretSource.cpp \
	FileSecretSource.hpp \
	HttpTransport.hpp \
	OpenAIActivation.cpp \
	OpenAIActivation.hpp \
	OpenAIBudget.hpp \
	OpenAIResponsesProvider.cpp \
	OpenAIResponsesProvider.hpp \
	Provider.hpp \
	ProviderTaskHandler.cpp \
	ProviderTaskHandler.hpp \
	SecretSource.hpp \
	StateLock.cpp \
	StateLock.hpp \
	TaskExecutor.cpp \
	TaskExecutor.hpp
gaudere_resume_after_wake_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS) $(CURL_LIBS)

gaudere_resume_after_wake_v1_prepare_SOURCES = \
	resume_after_wake_v1_prepare_main.cpp \
	ResumeAfterWakeV1Prepare.cpp \
	ResumeAfterWakeV1Prepare.hpp \
	ResumeAfterWakeV1.cpp \
	ResumeAfterWakeV1.hpp \
	ResumeContextSnapshot.cpp \
	ResumeContextSnapshotInspection.cpp \
	ResumeContextSnapshotRequest.cpp \
	ResumeContextSnapshot.hpp \
	WakeSourceDecision.cpp \
	WakeSourceDecision.hpp \
	StateLock.cpp \
	StateLock.hpp \
	TaskExecutor.cpp \
	TaskExecutor.hpp
gaudere_resume_after_wake_v1_prepare_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS)

gaudere_resume_after_wake_v1_SOURCES = \
	resume_after_wake_v1_main.cpp \
	ResumeAfterWakeV1.hpp \
	ResumeAfterWakeCognition.cpp \
	ResumeAfterWakeCognition.hpp \
	ResumeAfterWakeV1Cognition.cpp \
	ResumeAfterWakeV1Cognition.hpp \
	ResumeAfterWakeV1StructuredInputAdapter.cpp \
	ResumeAfterWakeV1StructuredInputAdapter.hpp \
	ResumeContextSnapshot.cpp \
	ResumeContextSnapshotInspection.cpp \
	ResumeContextSnapshot.hpp \
	ResumeDecisionStructuredOutput.hpp \
	WakeSourceDecision.hpp \
	CurlHttpTransport.cpp \
	CurlHttpTransport.hpp \
	FileSecretSource.cpp \
	FileSecretSource.hpp \
	HttpTransport.hpp \
	OpenAIBudget.hpp \
	OpenAIJsonSchemaTransport.cpp \
	OpenAIJsonSchemaTransport.hpp \
	OpenAIResponsesProvider.cpp \
	OpenAIResponsesProvider.hpp \
	OpenAIStructuredActivation.cpp \
	OpenAIStructuredActivation.hpp \
	Provider.hpp \
	ProviderTaskHandler.cpp \
	ProviderTaskHandler.hpp \
	SecretSource.hpp \
	StateLock.cpp \
	StateLock.hpp \
	TaskExecutor.cpp \
	TaskExecutor.hpp
gaudere_resume_after_wake_v1_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS) $(CURL_LIBS)
