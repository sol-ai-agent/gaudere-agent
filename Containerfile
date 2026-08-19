ARG BUILDER_IMAGE=registry.fedoraproject.org/fedora:44
FROM ${BUILDER_IMAGE} AS builder

ARG GAUDERE_REF=2123bf99417d28863dae5c964ac22f850482947f

USER root

RUN dnf install -y \
        autoconf \
        automake \
        diffutils \
        gcc-c++ \
        git \
        libtool \
        make \
        pkgconf-pkg-config \
        sqlite-devel \
    && dnf clean all

RUN git clone https://github.com/sol-ai-agent/gaudere.git /src/gaudere \
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
    && cp /opt/gaudere-agent/bin/gaudere-agent /opt/runtime/bin/

FROM registry.fedoraproject.org/fedora:44

RUN dnf install -y libstdc++ sqlite-libs \
    && dnf clean all \
    && useradd --uid 1000 --create-home --shell /sbin/nologin gaudere

COPY --from=builder /opt/runtime/ /usr/local/

RUN echo /usr/local/lib > /etc/ld.so.conf.d/gaudere.conf \
    && ldconfig

USER 1000:1000
ENTRYPOINT ["/usr/local/bin/gaudere-agent"]
