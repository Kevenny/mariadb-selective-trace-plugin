#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# validate-ol8.sh — validates the selective_trace.so built for EL8 on a
# clean Oracle Linux 8 with MariaDB 11.4 installed via the official RPM.
#
# Runs INSIDE an oraclelinux:8 container (as root) with the .so mounted
# at /plugin_out:
#   docker run --rm -i -v "<repo>/build/plugin_output-ol8:/plugin_out:ro" \
#       oraclelinux:8 bash < scripts/validate-ol8.sh
# ---------------------------------------------------------------------------
set -euo pipefail

echo ">> [1/4] Setting up the MariaDB 11.4 repo and installing RPMs"
curl -LsS https://r.mariadb.com/downloads/mariadb_repo_setup \
    | bash -s -- --mariadb-server-version=mariadb-11.4 >/dev/null
dnf -y install MariaDB-server MariaDB-client >/dev/null
mariadbd --version

echo ">> [2/4] Installing the plugin and initializing the datadir"
cp /plugin_out/selective_trace.so /usr/lib64/mysql/plugin/
mariadb-install-db --user=mysql >/dev/null

echo ">> [3/4] Starting mariadbd with the plugin"
/usr/sbin/mariadbd --user=mysql --skip-networking \
    --socket=/tmp/m.sock \
    --plugin-load-add=selective_trace.so \
    --plugin-maturity=experimental \
    --selective_trace_enabled=ON \
    --selective_trace_schemas_to_log=hotdb \
    --selective_trace_file_path=/tmp/selective_trace.json \
    >/tmp/mariadbd.log 2>&1 &

for i in $(seq 1 60); do
    mariadb -uroot -S /tmp/m.sock -e "SELECT 1" >/dev/null 2>&1 && break
    sleep 1
done

M="mariadb -uroot -S /tmp/m.sock"
$M -e "SELECT VERSION() AS versao"
$M -e "SELECT PLUGIN_NAME, PLUGIN_STATUS, PLUGIN_AUTH_VERSION
       FROM information_schema.PLUGINS WHERE PLUGIN_NAME='selective_trace'"

echo ">> [4/4] Functional smoke test"
$M --force <<'SQL'
CREATE DATABASE hotdb;
CREATE DATABASE colddb;
CREATE TABLE hotdb.t (id INT PRIMARY KEY, v VARCHAR(30));
CREATE TABLE colddb.t (id INT PRIMARY KEY, v VARCHAR(30));
INSERT INTO hotdb.t VALUES (1,'el8');
SELECT * FROM hotdb.t;
INSERT INTO colddb.t VALUES (9,'fora do filtro');
SELECT * FROM hotdb.t JOIN colddb.t USING (id);
SELECT * FROM hotdb.nao_existe;
SET GLOBAL selective_trace_output='TABLE';
UPDATE hotdb.t SET v='table-mode' WHERE id=1;
DO SLEEP(2);
SQL

echo "--- arquivo JSON (modo FILE) ---"
cat /tmp/selective_trace.json
echo "--- tabela mysql.selective_trace_events (modo TABLE) ---"
$M -e "SELECT tables_involved, command, error_code, LEFT(query,50) AS q
       FROM mysql.selective_trace_events"
echo "--- UNINSTALL/INSTALL ---"
$M -e "UNINSTALL PLUGIN selective_trace;
       INSTALL PLUGIN selective_trace SONAME 'selective_trace.so';
       SELECT 'servidor vivo' AS status"
mariadb-admin -uroot -S /tmp/m.sock shutdown
echo ">> OL8 VALIDATION COMPLETE"
