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
    && test -x /opt/runtime/bin/gaudere-resume-after-wake \
    && test -x /opt/runtime/bin/gaudere-resume-after-wake-v1-prepare

FROM registry.fedoraproject.org/fedora:44

ARG GAUDERE_AGENT_REF
ARG GAUDERE_REF

LABEL org.opencontainers.image.source="https://github.com/sol-ai-agent/gaudere-agent" \
      org.opencontainers.image.revision="${GAUDERE_AGENT_REF}" \
      io.gaudere.agent.revision="${GAUDERE_AGENT_REF}" \
      io.gaudere.core.revision="${GAUDERE_REF}"

RUN dnf install -y libcurl libstdc++ sqlite-libs \
    && dnf clean all \
    && useradd --uid 1000 --create-home --shell /sbin/nologin gaudere

COPY --from=builder /opt/runtime/ /usr/local/

RUN test -x /usr/local/bin/gaudere-resume-after-wake \
    && test -x /usr/local/bin/gaudere-resume-after-wake-v1-prepare \
    && echo /usr/local/lib > /etc/ld.so.conf.d/gaudere.conf \
    && ldconfig

USER 1000:1000
ENTRYPOINT ["/usr/local/bin/gaudere-agent"]
