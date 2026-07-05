#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# run-mtr.sh — runs the plugin's MTR integration suite.
#
# mysql-test-run resolves suites relative to the MariaDB *source tree*, so we
# install src/mysql-test/suite/selective_trace there before calling mtr. In the
# official packaging (make install) this is done by MYSQL_ADD_PLUGIN's
# INSTALL_MYSQL_TEST; here we replicate it for the dev flow in the build tree.
#
# Usage (inside the dev container):  ./scripts/run-mtr.sh [extra mtr args]
#   or from the host:  docker exec mariadb-plugin-dev bash -lc './scripts/run-mtr.sh'
# ---------------------------------------------------------------------------
set -euo pipefail

MARIADB_SRC_DIR="${MARIADB_SRC_DIR:-/opt/mariadb-src}"
BUILD_DIR="${BUILD_DIR:-/opt/mariadb-build}"
WORKSPACE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

SUITE_SRC="${WORKSPACE_DIR}/src/mysql-test/suite/selective_trace"
SUITE_DST="${MARIADB_SRC_DIR}/mysql-test/suite/selective_trace"

echo ">> Installing the MTR suite at ${SUITE_DST}"
rm -rf "${SUITE_DST}"
cp -r "${SUITE_SRC}" "${SUITE_DST}"

echo ">> Ensuring the server + plugin are built"
# MTR needs mariadbd and the local clients (mariadb-admin etc.), not just
# the .so. A partial build (plugin target only) is not enough.
if [ ! -x "${BUILD_DIR}/sql/mariadbd" ]; then
    echo "!! ${BUILD_DIR}/sql/mariadbd does not exist — run first: ./scripts/build.sh full"
    exit 1
fi
if [ ! -f "${BUILD_DIR}/plugin/selective_trace/selective_trace.so" ]; then
    (cd "${BUILD_DIR}" && ninja selective_trace)
fi

echo ">> Running mtr --suite=selective_trace"
cd "${BUILD_DIR}/mysql-test"
exec ./mtr --suite=selective_trace "$@"
