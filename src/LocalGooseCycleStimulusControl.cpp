#include "LocalGooseCycleStimulusControl.hpp"

#include <sstream>
#include <stdexcept>
#include <utility>

namespace gaudere_agent {
namespace {

const char* result_name(
    const LocalGooseCycleStimulusServiceResult result) noexcept
{
    using Result = LocalGooseCycleStimulusServiceResult;
    switch (result) {
    case Result::accepted: return "accepted";
    case Result::duplicate: return "duplicate";
    case Result::consumed: return "consumed";
    case Result::superseded: return "superseded";
    case Result::manual_review: return "manual_review";
    case Result::conflict: return "conflict";
    case Result::invalid: return "invalid";
    case Result::unavailable: return "unavailable";
    }
    return "unknown";
}

const char* status_name(
    const LocalGooseCycleStimulusStatus status) noexcept
{
    using Status = LocalGooseCycleStimulusStatus;
    switch (status) {
    case Status::accepted: return "accepted";
    case Status::consumed: return "consumed";
    case Status::superseded: return "superseded";
    case Status::manual_review: return "manual_review";
    }
    return "unknown";
}

const char* cycle_state_name(
    const LocalGooseCycleState state) noexcept
{
    using State = LocalGooseCycleState;
    switch (state) {
    case State::dormant: return "dormant";
    case State::scheduled: return "scheduled";
    case State::prepared: return "prepared";
    case State::blocked: return "blocked";
    }
    return "unknown";
}

std::string report(const LocalGooseCycleStimulusServiceStep& step)
{
    std::ostringstream output;
    output << "result=" << result_name(step.result) << '\n';
    if (step.stimulus) {
        output << "stimulus_id=\"" << step.stimulus->id << "\"\n"
               << "source_id=\"" << step.stimulus->source_id << "\"\n"
               << "stimulus_status="
               << status_name(step.stimulus->status) << '\n'
               << "target_cycle_revision="
               << step.stimulus->target_cycle_revision << '\n'
               << "target_cycle_generation="
               << step.stimulus->target_cycle_generation << '\n';
        if (step.stimulus->resulting_cycle_revision) {
            output << "resulting_cycle_revision="
                   << *step.stimulus->resulting_cycle_revision << '\n';
        } else {
            output << "resulting_cycle_revision=none\n";
        }
    }
    if (step.cursor) {
        output << "cycle_revision=" << step.cursor->revision << '\n'
               << "cycle_generation=" << step.cursor->generation << '\n'
               << "cycle_state=" << cycle_state_name(step.cursor->state) << '\n';
        if (step.cursor->due_at_ms) {
            output << "cycle_due_at_ms=" << *step.cursor->due_at_ms << '\n';
        } else {
            output << "cycle_due_at_ms=none\n";
        }
    }
    if (!step.detail.empty()) output << "detail=" << step.detail << '\n';
    return output.str();
}

} // namespace

LocalGooseCycleStimulusControl::LocalGooseCycleStimulusControl(
    LocalGooseCycleStimulusService& service,
    NowMs now_ms)
    : service_(service), now_ms_(std::move(now_ms))
{
    if (!now_ms_) {
        throw std::invalid_argument(
            "Local Goose stimulus control clock is required");
    }
}

LiveControlReply LocalGooseCycleStimulusControl::stimulate(
    const std::string& request_id)
{
    const auto observed_at_ms = now_ms_();
    if (observed_at_ms < 0) {
        return {false, 4,
                "gaudere-agent: Local Goose stimulus clock is negative\n"};
    }

    const auto acceptance =
        service_.accept_explicit_recheck(request_id, observed_at_ms);
    if (acceptance.result
            != LocalGooseCycleStimulusServiceResult::accepted
        && acceptance.result
            != LocalGooseCycleStimulusServiceResult::duplicate) {
        return {
            false, 4,
            "gaudere-agent: Local Goose stimulus acceptance="
                + std::string{result_name(acceptance.result)} + "\n"
                + report(acceptance)};
    }
    if (!acceptance.stimulus) {
        throw std::runtime_error(
            "accepted Local Goose stimulus lacks durable record");
    }

    const auto reconciled = service_.reconcile(
        acceptance.stimulus->id, observed_at_ms);
    const std::string body =
        "acceptance=" + std::string{result_name(acceptance.result)} + "\n"
        + report(reconciled);
    return {
        reconciled.result
            == LocalGooseCycleStimulusServiceResult::consumed,
        reconciled.result
            == LocalGooseCycleStimulusServiceResult::consumed ? 0 : 4,
        body};
}

} // namespace gaudere_agent
