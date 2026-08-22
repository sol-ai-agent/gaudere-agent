# Initial production provider capability checklist

This checklist is deliberately split from the first real provider call.

## Capability-only transition

1. Confirm current offline service is healthy and provider budget is still zero.
2. Stop `gaudere-agent.service` cleanly.
3. Run `scripts/install-openai-user-service.sh`.
   - It accepts exactly schema v3 or v4 and rejects every other schema version.
   - It holds the production state lock.
   - It refuses inherited nonterminal OpenAI or bounded-reflection Tasks and any
     active leased Task.
   - It refuses an image that predates bounded reflection, using an offline control
     binary preflight with no state or secret mounted.
   - On schema v4, it requires the selected image to reopen a disposable consistent
     copy with networking disabled, no secret/provider configuration and no
     `--wake-intents`; the production database is never mounted by that probe.
   - It requires the local runtime image and named Podman secret to exist.
   - It installs the same byte-for-byte OpenAI profile but does not start the
     service. Schema-v4 compatibility does not activate WakeIntent.
4. Start `gaudere-agent.service` explicitly.
5. Run `scripts/validate-openai-service-capability.sh live-service-20260819-2317`.
6. Require `provider_enabled=true`, `total_used=0`, `in_window_used=0`, historical
   Task intact and service still active.
7. Inspect the journal for the fixed model and budget policy; do not submit provider
   work yet.

## Before the first real production call

- Keep the OpenAI project hard spend limit in place.
- Reduce project/model rate limits conservatively if the platform permits it.
- Re-check live budget is still zero.
- Submit exactly one bounded provider Task through live control.
- Inspect the same durable Task until terminal.
- Require normalized provider usage metadata to be present for a successful result.
- Confirm budget moves from 0 to exactly 1 and the service remains active.

## Offline rollback

If capability startup or validation fails, stop the service, run
`scripts/install-user-service.sh`, then start it explicitly. The offline profile restores
`Network=none` and removes the provider secret/model arguments without changing the
production database.
