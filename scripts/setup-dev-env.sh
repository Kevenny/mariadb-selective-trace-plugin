#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# setup-dev-env.sh
#
# Ponto de entrada único para preparar todo o ambiente:
#   1. Sobe o container de desenvolvimento (docker compose)
#   2. Baixa o fonte do MariaDB 11.4.4 dentro do container
#   3. Deixa tudo pronto para o Claude Code começar a análise/implementação
#
# Uso (na máquina host, fora do container):
#   ./scripts/setup-dev-env.sh
# ---------------------------------------------------------------------------
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

echo "=================================================================="
echo " MariaDB 11.4.4 - Selective Query Log Plugin - Setup"
echo "=================================================================="

echo ""
echo ">> [1/3] Buildando imagem Docker de desenvolvimento..."
docker compose -f docker/docker-compose.yml build dev

echo ""
echo ">> [2/3] Subindo container de desenvolvimento..."
docker compose -f docker/docker-compose.yml up -d dev

echo ""
echo ">> [3/3] Baixando o fonte do MariaDB 11.4.4 dentro do container..."
docker compose -f docker/docker-compose.yml exec dev \
    bash -lc "./scripts/download-mariadb-source.sh"

echo ""
echo "=================================================================="
echo " Ambiente pronto!"
echo "=================================================================="
echo ""
echo "Para entrar no container:"
echo "   docker compose -f docker/docker-compose.yml exec dev bash"
echo ""
echo "Dentro do container, para iniciar o Claude Code no projeto:"
echo "   cd /workspace && claude"
echo ""
echo "O Claude Code deve ler o arquivo CLAUDE.md na raiz do projeto,"
echo "que contém toda a especificação do plugin a ser implementado."
echo ""
