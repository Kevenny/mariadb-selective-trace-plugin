# Dockerfile - Ambiente de desenvolvimento para plugin MariaDB 11.4.4
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV MARIADB_VERSION=11.4.4
ENV MARIADB_SRC_DIR=/opt/mariadb-src
ENV BUILD_DIR=/opt/mariadb-build

# ---------------------------------------------------------------------------
# Dependências de build do MariaDB Server
# (lista baseada nas dependências oficiais do BUILD/ do MariaDB)
# ---------------------------------------------------------------------------
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    git \
    wget \
    curl \
    ca-certificates \
    bison \
    flex \
    libssl-dev \
    libboost-dev \
    libboost-all-dev \
    libncurses5-dev \
    libreadline-dev \
    libpam0g-dev \
    libaio-dev \
    libnuma-dev \
    libxml2-dev \
    libjudy-dev \
    libpcre2-dev \
    libsystemd-dev \
    zlib1g-dev \
    libzstd-dev \
    liblz4-dev \
    libsnappy-dev \
    libjemalloc-dev \
    libcurl4-openssl-dev \
    libevent-dev \
    liburing-dev \
    gnutls-dev \
    libkrb5-dev \
    gdb \
    valgrind \
    vim \
    less \
    tree \
    python3 \
    python3-pip \
    pkg-config \
    ccache \
    && rm -rf /var/lib/apt/lists/*

# ---------------------------------------------------------------------------
# Claude Code CLI (Node.js necessário)
# ---------------------------------------------------------------------------
RUN curl -fsSL https://deb.nodesource.com/setup_20.x | bash - \
    && apt-get install -y nodejs \
    && npm install -g @anthropic-ai/claude-code \
    && rm -rf /var/lib/apt/lists/*

# ---------------------------------------------------------------------------
# Usuário não-root para desenvolvimento (evita rodar mysqld como root sem flag)
# ---------------------------------------------------------------------------
RUN useradd -ms /bin/bash devuser \
    && mkdir -p ${MARIADB_SRC_DIR} ${BUILD_DIR} \
    && chown -R devuser:devuser ${MARIADB_SRC_DIR} ${BUILD_DIR} /opt

WORKDIR /workspace
COPY --chown=devuser:devuser . /workspace

USER devuser

# ccache acelera recompilações incrementais do plugin
ENV PATH="/usr/lib/ccache:${PATH}"
ENV CCACHE_DIR=/opt/ccache
RUN mkdir -p /opt/ccache

CMD ["/bin/bash"]
