#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# download-mariadb-source.sh
#
# Baixa o código-fonte oficial do MariaDB 11.4.4 (branch de release, via git,
# com submódulos) para dentro do container de desenvolvimento.
#
# Uso:
#   ./scripts/download-mariadb-source.sh
#
# Variáveis de ambiente:
#   MARIADB_VERSION   (default: 11.4.4)
#   MARIADB_SRC_DIR   (default: /opt/mariadb-src)
# ---------------------------------------------------------------------------
set -euo pipefail

MARIADB_VERSION="${MARIADB_VERSION:-11.4.4}"
MARIADB_SRC_DIR="${MARIADB_SRC_DIR:-/opt/mariadb-src}"
REPO_URL="https://github.com/MariaDB/server.git"
TAG="mariadb-${MARIADB_VERSION}"

echo ">> Preparando diretório de fonte em: ${MARIADB_SRC_DIR}"
mkdir -p "${MARIADB_SRC_DIR}"

if [ -d "${MARIADB_SRC_DIR}/.git" ]; then
    echo ">> Repositório já existe. Atualizando referências..."
    cd "${MARIADB_SRC_DIR}"
    git fetch --tags origin
else
    echo ">> Clonando MariaDB server (shallow, apenas a tag ${TAG})..."
    git clone --branch "${TAG}" --depth 1 "${REPO_URL}" "${MARIADB_SRC_DIR}"
    cd "${MARIADB_SRC_DIR}"
fi

echo ">> Fazendo checkout da tag ${TAG}"
git checkout "tags/${TAG}"

echo ">> Inicializando submódulos (wsrep, storage engines externos, etc.)"
git submodule update --init --recursive --depth 1

echo ""
echo ">> Fonte do MariaDB ${MARIADB_VERSION} pronto em: ${MARIADB_SRC_DIR}"
echo ">> Diretórios relevantes para o plugin:"
echo "     - ${MARIADB_SRC_DIR}/plugin/               (plugins existentes, referência)"
echo "     - ${MARIADB_SRC_DIR}/include/mysql/         (headers da plugin API)"
echo "     - ${MARIADB_SRC_DIR}/sql/                   (núcleo do servidor: sql_parse.cc, log.cc, etc.)"
echo ""
echo ">> Próximo passo: ./scripts/build.sh  (build inicial completo)"
echo "   ou peça ao Claude Code para ler docs/ARCHITECTURE.md e iniciar a implementação."
