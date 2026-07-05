#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# setup-dev-env.sh
#
# Single entry point to prepare the whole environment:
#   1. Start the development container (docker compose)
#   2. Download the MariaDB 11.4.4 source into the container
#   3. Leave everything ready to start analysis/implementation
#
# Usage (on the host, outside the container):
#   ./scripts/setup-dev-env.sh
# ---------------------------------------------------------------------------
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

echo "=================================================================="
echo " MariaDB 11.4.4 - Selective Query Log Plugin - Setup"
echo "=================================================================="

echo ""
echo ">> [1/3] Building the development Docker image..."
docker compose -f docker/docker-compose.yml build dev

echo ""
echo ">> [2/3] Starting the development container..."
docker compose -f docker/docker-compose.yml up -d dev

echo ""
echo ">> [3/3] Downloading the MariaDB 11.4.4 source into the container..."
docker compose -f docker/docker-compose.yml exec dev \
    bash -lc "./scripts/download-mariadb-source.sh"

echo ""
echo "=================================================================="
echo " Environment ready!"
echo "=================================================================="
echo ""
echo "To enter the container:"
echo "   docker compose -f docker/docker-compose.yml exec dev bash"
echo ""
echo "Inside the container, to start Claude Code on the project:"
echo "   cd /workspace && claude"
echo ""
echo "Claude Code should read CLAUDE.md at the project root,"
echo "which holds the full spec of the plugin to implement."
echo ""
