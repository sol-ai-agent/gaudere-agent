#ifndef GAUDERE_AGENT_RESUME_DECISION_STRUCTURED_OUTPUT_HPP
#define GAUDERE_AGENT_RESUME_DECISION_STRUCTURED_OUTPUT_HPP

#include "OpenAIJsonSchemaTransport.hpp"

namespace gaudere_agent {

/** API-level shape guarantee for bounded resume cognition.
 *
 * Structured Outputs requires every property to be required for reliable strict
 * schemas; objective is therefore nullable. The durable cognition normalizer is
 * still the semantic authority and removes null for stop / requires text for
 * continue.
 */
inline OpenAIJsonSchemaContract resume_decision_structured_output_contract()
{
    return OpenAIJsonSchemaContract{
        "gaudere_resume_decision_v1",
        R"({"type":"object","properties":{"schema":{"type":"string","enum":["gaudere.cognition.resume-decision.v1"]},"decision":{"type":"string","enum":["stop","continue"]},"reason":{"type":"string"},"objective":{"type":["string","null"]}},"required":["schema","decision","reason","objective"],"additionalProperties":false})"
    };
}

} // namespace gaudere_agent

#endif
