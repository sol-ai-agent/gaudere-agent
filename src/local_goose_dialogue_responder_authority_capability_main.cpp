#include "LocalGooseDialogueResponder.hpp"
#include "LocalGooseDialogueResponderAuthority.hpp"
#include "LocalGooseDialogueResponderAuthorityService.hpp"

#include <iostream>
#include <string>

int main(int argc, char* argv[])
{
    if (argc != 2 || std::string{argv[1]} != "--check") {
        std::cerr
            << "usage: gaudere-local-goose-dialogue-responder-authority-capability --check\n";
        return 2;
    }

    const gaudere_agent::LocalGooseDialogueResponderAuthorityServiceConfig config;
    if (config.authority_enabled) {
        std::cerr << "responder decision authority default unexpectedly enabled\n";
        return 1;
    }
    if (gaudere_agent::local_goose_dialogue_responder_owned_sidecar_schema != 2
        || gaudere_agent::local_goose_dialogue_responder_sidecar_schema != 3
        || gaudere_agent::local_goose_dialogue_responder_authority_sidecar_schema != 3) {
        std::cerr << "responder decision authority schema contract differs\n";
        return 1;
    }

    // This executable deliberately has no state/sidecar argument and constructs
    // no store, dispatcher or authority service. The production image therefore
    // carries the compiled authority implementation without exposing a mutation
    // or activation path. A later production wiring gate remains mandatory.
    std::cout << "AUTHORITY_DEFAULT=disabled\n";
    std::cout << "RESPONDER_OWNED_SCHEMA=2\n";
    std::cout << "RESPONDER_READABLE_SCHEMA=3\n";
    std::cout << "AUTHORITY_SCHEMA=3\n";
    std::cout << "STATE_ACCESS=absent\n";
    std::cout << "MUTATION_SURFACE=absent\n";
    std::cout << "GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_CAPABILITY=PASS\n";
    return 0;
}
