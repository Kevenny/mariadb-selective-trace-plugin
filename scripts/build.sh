#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# build.sh
#
# Builds the MariaDB server (minimal build needed for plugins) and then
# compiles only the plugin(s) under src/.
#
# Usage:
#   ./scripts/build.sh              # full build (first time, slower)
#   ./scripts/build.sh --plugin     # recompile the plugin only (incremental)
#   ./scripts/build.sh --package    # produce a test package (mysqld + plugin)
# ---------------------------------------------------------------------------
set -euo pipefail

MARIADB_SRC_DIR="${MARIADB_SRC_DIR:-/opt/mariadb-src}"
BUILD_DIR="${BUILD_DIR:-/opt/mariadb-build}"
PLUGIN_LINK_NAME="selective_trace"
WORKSPACE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

MODE="${1:-full}"

link_plugin_source() {
    echo ">> Linking plugin source (src/) into the MariaDB plugin tree"
    local target="${MARIADB_SRC_DIR}/plugin/${PLUGIN_LINK_NAME}"
    if [ ! -L "${target}" ]; then
        rm -rf "${target}"
        ln -s "${WORKSPACE_DIR}/src" "${target}"
        echo "   Link created: ${target} -> ${WORKSPACE_DIR}/src"
    else
        echo "   Link already exists: ${target}"
    fi
}

configure_cmake() {
    echo ">> Configuring build with CMake (Ninja + ccache)"
    mkdir -p "${BUILD_DIR}"
    cd "${BUILD_DIR}"
    cmake "${MARIADB_SRC_DIR}" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DPLUGIN_SELECTIVE_TRACE=DYNAMIC \
        -DWITHOUT_TOKUDB=1 \
        -DWITHOUT_MROONGA=1 \
        -DWITHOUT_ROCKSDB=1 \
        -DWITHOUT_OQGRAPH=1 \
        -DWITHOUT_SPHINX=1 \
        -DWITH_UNIT_TESTS=OFF \
        -DWITH_EMBEDDED_SERVER=OFF
}

build_full() {
    link_plugin_source
    configure_cmake
    echo ">> Building the full MariaDB (this can take 20 to 60+ minutes)"
    cd "${BUILD_DIR}"
    ninja -j"$(nproc)"
    echo ">> Full build finished."
}

build_plugin_only() {
    link_plugin_source
    if [ ! -f "${BUILD_DIR}/build.ninja" ]; then
        echo "!! Build not configured yet. Run first: ./scripts/build.sh full"
        exit 1
    fi
    echo ">> Recompiling only the plugin ${PLUGIN_LINK_NAME}"
    cd "${BUILD_DIR}"
    cmake . >/dev/null   # re-scan in case of new .cc files
    ninja "${PLUGIN_LINK_NAME}"
    echo ">> Plugin recompiled."
}

package_for_test() {
    echo ">> Packaging the compiled plugin for the test container"
    local out_dir="${PLUGIN_OUTPUT_DIR:-${WORKSPACE_DIR}/build/plugin_output}"
    mkdir -p "${out_dir}"

    local so_path
    so_path=$(find "${BUILD_DIR}" -name "ha_${PLUGIN_LINK_NAME}.so" -o -name "${PLUGIN_LINK_NAME}.so" | head -n1)

    if [ -z "${so_path}" ]; then
        echo "!! Plugin .so not found. Run the build first."
        exit 1
    fi

    cp "${so_path}" "${out_dir}/${PLUGIN_LINK_NAME}.so"
    echo ">> Plugin copied to: ${out_dir}/${PLUGIN_LINK_NAME}.so"
    echo ">> Start the test container with:"
    echo "     docker compose --profile test up -d mariadb-test"
}

case "${MODE}" in
    full|"")
        build_full
        ;;
    --plugin)
        build_plugin_only
        ;;
    --package)
        package_for_test
        ;;
    *)
        echo "Usage: $0 [full|--plugin|--package]"
        exit 1
        ;;
esac
