#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# download-mariadb-source.sh
#
# Downloads the official MariaDB source (release branch, via git, with
# submodules) into the development container.
#
# Usage:
#   ./scripts/download-mariadb-source.sh
#
# Environment variables:
#   MARIADB_VERSION   (default: 11.4.4)
#   MARIADB_SRC_DIR   (default: /opt/mariadb-src)
# ---------------------------------------------------------------------------
set -euo pipefail

MARIADB_VERSION="${MARIADB_VERSION:-11.4.4}"
MARIADB_SRC_DIR="${MARIADB_SRC_DIR:-/opt/mariadb-src}"
REPO_URL="https://github.com/MariaDB/server.git"
TAG="mariadb-${MARIADB_VERSION}"

echo ">> Preparing source directory at: ${MARIADB_SRC_DIR}"
mkdir -p "${MARIADB_SRC_DIR}"

if [ -d "${MARIADB_SRC_DIR}/.git" ]; then
    echo ">> Repository already exists. Updating refs..."
    cd "${MARIADB_SRC_DIR}"
    git fetch --tags origin
else
    echo ">> Cloning MariaDB server (shallow, tag ${TAG} only)..."
    git clone --branch "${TAG}" --depth 1 "${REPO_URL}" "${MARIADB_SRC_DIR}"
    cd "${MARIADB_SRC_DIR}"
fi

echo ">> Checking out tag ${TAG}"
git checkout "tags/${TAG}"

echo ">> Initializing submodules (wsrep, external storage engines, etc.)"
git submodule update --init --recursive --depth 1

echo ""
echo ">> MariaDB ${MARIADB_VERSION} source ready at: ${MARIADB_SRC_DIR}"
echo ">> Directories relevant to the plugin:"
echo "     - ${MARIADB_SRC_DIR}/plugin/               (existing plugins, reference)"
echo "     - ${MARIADB_SRC_DIR}/include/mysql/         (plugin API headers)"
echo "     - ${MARIADB_SRC_DIR}/sql/                   (server core: sql_parse.cc, log.cc, etc.)"
echo ""
echo ">> Next step: ./scripts/build.sh  (initial full build)"
echo "   or ask Claude Code to read docs/ARCHITECTURE.md and start implementing."
