AM_CPPFLAGS = -I$(top_srcdir)/src $(GAUDERE_CFLAGS) $(JSON_CFLAGS) $(CURL_CFLAGS)

check_PROGRAMS = \
	bounded_reflection_test \
	curl_http_transport_test \
	explicit_wake_integration_test \
	explicit_wake_test \
	file_secret_source_test \
	live_control_test \
	live_control_processor_test \
	local_wait_handler_test \
	openai_activation_test \
	openai_one_shot_test \
	openai_responses_hardening_test \
	openai_responses_provider_test \
	provider_task_handler_test \
	provider_usage_durability_test \
	state_lock_test \
	task_dispatcher_test \
	task_executor_test \
	task_report_test \
	work_controller_test
TESTS = smoke.sh image_provenance_validator_test.sh $(check_PROGRAMS)
EXTRA_DIST = \
	first_production_reflection_validator_test.sh \
	image_provenance_validator_test.sh \
	schema_v4_migration_copy_validator_test.sh \
	smoke.sh

bounded_reflection_test_SOURCES = \
	bounded_reflection_test.cpp \
	../src/BoundedReflection.cpp \
	../src/BoundedReflection.hpp \
	../src/TaskExecutor.cpp \
	../src/TaskExecutor.hpp
bounded_reflection_test_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS)

curl_http_transport_test_SOURCES = \
	curl_http_transport_test.cpp \
	../src/CurlHttpTransport.cpp \
	../src/CurlHttpTransport.hpp \
	../src/HttpTransport.hpp
curl_http_transport_test_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS) $(CURL_LIBS)

explicit_wake_test_SOURCES = \
	explicit_wake_test.cpp \
	../src/ExplicitWake.cpp \
	../src/ExplicitWake.hpp
explicit_wake_test_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS)

explicit_wake_integration_test_SOURCES = \
	explicit_wake_integration_test.cpp \
	../src/BoundedReflection.cpp \
	../src/BoundedReflection.hpp \
	../src/ExplicitWake.cpp \
	../src/ExplicitWake.hpp \
	../src/LiveControl.cpp \
	../src/LiveControl.hpp \
	../src/LiveControlProcessor.cpp \
	../src/LiveControlProcessor.hpp \
	../src/OpenAITask.cpp \
	../src/OpenAITask.hpp \
	../src/TaskDispatcher.cpp \
	../src/TaskDispatcher.hpp \
	../src/TaskExecutor.cpp \
	../src/TaskExecutor.hpp \
	../src/TaskReport.cpp \
	../src/TaskReport.hpp \
	../src/WorkController.cpp \
	../src/WorkController.hpp
explicit_wake_integration_test_LDADD = \
	$(GAUDERE_LIBS) $(JSON_LIBS) -lsqlite3

file_secret_source_test_SOURCES = \
	file_secret_source_test.cpp \
	../src/FileSecretSource.cpp \
	../src/FileSecretSource.hpp \
	../src/SecretSource.hpp
file_secret_source_test_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS)

live_control_test_SOURCES = \
	live_control_test.cpp \
	../src/LiveControl.cpp \
	../src/LiveControl.hpp
live_control_test_LDADD = $(JSON_LIBS)

live_control_processor_test_SOURCES = \
	live_control_processor_test.cpp \
	../src/BoundedReflection.cpp \
	../src/BoundedReflection.hpp \
	../src/ExplicitWake.cpp \
	../src/ExplicitWake.hpp \
	../src/LiveControl.cpp \
	../src/LiveControl.hpp \
	../src/LiveControlProcessor.cpp \
	../src/LiveControlProcessor.hpp \
	../src/OpenAITask.cpp \
	../src/OpenAITask.hpp \
	../src/TaskReport.cpp \
	../src/TaskReport.hpp
live_control_processor_test_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS)

local_wait_handler_test_SOURCES = \
	local_wait_handler_test.cpp \
	../src/LocalWaitHandler.cpp \
	../src/LocalWaitHandler.hpp
local_wait_handler_test_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS)

openai_activation_test_SOURCES = \
	openai_activation_test.cpp \
	../src/CurlHttpTransport.cpp \
	../src/CurlHttpTransport.hpp \
	../src/FileSecretSource.cpp \
	../src/FileSecretSource.hpp \
	../src/HttpTransport.hpp \
	../src/OpenAIActivation.cpp \
	../src/OpenAIActivation.hpp \
	../src/OpenAIResponsesProvider.cpp \
	../src/OpenAIResponsesProvider.hpp \
	../src/Provider.hpp \
	../src/ProviderTaskHandler.cpp \
	../src/ProviderTaskHandler.hpp \
	../src/SecretSource.hpp \
	../src/TaskExecutor.hpp
openai_activation_test_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS) $(CURL_LIBS)

openai_one_shot_test_SOURCES = \
	openai_one_shot_test.cpp \
	../src/OpenAIOneShot.cpp \
	../src/OpenAIOneShot.hpp \
	../src/OpenAITask.cpp \
	../src/OpenAITask.hpp \
	../src/TaskDispatcher.cpp \
	../src/TaskDispatcher.hpp \
	../src/TaskExecutor.cpp \
	../src/TaskExecutor.hpp \
	../src/TaskReport.cpp \
	../src/TaskReport.hpp \
	../src/WorkController.cpp \
	../src/WorkController.hpp
openai_one_shot_test_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS)

openai_responses_hardening_test_SOURCES = \
	openai_responses_hardening_test.cpp \
	../src/HttpTransport.hpp \
	../src/OpenAIResponsesProvider.cpp \
	../src/OpenAIResponsesProvider.hpp \
	../src/Provider.hpp \
	../src/SecretSource.hpp
openai_responses_hardening_test_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS)

openai_responses_provider_test_SOURCES = \
	openai_responses_provider_test.cpp \
	../src/HttpTransport.hpp \
	../src/OpenAIResponsesProvider.cpp \
	../src/OpenAIResponsesProvider.hpp \
	../src/Provider.hpp \
	../src/SecretSource.hpp
openai_responses_provider_test_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS)

provider_task_handler_test_SOURCES = \
	provider_task_handler_test.cpp \
	../src/Provider.hpp \
	../src/ProviderTaskHandler.cpp \
	../src/ProviderTaskHandler.hpp \
	../src/TaskExecutor.cpp \
	../src/TaskExecutor.hpp
provider_task_handler_test_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS)

provider_usage_durability_test_SOURCES = \
	provider_usage_durability_test.cpp \
	../src/Provider.hpp \
	../src/ProviderTaskHandler.cpp \
	../src/ProviderTaskHandler.hpp \
	../src/TaskExecutor.cpp \
	../src/TaskExecutor.hpp
provider_usage_durability_test_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS)

state_lock_test_SOURCES = \
	state_lock_test.cpp \
	../src/StateLock.cpp \
	../src/StateLock.hpp
state_lock_test_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS)

task_executor_test_SOURCES = \
	task_executor_test.cpp \
	../src/TaskExecutor.cpp \
	../src/TaskExecutor.hpp
task_executor_test_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS)

task_dispatcher_test_SOURCES = \
	task_dispatcher_test.cpp \
	../src/TaskDispatcher.cpp \
	../src/TaskDispatcher.hpp \
	../src/TaskExecutor.cpp \
	../src/TaskExecutor.hpp
task_dispatcher_test_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS)

task_report_test_SOURCES = \
	task_report_test.cpp \
	../src/TaskReport.cpp \
	../src/TaskReport.hpp
task_report_test_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS)

work_controller_test_SOURCES = \
	work_controller_test.cpp \
	../src/WorkController.cpp \
	../src/WorkController.hpp \
	../src/TaskDispatcher.cpp \
	../src/TaskDispatcher.hpp \
	../src/TaskExecutor.cpp \
	../src/TaskExecutor.hpp
work_controller_test_LDADD = $(GAUDERE_LIBS) $(JSON_LIBS)
