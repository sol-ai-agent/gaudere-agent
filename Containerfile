ARG BUILDER_IMAGE=registry.fedoraproject.org/fedora:44
ARG GAUDERE_AGENT_REF
ARG GAUDERE_REF
FROM ${BUILDER_IMAGE} AS builder

# Deliberately has no default. scripts/build-image.sh and CI must both provide
# the same pinned commit from gaudere.ref so container and CI builds cannot drift.
ARG GAUDERE_AGENT_REF
ARG GAUDERE_REF

USER root

RUN dnf install -y \
        autoconf \
        automake \
        diffutils \
        gcc-c++ \
        git \
        json-devel \
        libcurl-devel \
        libtool \
        make \
        pkgconf-pkg-config \
        sqlite-devel \
    && dnf clean all

RUN test -n "${GAUDERE_AGENT_REF}" \
    && test "${#GAUDERE_AGENT_REF}" -eq 40 \
    && test -z "$(printf '%s' "${GAUDERE_AGENT_REF}" | tr -d '0-9a-f')" \
    && test -n "${GAUDERE_REF}" \
    && test "${#GAUDERE_REF}" -eq 40 \
    && test -z "$(printf '%s' "${GAUDERE_REF}" | tr -d '0-9a-f')" \
    && git clone https://github.com/sol-ai-agent/gaudere.git /src/gaudere \
    && git -C /src/gaudere checkout --detach "${GAUDERE_REF}"

RUN cd /src/gaudere \
    && autoreconf --install --force \
    && mkdir build \
    && cd build \
    && ../configure --prefix=/opt/gaudere \
       CXXFLAGS="-O2 -Wall -Wextra -Wpedantic -Werror" \
    && make --jobs=2 check \
    && make install

COPY . /src/gaudere-agent

RUN cd /src/gaudere-agent \
    && autoreconf --install --force \
    && mkdir build \
    && cd build \
    && PKG_CONFIG_PATH=/opt/gaudere/lib/pkgconfig \
       LD_LIBRARY_PATH=/opt/gaudere/lib \
       ../configure --prefix=/opt/gaudere-agent \
       CXXFLAGS="-O2 -Wall -Wextra -Wpedantic -Werror" \
    && PKG_CONFIG_PATH=/opt/gaudere/lib/pkgconfig \
       LD_LIBRARY_PATH=/opt/gaudere/lib \
       make --jobs=2 check \
    && make install

RUN mkdir -p /opt/runtime/bin /opt/runtime/lib \
    && cp -a /opt/gaudere/lib/libgaudere.so* /opt/runtime/lib/ \
    && cp -a /opt/gaudere/lib/libgaudere-persistence-sqlite.so* /opt/runtime/lib/ \
    && cp /opt/gaudere-agent/bin/gaudere-agent /opt/runtime/bin/ \
    && cp /opt/gaudere-agent/bin/gaudere-control /opt/runtime/bin/ \
    && cp /opt/gaudere-agent/bin/gaudere-resume-after-wake /opt/runtime/bin/ \
    && cp /opt/gaudere-agent/bin/gaudere-resume-after-wake-v1-prepare /opt/runtime/bin/ \
    && cp /opt/gaudere-agent/bin/gaudere-resume-after-wake-v1 /opt/runtime/bin/ \
    && cp /opt/gaudere-agent/bin/gaudere-current-cognition-prepare /opt/runtime/bin/ \
    && cp /opt/gaudere-agent/bin/gaudere-current-cognition /opt/runtime/bin/ \
    && cp /opt/gaudere-agent/bin/gaudere-autonomous-cognition-pulse /opt/runtime/bin/ \
    && cp /opt/gaudere-agent/bin/gaudere-continuity-delta-checkpoint /opt/runtime/bin/ \
    && cp /opt/gaudere-agent/bin/gaudere-local-activity-seed /opt/runtime/bin/ \
    && cp /opt/gaudere-agent/bin/gaudere-local-activity-status /opt/runtime/bin/ \
    && cp /opt/gaudere-agent/bin/gaudere-goose-tools-mcp /opt/runtime/bin/ \
    && test -x /opt/runtime/bin/gaudere-resume-after-wake \
    && test -x /opt/runtime/bin/gaudere-resume-after-wake-v1-prepare \
    && test -x /opt/runtime/bin/gaudere-resume-after-wake-v1 \
    && test -x /opt/runtime/bin/gaudere-current-cognition-prepare \
    && test -x /opt/runtime/bin/gaudere-current-cognition \
    && test -x /opt/runtime/bin/gaudere-autonomous-cognition-pulse \
    && test -x /opt/runtime/bin/gaudere-continuity-delta-checkpoint \
    && test -x /opt/runtime/bin/gaudere-local-activity-seed \
    && test -x /opt/runtime/bin/gaudere-local-activity-status \
    && test -x /opt/runtime/bin/gaudere-goose-tools-mcp

# Goose is pinned independently of the Gaudere source tree. Keep the download
# and archive verification in a disposable stage so the final runtime image does
# not need curl/tar merely to carry the local reasoning engine.
FROM registry.fedoraproject.org/fedora:44 AS goose-cli
ARG GOOSE_VERSION=1.49.0
ARG GOOSE_ARCHIVE_SHA256=38d5035e4a786f6b62abe0cd0f2bef7e6ac8041e3e006e2000561dd8df6aead3
RUN dnf install -y curl findutils gzip tar coreutils \
    && dnf clean all \
    && curl --fail --location --retry 3 \
       --output /tmp/goose.tar.gz \
       "https://github.com/aaif-goose/goose/releases/download/v${GOOSE_VERSION}/goose-x86_64-unknown-linux-gnu.tar.gz" \
    && printf '%s  %s\n' "${GOOSE_ARCHIVE_SHA256}" /tmp/goose.tar.gz | sha256sum -c - \
    && mkdir -p /tmp/goose-unpack /opt/goose \
    && tar -xzf /tmp/goose.tar.gz -C /tmp/goose-unpack \
    && test "$(find /tmp/goose-unpack -type f -name goose -perm /111 | wc -l)" -eq 1 \
    && GOOSE_BIN="$(find /tmp/goose-unpack -type f -name goose -perm /111 -print -quit)" \
    && install -m 0755 "${GOOSE_BIN}" /opt/goose/goose \
    && /opt/goose/goose --version

FROM registry.fedoraproject.org/fedora:44

ARG GAUDERE_AGENT_REF
ARG GAUDERE_REF
ARG GOOSE_VERSION=1.49.0

LABEL org.opencontainers.image.source="https://github.com/sol-ai-agent/gaudere-agent" \
      org.opencontainers.image.revision="${GAUDERE_AGENT_REF}" \
      io.gaudere.agent.revision="${GAUDERE_AGENT_REF}" \
      io.gaudere.core.revision="${GAUDERE_REF}" \
      io.gaudere.goose.version="${GOOSE_VERSION}"

RUN dnf install -y libcurl libstdc++ sqlite-libs \
    && dnf clean all \
    && useradd --uid 1000 --create-home --shell /sbin/nologin gaudere

COPY --from=builder /opt/runtime/ /usr/local/
COPY --from=goose-cli /opt/goose/goose /usr/local/bin/goose

RUN test -x /usr/local/bin/gaudere-resume-after-wake \
    && test -x /usr/local/bin/gaudere-resume-after-wake-v1-prepare \
    && test -x /usr/local/bin/gaudere-resume-after-wake-v1 \
    && test -x /usr/local/bin/gaudere-current-cognition-prepare \
    && test -x /usr/local/bin/gaudere-current-cognition \
    && test -x /usr/local/bin/gaudere-autonomous-cognition-pulse \
    && test -x /usr/local/bin/gaudere-continuity-delta-checkpoint \
    && test -x /usr/local/bin/gaudere-local-activity-seed \
    && test -x /usr/local/bin/gaudere-local-activity-status \
    && test -x /usr/local/bin/gaudere-goose-tools-mcp \
    && test -x /usr/local/bin/goose \
    && /usr/local/bin/goose --version \
    && echo /usr/local/lib > /etc/ld.so.conf.d/gaudere.conf \
    && ldconfig

USER 1000:1000
ENTRYPOINT ["/usr/local/bin/gaudere-agent"]
