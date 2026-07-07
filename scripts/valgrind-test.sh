#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# valgrind-test.sh — validates the absence of leaks attributable to the plugin
#
# Roda DENTRO do container de desenvolvimento (mariadb-plugin-dev), que tem
# the mariadbd built with symbols in /opt/mariadb-build:
#   docker exec -i mariadb-plugin-dev bash < scripts/valgrind-test.sh
#
# Sobe o mariadbd sob Valgrind com o selective_trace carregado, roda uma
# a battery of queries covering both output modes and filter changes,
# derruba o servidor de forma limpa e reporta leaks com frames do plugin.
# ---------------------------------------------------------------------------
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-/opt/mariadb-build}"
SRC_DIR="${MARIADB_SRC_DIR:-/opt/mariadb-src}"
DATADIR=/tmp/vgdata
SOCK=/tmp/vg.sock
VGLOG=/tmp/valgrind_mysqld.log

CLIENT="$BUILD_DIR/client/mariadb -uroot -S $SOCK"
ADMIN="$BUILD_DIR/client/mariadb-admin -uroot -S $SOCK"

rm -rf "$DATADIR" "$VGLOG"
mkdir -p "$DATADIR"

echo ">> Inicializando datadir de teste"
cd "$BUILD_DIR"
scripts/mariadb-install-db --no-defaults --srcdir="$SRC_DIR" \
    --datadir="$DATADIR" --auth-root-authentication-method=normal \
    >/dev/null

echo ">> Subindo mariadbd sob Valgrind (lento: aguarde o socket)"
valgrind --tool=memcheck --leak-check=full \
    --show-leak-kinds=definite,indirect,possible --num-callers=24 \
    --log-file="$VGLOG" \
    "$BUILD_DIR/sql/mariadbd" --no-defaults \
    --datadir="$DATADIR" --socket="$SOCK" --skip-networking \
    --plugin-dir="$BUILD_DIR/plugin/selective_trace" \
    --plugin-load-add=selective_trace.so \
    --plugin-maturity=experimental \
    --loose-innodb-buffer-pool-size=64M \
    >/tmp/vg_mysqld_stderr.log 2>&1 &
VGPID=$!

for i in $(seq 1 180); do
    $CLIENT -e "SELECT 1" >/dev/null 2>&1 && break
    kill -0 $VGPID 2>/dev/null || { echo "mariadbd morreu"; tail -40 /tmp/vg_mysqld_stderr.log; exit 1; }
    sleep 2
done
$CLIENT -e "SELECT VERSION()" >/dev/null

echo ">> Plugin exercise battery"
# --force: a bateria inclui um erro de SQL proposital (evento com error_code)
$CLIENT --force <<'SQL'
CREATE DATABASE vg_hot;
CREATE DATABASE vg_cold;
CREATE TABLE vg_hot.t (id INT AUTO_INCREMENT PRIMARY KEY, v VARCHAR(64));
CREATE TABLE vg_cold.t (id INT AUTO_INCREMENT PRIMARY KEY, v VARCHAR(64));

SET GLOBAL selective_trace_enabled=ON;
SET GLOBAL selective_trace_schemas_to_log='vg_hot';
SET GLOBAL selective_trace_file_path='/tmp/vg_selective.json';

-- modo FILE
INSERT INTO vg_hot.t (v) VALUES ('a'),('b'),('c');
SELECT COUNT(*) FROM vg_hot.t;
UPDATE vg_hot.t SET v='x' WHERE id=1;
DELETE FROM vg_hot.t WHERE id=2;
INSERT INTO vg_cold.t (v) VALUES ('invisivel');
SELECT * FROM vg_hot.t JOIN vg_cold.t USING (id);

-- troca de filtros repetida (exercita update/free das listas)
SET GLOBAL selective_trace_tables_to_log='vg_cold.t,logs.*';
SET GLOBAL selective_trace_tables_to_log='vg_cold.t';
SET GLOBAL selective_trace_tables_to_log='';
SET GLOBAL selective_trace_schemas_to_log='vg_hot,vg_cold,a,b,c,d,e,f';
SET GLOBAL selective_trace_schemas_to_log='vg_hot:insert|update,vg_cold:dml';
SET GLOBAL selective_trace_tables_to_log='vg_cold.t:delete,logs.*:ddl';
SET GLOBAL selective_trace_schemas_to_log='vg_hot';
SET GLOBAL selective_trace_tables_to_log='';
-- connection filter: parse/sort/dedupe of the id list, then clear
SET GLOBAL selective_trace_connections_to_log='7,42,42,100,1';
SET GLOBAL selective_trace_connections_to_log='';
SET GLOBAL selective_trace_file_path='/tmp/vg_selective2.json';

-- erro de SQL (evento com error_code)
SELECT * FROM vg_hot.nao_existe;

-- mascaramento de credenciais (aloca a query saneada)
SET GLOBAL selective_trace_schemas_to_log='vg_hot,mysql';
CREATE USER IF NOT EXISTS vguser@localhost IDENTIFIED BY 'vgsecret';
SET PASSWORD FOR vguser@localhost = PASSWORD('vgsecret2');
DROP USER IF EXISTS vguser@localhost;
SET GLOBAL selective_trace_schemas_to_log='vg_hot';

-- TABLE mode (internal thread + lazy table creation)
SET GLOBAL selective_trace_output='TABLE';
INSERT INTO vg_hot.t (v) VALUES ('table-mode');
SELECT COUNT(*) FROM vg_hot.t;
DO SLEEP(2);
SELECT COUNT(*) AS eventos FROM mysql.selective_trace_events;

-- min_duration
SET GLOBAL selective_trace_min_duration_ms=500;
SELECT SLEEP(0.6);
SELECT 1;
SET GLOBAL selective_trace_min_duration_ms=0;

-- desliga e religa
SET GLOBAL selective_trace_enabled=OFF;
SELECT COUNT(*) FROM vg_hot.t;
SET GLOBAL selective_trace_enabled=ON;
SQL

# erro proposital em SET (caminho de check que rejeita)
$CLIENT -e "SET GLOBAL selective_trace_tables_to_log='invalido'" 2>/dev/null || true

echo ">> UNINSTALL/INSTALL sob Valgrind"
$CLIENT -e "UNINSTALL PLUGIN selective_trace" || true
sleep 2
$CLIENT -e "INSTALL PLUGIN selective_trace SONAME 'selective_trace.so'"
$CLIENT -e "SET GLOBAL selective_trace_enabled=ON; SET GLOBAL selective_trace_schemas_to_log='vg_hot'; SELECT COUNT(*) FROM vg_hot.t"

echo ">> Shutdown limpo"
$ADMIN shutdown
wait $VGPID || true

echo ""
echo "== RESUMO VALGRIND (frames do plugin) =="
grep -n "selective_trace\|filter_engine\|log_writer" "$VGLOG" | head -40 || echo "(nenhum frame do plugin em leaks)"
echo ""
echo "== LEAK SUMMARY =="
grep -A6 "LEAK SUMMARY" "$VGLOG" | head -12
echo ""
echo "Log completo: $VGLOG"
