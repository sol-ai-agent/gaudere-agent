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

gaudere_agent_SOURCES += \
	LocalGooseCognition.cpp \
	LocalGooseCognition.hpp \
	LocalGooseCognitionHandler.cpp \
	LocalGooseCognitionHandler.hpp \
	LocalGooseDialogue.cpp \
	LocalGooseDialogue.hpp \
	LocalGooseDialogueHandler.cpp \
	LocalGooseDialogueHandler.hpp \
	LocalGooseCognitionService.cpp \
	LocalGooseCognitionService.hpp \
	LocalGooseCycle.cpp \
	LocalGooseCycle.hpp \
	LocalGooseCycleHandler.cpp \
	LocalGooseCycleHandler.hpp \
	LocalGooseCycleSchedulerBridge.cpp \
	LocalGooseCycleSchedulerBridge.hpp \
	LocalGooseCycleService.cpp \
	LocalGooseCycleService.hpp \
	LocalGooseCycleSidecarInspection.cpp \
	LocalGooseCycleStore.cpp \
	LocalGooseCycleStore.hpp \
	LocalGooseCycleStimulusControl.cpp \
	LocalGooseCycleStimulusControl.hpp \
	LocalGooseCycleStimulusService.cpp \
	LocalGooseCycleStimulusService.hpp \
	LocalGooseCycleStimulusStore.cpp \
	LocalGooseCycleStimulusStore.hpp \
	LocalGooseRunner.cpp \
	LocalGooseRunner.hpp

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

bin_PROGRAMS += gaudere-goose-tools-mcp

gaudere_goose_tools_mcp_SOURCES = \
	goose_tools_mcp_main.cpp \
	GooseGovernanceStore.cpp \
	GooseGovernanceStore.hpp \
	GooseToolsMcp.cpp \
	GooseToolsMcp.hpp \
	LiveControl.cpp \
	LiveControl.hpp \
	Sha256.cpp \
	Sha256.hpp

gaudere_goose_tools_mcp_LDADD = $(JSON_LIBS) -lsqlite3
gaudere_goose_tools_mcp_LDFLAGS = -pthread

bin_PROGRAMS += gaudere-local-goose-cycle-runtime

gaudere_local_goose_cycle_runtime_SOURCES = \
	local_goose_cycle_runtime_main.cpp \
	BoundedReflection.cpp \
	BoundedReflection.hpp \
	ExplicitWake.cpp \
	ExplicitWake.hpp \
	LiveControl.cpp \
	LiveControl.hpp \
	LiveControlProcessor.cpp \
	LiveControlProcessor.hpp \
	LocalContinuityObservation.cpp \
	LocalContinuityObservation.hpp \
	LocalEchoHandler.cpp \
	LocalEchoHandler.hpp \
	LocalGooseCognition.cpp \
	LocalGooseCognition.hpp \
	LocalGooseCycle.cpp \
	LocalGooseCycle.hpp \
	LocalGooseCycleHandler.cpp \
	LocalGooseCycleHandler.hpp \
	LocalGooseCycleSchedulerBridge.cpp \
	LocalGooseCycleSchedulerBridge.hpp \
	LocalGooseCycleService.cpp \
	LocalGooseCycleService.hpp \
	LocalGooseCycleSidecarInspection.cpp \
	LocalGooseCycleStore.cpp \
	LocalGooseCycleStore.hpp \
	LocalGooseCycleStimulusControl.cpp \
	LocalGooseCycleStimulusControl.hpp \
	LocalGooseCycleStimulusService.cpp \
	LocalGooseCycleStimulusService.hpp \
	LocalGooseCycleStimulusStore.cpp \
	LocalGooseCycleStimulusStore.hpp \
	LocalGooseRunner.cpp \
	LocalGooseRunner.hpp \
	OpenAIBudget.hpp \
	OpenAITask.cpp \
	OpenAITask.hpp \
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
	WakeSourceDecision.cpp \
	WakeSourceDecision.hpp \
	WorkController.cpp \
	WorkController.hpp

gaudere_local_goose_cycle_runtime_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS) -lsqlite3
gaudere_local_goose_cycle_runtime_LDFLAGS = -pthread

bin_PROGRAMS += gaudere-local-goose-cycle-stimulus-proof

gaudere_local_goose_cycle_stimulus_proof_SOURCES = \
	local_goose_cycle_stimulus_proof_main.cpp \
	LiveControl.cpp \
	LiveControl.hpp \
	LocalContinuityObservation.cpp \
	LocalContinuityObservation.hpp \
	LocalGooseCognition.cpp \
	LocalGooseCognition.hpp \
	LocalGooseCycle.cpp \
	LocalGooseCycle.hpp \
	LocalGooseCycleStimulusControl.cpp \
	LocalGooseCycleStimulusControl.hpp \
	LocalGooseCycleStimulusService.cpp \
	LocalGooseCycleStimulusService.hpp \
	LocalGooseCycleStimulusStore.cpp \
	LocalGooseCycleStimulusStore.hpp \
	LocalGooseCycleStore.cpp \
	LocalGooseCycleStore.hpp \
	Sha256.cpp \
	Sha256.hpp

gaudere_local_goose_cycle_stimulus_proof_LDADD = \
	$(GAUDERE_LIBS) $(JSON_LIBS) -lsqlite3

gaudere_local_goose_cycle_stimulus_proof_LDFLAGS = -pthread

bin_PROGRAMS += gaudere-local-goose-cycle-stimulus-init

gaudere_local_goose_cycle_stimulus_init_SOURCES = \
	local_goose_cycle_stimulus_init_main.cpp \
	LocalGooseCycleStimulusStore.cpp \
	LocalGooseCycleStimulusStore.hpp \
	Sha256.cpp \
	Sha256.hpp

gaudere_local_goose_cycle_stimulus_init_LDADD = \
	$(JSON_LIBS) -lsqlite3

bin_PROGRAMS += gaudere-local-goose-cycle-seed gaudere-local-goose-cycle-activate

gaudere_local_goose_cycle_seed_SOURCES = \
	local_goose_cycle_seed_main.cpp \
	LocalActivityPulseStore.cpp \
	LocalActivityPulseStore.hpp \
	LocalActivityPulseSidecarInspection.cpp \
	LocalContinuityObservation.cpp \
	LocalContinuityObservation.hpp \
	LocalGooseCycleBootstrap.cpp \
	LocalGooseCycleBootstrap.hpp \
	LocalGooseCycleStore.cpp \
	LocalGooseCycleStore.hpp \
	Sha256.cpp \
	Sha256.hpp \
	StateLock.cpp \
	StateLock.hpp

gaudere_local_goose_cycle_seed_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS) -lsqlite3

gaudere_local_goose_cycle_activate_SOURCES = \
	local_goose_cycle_activate_main.cpp \
	LocalActivityPulseStore.cpp \
	LocalActivityPulseStore.hpp \
	LocalContinuityObservation.cpp \
	LocalContinuityObservation.hpp \
	LocalGooseCycleBootstrap.cpp \
	LocalGooseCycleBootstrap.hpp \
	LocalGooseCycleStore.cpp \
	LocalGooseCycleStore.hpp \
	Sha256.cpp \
	Sha256.hpp \
	StateLock.cpp \
	StateLock.hpp

gaudere_local_goose_cycle_activate_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS) -lsqlite3
