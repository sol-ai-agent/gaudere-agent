# Opt-in OpenAI service capability

The ordinary Gaudere service remains offline by default. `scripts/install-user-service.sh`
installs `deploy/quadlet/gaudere-agent.container`, which retains `Network=none`, mounts
no provider secret and passes no `--openai-model` argument.

Permanent provider capability is a separate explicit transition. It does **not** create
another service or another state owner. `scripts/install-openai-user-service.sh` copies
`deploy/quadlet/gaudere-agent-openai.container.in` into the same installed path
`gaudere-agent.container`, so the generated unit remains exactly
`gaudere-agent.service` and still owns the same production state database.

## Safety boundary

The OpenAI profile:

- selects only `gpt-5.6-sol`;
- mounts only Podman secret `gaudere-openai-api-key` at
  `/run/secrets/gaudere-openai-api-key`, UID/GID 1000 and mode 0400;
- permits normal rootless outbound networking but declares no `PublishPort`, so no
  inbound host port is exposed;
- uses `Pull=never` so service startup cannot contact a registry to replace the local
  runtime image;
- keeps the existing read-only filesystem, no-new-privileges, dropped capabilities,
  memory/PID limits, state volume and Unix live-control socket;
- does not include `--openai-once` or any task payload;
- retains the durable bootstrap budget: 12 lifetime new provider calls, at most 4 in a
  rolling 24 hours, minimum 15 minutes between new calls.

Provider startup validates the configured secret and registers the handler but does not
submit a Task. To prevent the first capability transition from reviving forgotten work,
the installer requires the service stopped, holds the production state lock, requires
schema v3, and refuses installation if any pre-existing
`provider.openai.responses` or `cognition.reflect.v1` Task is nonterminal, or if any
leased Task remains active.

## Initial capability-only proof

The first production activation must stop the offline service, install the OpenAI
profile, start the same service, and run:

```sh
sh scripts/validate-openai-service-capability.sh live-service-20260819-2317
```

With the untouched production budget, the validator requires:

- `provider_enabled=true`;
- `total_used=0`;
- `in_window_used=0`;
- service remains active;
- the historical durable task remains readable through the sole live owner.

The validator submits **no provider Task** and therefore performs no API request.

Before the first real production provider Task, reduce OpenAI project/model rate limits
to conservative values if supported by the platform. The project hard spend limit and
Gaudere's durable local call budget are independent layers and should both remain.

## Revert to offline

Stop `gaudere-agent.service`, run:

```sh
sh scripts/install-user-service.sh
```

then start the service explicitly. This overwrites the installed Quadlet with the
repository's offline profile, restoring `Network=none` and removing the secret/model
activation. No production database conversion is involved in this revert.
