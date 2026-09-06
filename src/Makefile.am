AM_CPPFLAGS = $(GAUDERE_CFLAGS) $(JSON_CFLAGS) $(CURL_CFLAGS)

bin_PROGRAMS = gaudere-agent gaudere-control gaudere-resume-after-wake gaudere-resume-after-wake-v1-prepare gaudere-resume-after-wake-v1 gaudere-current-cognition-prepare gaudere-current-cognition gaudere-autonomous-cognition-pulse gaudere-continuity-delta-checkpoint gaudere-local-activity-seed gaudere-local-activity-status

gaudere_agent_SOURCES = \
	main.cpp \
	AutonomousCognitionProviderGate.cpp \
	AutonomousCognitionProviderGate.hpp \
	AutonomousCognitionProviderService.cpp \
	AutonomousCognitionProviderService.hpp \
	AutonomousCognitionStaleRefresh.cpp \
	AutonomousCognitionStaleRefresh.hpp \
	AutonomousCognitionPulse.cpp \
	AutonomousCognitionPulse.hpp \
	AutonomousCognitionPulseService.cpp \
	AutonomousCognitionPulseService.hpp \
	AutonomousCognitionPulseSidecarInspection.cpp \
	AutonomousCognitionPulseStore.cpp \
	AutonomousCognitionPulseStore.hpp \
	BoundedReflection.cpp \
	BoundedReflection.hpp \
	CanonicalCognitionDecision.cpp \
	CanonicalCognitionDecision.hpp \
	ContinuityDeltaCheckpoint.cpp \
	ContinuityDeltaCheckpoint.hpp \
	ContinuityDeltaCheckpointInspection.cpp \
	ContinuityDeltaCheckpointInspection.hpp \
	CurlHttpTransport.cpp \
	CurlHttpTransport.hpp \
	CurrentCognitionCycle.cpp \
	CurrentCognitionCycle.hpp \
	CurrentCognitionHandler.cpp \
	CurrentCognitionHandler.hpp \
	CurrentCognitionTaskInspection.cpp \
	CurrentCognitionTaskInspection.hpp \
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
	LocalActivityPulse.cpp \
	LocalActivityPulse.hpp \
	LocalActivityPulseSchedulerBridge.cpp \
	LocalActivityPulseSchedulerBridge.hpp \
	LocalActivityPulseService.cpp \
	LocalActivityPulseService.hpp \
	LocalActivityPulseSidecarInspection.cpp \
	LocalActivityPulseStatus.cpp \
	LocalActivityPulseStatus.hpp \
	LocalActivityPulseStore.cpp \
	LocalActivityPulseStore.hpp \
	LocalContinuityObservation.cpp \
	LocalContinuityObservation.hpp \
	LocalContinuityObservationHandler.cpp \
	LocalContinuityObservationHandler.hpp \
	LocalEchoHandler.cpp \
	LocalEchoHandler.hpp \
	LocalWaitHandler.cpp \
	LocalWaitHandler.hpp \
	OpenAIActivation.cpp \
	OpenAIActivation.hpp \
	OpenAIBudget.hpp \
	OpenAIJsonSchemaTransport.cpp \
	OpenAIJsonSchemaTransport.hpp \
	OpenAIOneShot.cpp \
	OpenAIOneShot.hpp \
	OpenAIResponsesProvider.cpp \
	OpenAIResponsesProvider.hpp \
	OpenAIStructuredActivation.cpp \
	OpenAIStructuredActivation.hpp \
	OpenAITask.cpp \
	OpenAITask.hpp \
	Provider.hpp \
	ProviderTaskHandler.cpp \
	ProviderTaskHandler.hpp \
	ResumeAfterWakeCognition.cpp \
	ResumeAfterWakeCognition.hpp \
	ResumeContextSnapshot.cpp \
	ResumeContextSnapshotInspection.cpp \
	ResumeContextSnapshot.hpp \
	ResumeDecisionStructuredOutput.hpp \
	SecretSource.hpp \
	Sha256.cpp \
	Sha256.hpp \
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
gaudere_agent_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS) $(CURL_LIBS) -lsqlite3

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

gaudere_current_cognition_prepare_SOURCES = \
	current_cognition_prepare_main.cpp \
	CurrentCognitionCycle.cpp \
	CurrentCognitionCycle.hpp \
	ResumeContextSnapshot.cpp \
	ResumeContextSnapshotInspection.cpp \
	ResumeContextSnapshotRequest.cpp \
	ResumeContextSnapshot.hpp \
	StateLock.cpp \
	StateLock.hpp \
	TaskExecutor.cpp \
	TaskExecutor.hpp
gaudere_current_cognition_prepare_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS)

gaudere_current_cognition_SOURCES = \
	current_cognition_main.cpp \
	CurrentCognitionCycle.cpp \
	CurrentCognitionCycle.hpp \
	CurrentCognitionHandler.cpp \
	CurrentCognitionHandler.hpp \
	ResumeAfterWakeCognition.cpp \
	ResumeAfterWakeCognition.hpp \
	ResumeContextSnapshotInspection.cpp \
	ResumeContextSnapshot.hpp \
	ResumeDecisionStructuredOutput.hpp \
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
gaudere_current_cognition_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS) $(CURL_LIBS)

gaudere_autonomous_cognition_pulse_SOURCES = \
	autonomous_cognition_pulse_main.cpp \
	AutonomousCognitionPulse.cpp \
	AutonomousCognitionPulse.hpp \
	AutonomousCognitionPulseStore.cpp \
	AutonomousCognitionPulseStore.hpp \
	CanonicalCognitionDecision.cpp \
	CanonicalCognitionDecision.hpp \
	CurrentCognitionCycle.cpp \
	CurrentCognitionCycle.hpp \
	OpenAIBudget.hpp \
	ResumeContextSnapshot.cpp \
	ResumeContextSnapshotInspection.cpp \
	ResumeContextSnapshot.hpp \
	Sha256.cpp \
	Sha256.hpp \
	StateLock.cpp \
	StateLock.hpp \
	TaskExecutor.cpp \
	TaskExecutor.hpp \
	WakeSourceDecision.hpp
gaudere_autonomous_cognition_pulse_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS) -lsqlite3

gaudere_continuity_delta_checkpoint_SOURCES = \
	continuity_delta_checkpoint_main.cpp \
	ContinuityDeltaCheckpoint.cpp \
	ContinuityDeltaCheckpoint.hpp \
	CanonicalCognitionDecision.cpp \
	CanonicalCognitionDecision.hpp \
	CurrentCognitionCycle.cpp \
	CurrentCognitionCycle.hpp \
	CurrentCognitionTaskInspection.cpp \
	CurrentCognitionTaskInspection.hpp \
	ResumeAfterWake.hpp \
	ResumeAfterWakeCognition.cpp \
	ResumeAfterWakeCognition.hpp \
	ResumeContextSnapshot.cpp \
	ResumeContextSnapshotInspection.cpp \
	ResumeContextSnapshot.hpp \
	Sha256.cpp \
	Sha256.hpp \
	StateLock.cpp \
	StateLock.hpp \
	TaskExecutor.cpp \
	TaskExecutor.hpp \
	WakeSourceDecision.hpp
gaudere_continuity_delta_checkpoint_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS)

gaudere_local_activity_seed_SOURCES = \
	local_activity_seed_main.cpp \
	CanonicalCognitionDecision.cpp \
	CanonicalCognitionDecision.hpp \
	ContinuityDeltaCheckpoint.cpp \
	ContinuityDeltaCheckpoint.hpp \
	ContinuityDeltaCheckpointInspection.cpp \
	ContinuityDeltaCheckpointInspection.hpp \
	LocalActivityPulse.cpp \
	LocalActivityPulse.hpp \
	LocalActivityPulseStore.cpp \
	LocalActivityPulseStore.hpp \
	LocalContinuityObservation.cpp \
	LocalContinuityObservation.hpp \
	LocalContinuityObservationHandler.cpp \
	LocalContinuityObservationHandler.hpp \
	OpenAIBudget.hpp \
	Sha256.cpp \
	Sha256.hpp \
	StateLock.cpp \
	StateLock.hpp \
	TaskExecutor.cpp \
	TaskExecutor.hpp
gaudere_local_activity_seed_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS) -lsqlite3

gaudere_local_activity_status_SOURCES = \
	local_activity_status_main.cpp \
	LocalActivityPulseSchedulerBridge.cpp \
	LocalActivityPulseSchedulerBridge.hpp \
	LocalActivityPulseSidecarInspection.cpp \
	LocalActivityPulseStatus.cpp \
	LocalActivityPulseStatus.hpp \
	LocalActivityPulseStore.cpp \
	LocalActivityPulseStore.hpp \
	LocalContinuityObservation.cpp \
	LocalContinuityObservation.hpp \
	Sha256.cpp \
	Sha256.hpp
gaudere_local_activity_status_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS) -lsqlite3
