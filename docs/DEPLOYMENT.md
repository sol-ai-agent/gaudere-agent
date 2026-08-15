# Rootless local deployment

This first deployment is tailored to the observed Fedora 44 workstation:

- Podman 5.8 rootless;
- cgroups v2 and systemd user manager;
- SELinux enforcing;
- crun and journald;
- no systemd lingering;
- no published port.

The runtime container has no network at all because the current executable
does not require outbound access yet. Networking will be widened only when a
specific provider contract requires it.

## Build

No host compiler or SQLite development package is required. The multi-stage
Containerfile installs missing build dependencies only inside its disposable
builder stage.

The build script first detects and reuses
`localhost/al_openai_cpp:10.0.0`, Bertrand's existing Autotools/GCC
development image. It falls back to the fully qualified Fedora 44 image only
when the local builder is absent.

```sh
./scripts/build-image.sh
```

The build pins the Gaudere library commit through `GAUDERE_REF`. Reusing the
local builder does not reuse its development mounts, shell configuration, or
history files; those belong to the interactive development workflow and are
not required by the reproducible image build.

## Install the user Quadlet

```sh
./scripts/install-user-service.sh
systemctl --user start gaudere-agent.service
systemctl --user status gaudere-agent.service
```

The installer does not enable the unit. With lingering disabled, the service
exists only while the user manager is available.

Persistent state is stored at:

```text
~/.local/share/gaudere/state/state.db
```

The directory is mounted with a private SELinux label. The image root is
read-only, all capabilities are dropped, new privileges are disabled, and the
container shares no host socket or device.

## Stop and inspect

```sh
systemctl --user stop gaudere-agent.service
journalctl --user -u gaudere-agent.service
```

Podman sends `SIGTERM` and waits 30 seconds. The application drains and
reports `gaudere-agent: safe` before exiting.

## Remove the unit

```sh
systemctl --user stop gaudere-agent.service
rm ~/.config/containers/systemd/gaudere-agent.container
systemctl --user daemon-reload
```

This does not delete the SQLite state directory or the image.
