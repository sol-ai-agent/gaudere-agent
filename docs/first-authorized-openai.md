# First authorized OpenAI call

This procedure is intentionally separate from normal service deployment. The normal
Quadlet remains offline (`Network=none`) throughout this phase.

## Initial identity model

For the first real credential, use a dedicated OpenAI project named `Gaudere` and a
**user-owned project API key controlled by Bertrand**.

This is intentional. User-owned project keys can be created with Restricted
permissions immediately. A project service account is a better future identity for
Gaudere itself, but its initial key is broader by default and can be introduced later
once autonomous credential ownership is actually useful.

The human owner must retain the ability to revoke the OpenAI-side key independently of
Gaudere.

## Project guardrails before creating the key

Before any secret is installed on the host:

1. Create/select the dedicated `Gaudere` project.
2. In project Limits / Model Usage, initially allow only `gpt-5.6-sol` if the UI
   permits a model allowlist. The generic `gpt-5.6` alias currently routes to Sol, but
   the bootstrap configuration uses the explicit Sol model name.
3. Set conservative project rate limits for that model.
4. Set a low monthly project spend limit and enable **hard enforcement**. For the
   experimental bootstrap phase, `$5/month` is the recommended starting ceiling; it
   can be raised deliberately later.
5. Add a spend alert below the hard ceiling (for example `$1`) so unexpected activity
   is visible well before the limit.
6. Create a new **project** secret key owned by Bertrand.
7. Choose `Restricted` permissions.
8. Permit only Read/Write on the Responses endpoint as required by the current key UI,
   and set unrelated API resources/endpoints to `None`.

The exact permission labels are OpenAI UI metadata and may evolve. If the UI cannot
express this narrow Responses-only key, stop and review the actual permission list
before broadening it.

## Install the key locally

Never paste the real key into Git, a shell command line, a normal environment
variable, a chat, or a file in the checkout.

After copying the newly created OpenAI key, run:

```sh
sh scripts/install-openai-secret.sh
```

The script prompts on the terminal with echo disabled and creates the rootless Podman
secret:

```text
gaudere-openai-api-key
```

The value is sent to Podman through stdin. It is not printed. If deliberate rotation
is needed later:

```sh
sh scripts/install-openai-secret.sh --replace
```

Podman's replacement affects newly created containers; it does not silently change a
secret already mounted into a running container. This matches Gaudere's explicit
restart/activation model.

## Rebuild before using the real key

Pull and rebuild current main before an authorized call so the image includes:

- normalized provider HTTP diagnostics that never persist provider `error.message`;
- explicit OpenAI `max_output_tokens: 1024` generation bound;
- the explicit `gpt-5.6-sol` authorized-validation default.

```sh
git pull --ff-only
./scripts/build-image.sh
```

Do not modify the normal Quadlet yet.

## First authorized call

Run only the disposable validator:

```sh
sh scripts/validate-openai-authorized.sh
```

It uses:

- disposable SQLite state beneath `~/.local/share/gaudere/validation/`;
- the existing rootless Podman secret mounted mode `0400`;
- no published inbound port;
- model `gpt-5.6-sol` by default;
- the normal durable Task -> Action -> effect marker -> OpenAI adapter -> HTTPS path;
- the provider-wide 1024 output-token cap.

Expected terminal shape:

```text
status=succeeded
attempts=1/2
result_content_type="text/plain; charset=utf-8"
result_output="..."
gaudere-agent: safe
gaudere authorized OpenAI validation: PASS
```

Any failure, ambiguity, unexpected model permission, spend-limit response, or transport
problem stops this phase. Do not compensate by broadening the key or permanently
enabling networking.

## Revocation / cleanup

Host-side removal:

```sh
podman secret rm gaudere-openai-api-key
```

OpenAI-side revocation remains authoritative and must also be available to Bertrand in
the project API Keys UI.

The normal always-running Gaudere service must remain offline until the authorized
one-shot has been reviewed and a separate deployment change is explicitly designed.
