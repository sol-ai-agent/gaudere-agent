# Local Goose dialogue responder retry admission v1

## Status

This tranche admits at most one immutable generation-1 retry lineage after a previously bound responder authority attempt has been recovered to terminal state before dispatch.

It is deliberately **not** retry execution authority. Preparing a lineage does not create or activate a grant, create a lease, prepare a decision or intent, create a disposition, submit a Task, advance the preferred head or ACK the completion feed.

## Authoritative evidence

`LocalGooseDialogueResponderRetryAdmissionService` reads the existing authoritative stores and requires all of the following before it may append a retry lineage:

1. the responder authority sidecar is schema v3 and the dedicated retry sidecar is schema v1;
2. the requested consumer still has the same exact pending completion event and that event is human-authored;
3. the preferred thread head still equals the completion event revision and Task id;
4. the triggering Task still exists in the authoritative Task store;
5. the historical decision is `expired`, preserves its exact intent binding, and has terminal reason `bound_intent_expired_before_dispatch`;
6. the historical intent is `expired`, has no submission/commit/completion evidence, and has terminal reason `lease_or_intent_expired`;
7. the historical intervention disposition is `manual_review`, remains bound to the same intent, and has terminal reason `lease_or_intent_expired`;
8. the historical lease is `expired`, has terminal reason `lease_expired`, committed zero turns and is past its expiration timestamp;
9. the historical grant remains the immutable activated grant bound to that historical lease, has no invented terminal transition, and its authority time window is already expired;
10. no Task exists under the deterministic V3 submission idempotency key derived from the historical intent request id.

Any mismatch fails closed before the retry sidecar is written.

## Only mutation

After every read-side proof succeeds, the service constructs a canonical `cognition.local-goose-dialogue.responder-retry-lineage.v1:` record with generation exactly `1` and calls only:

`LocalGooseDialogueResponderRetryStore::prepare_lineage()`

The retry store is append-only and already enforces one request, one lineage per consumer/event and one lineage per consumer/completion sequence. Exact replay is idempotent. A different second attempt conflicts.

## Production boundary

This tranche is library/test only. It is not wired into `main.cpp`, the production `Containerfile`, LiveControl, the observer, dispatcher or service configuration.

No production retry lineage is created by merging this code. A later explicitly gated one-shot Fedora helper may bind this service to the real recovered sequence-4 evidence and create the single generation-1 lineage. Even that future lineage-preparation gate must still create no new grant, lease, decision, intent, disposition, Task or ACK.

Only a still later contract may consume an admitted lineage as evidence for a new immutable authority attempt. The historical schema-v3 authority chain remains immutable and must never be reopened, renewed or overwritten.
