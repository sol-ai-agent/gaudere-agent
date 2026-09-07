# Keep the historical build manifest intact and layer the local Goose slice on top.
include Makefile.base.am

gaudere_agent_SOURCES += \
	LocalGooseCognition.cpp \
	LocalGooseCognition.hpp \
	LocalGooseCognitionHandler.cpp \
	LocalGooseCognitionHandler.hpp \
	LocalGooseCognitionService.cpp \
	LocalGooseCognitionService.hpp \
	LocalGooseRunner.cpp \
	LocalGooseRunner.hpp

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

gaudere_goose_tools_mcp_LDADD = $(JSON_LIBS) -lsqlite3 -pthread
