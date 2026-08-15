FROM registry.fedoraproject.org/fedora:44 AS builder

ARG GAUDERE_REF=6f6ad032bbfff837f17f84067132a2922c0f474b

RUN dnf install -y \
        autoconf \
        automake \
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
    && ../configure --prefix=/usr/local CXXFLAGS="-O2 -Wall -Wextra -Wpedantic -Werror" \
    && make --jobs=2 check \
    && make install

COPY . /src/gaudere-agent

RUN cd /src/gaudere-agent \
    && autoreconf --install --force \
    && mkdir build \
    && cd build \
    && PKG_CONFIG_PATH=/usr/local/lib/pkgconfig \
       LD_LIBRARY_PATH=/usr/local/lib \
       ../configure --prefix=/usr/local CXXFLAGS="-O2 -Wall -Wextra -Wpedantic -Werror" \
    && PKG_CONFIG_PATH=/usr/local/lib/pkgconfig \
       LD_LIBRARY_PATH=/usr/local/lib \
       make --jobs=2 check \
    && make install

FROM registry.fedoraproject.org/fedora:44

RUN dnf install -y libstdc++ sqlite-libs \
    && dnf clean all \
    && useradd --uid 1000 --create-home --shell /sbin/nologin gaudere

COPY --from=builder /usr/local /usr/local

RUN echo /usr/local/lib > /etc/ld.so.conf.d/gaudere.conf \
    && ldconfig

USER 1000:1000
ENTRYPOINT ["/usr/local/bin/gaudere-agent"]
