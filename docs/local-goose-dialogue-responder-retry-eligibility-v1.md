# Retry generation 1 — non-mutating eligibility inspection

Status: **code-only / no production wiring**. Depends on the production-audit admission proof for PR #269 (operator-reported `FEDORA_GAUDERE_RESPONDER_RETRY_ADMISSION_PRODUCTION=PASS` on 2026-11-06). This tranche creates no operational authority.

The new `LocalGooseDialogueResponderRetryEligibilityService` has one status call: `inspect_generation_1(expected_lineage_id, now_ms)`. It uses **const references** to the already-open authoritative completion, preferred-head, historical responder/authority, Task and retry lineage stores. The status explicitly means **audit evidence consistent, no authority**; it is not a grant, lease, decision, dispatch permission, ACK, nor provider enablement.

For the first bounded sequence-4 checkpoint the inspector requires:

- exact immutable generation-1 lineage identity supplied by the caller (in production the known fixed identity is `cognition.local-goose-dialogue.responder-retry-lineage.v1:da6bb42704949f0003e670c06e7ed04078c238f08356f1853f9b3776455f2bca`);
- known responder `system-responder-v1`, pending human sequence 4, cursor 3, thread `main` revision 3 and exact source Task, no consumed ACK;
- lineage indexed consistently by its request and event, valid schema v1, authority schema v3;
- immutable historical grant/lease/decision/intent/disposition still bound to each other and to the exact lineage and completion evidence;
- historical grant activated only for the expired original lease, lease expired with zero committed turns, decision and intent expired and disposition in manual review with exact terminal reasons;
- absence of a historical submission Task under the original intent's idempotency key.

Missing audit lineage returns `lineage_absent` (it is not created). Stale or inconsistent evidence returns `conflict`; invalid clock/schema or read failure returns `unavailable`. The only positive status is `evidence_consistent_no_authority`.

**Important constructor limitation:** the existing `LocalGooseDialogueResponderRetryStore` constructor can create an absent sidecar. This inspector is code-only and accepts *already-open* store handles. A future operational status command must first add and prove a separately audited read-only open path (including absence detection without file creation). Do not use a write-capable store constructor in a production read-only command. No such command or runtime wiring is introduced here.

The shared fake fixture is extracted from the previous admission test without changing its assertions. The new adversarial tests cover absence, consistent/no-mutation replay, mismatch, invalid clock, stale head, acknowledged trigger, durable submission Task, missing source Task and unexpired authority window. A distinct CI gate compiles the test on Fedora 44 with `-Werror` and statically forbids any mutable store operations and runtime wiring.

**Next, separately gated:** read-only file-opening semantics, isolated Fedora snapshot proof of post-#269 production data, and an independent retry-specific authority persistence design that does not collide with legacy `UNIQUE(consumer_id,event_id)`. No grant, lease, decision, intent, Task, ACK, dispatcher or provider follows automatically from the audit.
