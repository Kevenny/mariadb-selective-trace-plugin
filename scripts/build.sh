#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# build.sh
#
# Compila o servidor MariaDB (build mínima necessária para plugins) e,
# em seguida, compila apenas o(s) plugin(s) do diretório src/.
#
# Uso:
#   ./scripts/build.sh              # build completo (primeira vez, mais lento)
#   ./scripts/build.sh --plugin     # recompila só o plugin (build incremental)
#   ./scripts/build.sh --package    # gera pacote de teste (mysqld + plugin)
# ---------------------------------------------------------------------------
set -euo pipefail

MARIADB_SRC_DIR="${MARIADB_SRC_DIR:-/opt/mariadb-src}"
BUILD_DIR="${BUILD_DIR:-/opt/mariadb-build}"
PLUGIN_LINK_NAME="selective_log"
WORKSPACE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

MODE="${1:-full}"

link_plugin_source() {
    echo ">> Linkando código do plugin (src/) dentro da árvore de plugins do MariaDB"
    local target="${MARIADB_SRC_DIR}/plugin/${PLUGIN_LINK_NAME}"
    if [ ! -L "${target}" ]; then
        rm -rf "${target}"
        ln -s "${WORKSPACE_DIR}/src" "${target}"
        echo "   Link criado: ${target} -> ${WORKSPACE_DIR}/src"
    else
        echo "   Link já existe: ${target}"
    fi
}

configure_cmake() {
    echo ">> Configurando build com CMake (Ninja + ccache)"
    mkdir -p "${BUILD_DIR}"
    cd "${BUILD_DIR}"
    cmake "${MARIADB_SRC_DIR}" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DPLUGIN_SELECTIVE_LOG=DYNAMIC \
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
    echo ">> Compilando MariaDB completo (isso pode levar de 20 a 60+ minutos)"
    cd "${BUILD_DIR}"
    ninja -j"$(nproc)"
    echo ">> Build completo finalizado."
}

build_plugin_only() {
    link_plugin_source
    if [ ! -f "${BUILD_DIR}/build.ninja" ]; then
        echo "!! Build ainda não configurado. Rode primeiro: ./scripts/build.sh full"
        exit 1
    fi
    echo ">> Recompilando apenas o plugin ${PLUGIN_LINK_NAME}"
    cd "${BUILD_DIR}"
    cmake . >/dev/null   # re-scan em caso de novos arquivos .cc
    ninja "${PLUGIN_LINK_NAME}"
    echo ">> Plugin recompilado."
}

package_for_test() {
    echo ">> Empacotando plugin compilado para o container de teste"
    local out_dir="${WORKSPACE_DIR}/build/plugin_output"
    mkdir -p "${out_dir}"

    local so_path
    so_path=$(find "${BUILD_DIR}" -name "ha_${PLUGIN_LINK_NAME}.so" -o -name "${PLUGIN_LINK_NAME}.so" | head -n1)

    if [ -z "${so_path}" ]; then
        echo "!! Arquivo .so do plugin não encontrado. Rode o build antes."
        exit 1
    fi

    cp "${so_path}" "${out_dir}/${PLUGIN_LINK_NAME}.so"
    echo ">> Plugin copiado para: ${out_dir}/${PLUGIN_LINK_NAME}.so"
    echo ">> Suba o container de teste com:"
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
        echo "Uso: $0 [full|--plugin|--package]"
        exit 1
        ;;
esac
