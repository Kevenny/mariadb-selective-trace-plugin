#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# benchmark-profile.sh — teste com perfil de produção:
#   ~100k SELECT/min + ~20k INSERT/UPDATE/DELETE/min  (83% leitura / 17% escrita)
#
# Mix exato 15 SELECT : 3 writes (1 INSERT + 1 UPDATE + 1 DELETE) por iteração.
#
# Parte 1 — capacidade (full speed): qps por cenário → headroom sobre a
#           demanda real (~2.000 qps).
# Parte 2 — sustentado (SUSTAIN_SECS, default 300s) no cenário realista
#           (filtro schema:dml, output FILE): RSS do mariadbd, contadores do
#           plugin e crescimento do arquivo amostrados a cada 30s.
#
# Uso: docker exec -i mariadb-plugin-test bash < scripts/benchmark-profile.sh
# ---------------------------------------------------------------------------
set -euo pipefail

MYSQL="mariadb -uroot -p${MARIADB_ROOT_PASSWORD:-devpassword}"
SLAP="mariadb-slap -uroot -p${MARIADB_ROOT_PASSWORD:-devpassword}"

CONCURRENCY="${PROFILE_CONCURRENCY:-16}"
QUERIES="${PROFILE_QUERIES:-36000}"      # múltiplo de 18 (1 iteração = 18 queries)
SUSTAIN_SECS="${SUSTAIN_SECS:-300}"

SEL="SELECT v FROM t WHERE id = 500"
MIX="$SEL;$SEL;$SEL;$SEL;$SEL;INSERT INTO t (v) VALUES ('profile-row-payload');$SEL;$SEL;$SEL;$SEL;$SEL;UPDATE t SET v='updated' WHERE id = 500;$SEL;$SEL;$SEL;$SEL;$SEL;DELETE FROM t WHERE id = LAST_INSERT_ID()"

prepare() {
    $MYSQL -e "
        CREATE DATABASE IF NOT EXISTS app_main;
        DROP TABLE IF EXISTS app_main.t;
        CREATE TABLE app_main.t (id INT AUTO_INCREMENT PRIMARY KEY, v VARCHAR(64));
    "
    $MYSQL -Dapp_main -e "INSERT INTO t (v) SELECT 'seed' FROM seq_1_to_1000;"
    $MYSQL -e "TRUNCATE mysql.selective_log_events" 2>/dev/null || true
    rm -f /var/lib/mysql/selective_profile.json
}

set_scenario() {  # <enabled> <general_log> <schemas_filter> <output>
    $MYSQL -e "
        SET GLOBAL selective_log_enabled=$1;
        SET GLOBAL general_log=$2;
        SET GLOBAL selective_log_schemas_to_log='$3';
        SET GLOBAL selective_log_tables_to_log='';
        SET GLOBAL selective_log_output='$4';
        SET GLOBAL selective_log_log_file_path='/var/lib/mysql/selective_profile.json';
        SET GLOBAL selective_log_min_duration_ms=0;
    "
}

run_mix() {
    $SLAP --create-schema=app_main --no-drop --delimiter=';' \
          --query="$MIX" --concurrency="$CONCURRENCY" \
          --number-of-queries="$QUERIES" \
        | awk '/Average number of seconds/ {print $(NF-1)}'
}

scenario() {
    local name="$1"
    run_mix >/dev/null                      # aquecimento
    local secs
    secs=$(run_mix)
    local qps
    qps=$(awk -v q="$QUERIES" -v s="$secs" 'BEGIN {printf "%.0f", q/s}')
    echo "$name;$secs;$qps"
}

echo "== PARTE 1: capacidade (mix 83% SELECT / 17% DML, concurrency=$CONCURRENCY, $QUERIES queries) =="
echo "cenario;segundos;qps"
prepare

set_scenario OFF OFF '' FILE
scenario baseline

set_scenario OFF ON '' FILE
scenario general_log
$MYSQL -e "SET GLOBAL general_log=OFF"

# realista: só o DML do schema é auditado; SELECTs caem no caminho barato
set_scenario ON OFF 'app_main:dml' FILE
scenario sel_dml_file

set_scenario ON OFF 'app_main:dml' TABLE
scenario sel_dml_table

# pior caso: TUDO do schema logado (inclusive os 83% de SELECT)
set_scenario ON OFF 'app_main' FILE
scenario sel_all_file

echo ""
echo "== PARTE 2: sustentado ${SUSTAIN_SECS}s no cenário realista (app_main:dml, FILE) =="
prepare
set_scenario ON OFF 'app_main:dml' FILE

status_num() {
    $MYSQL -N -e "SHOW GLOBAL STATUS LIKE '$1'" | awk '{print $2}'
}

sample() {
    local t=$1
    local rss_kb file_kb logged failures errors
    rss_kb=$(awk '/VmRSS/ {print $2}' /proc/1/status)
    file_kb=$(du -k /var/lib/mysql/selective_profile.json 2>/dev/null | cut -f1 || echo 0)
    logged=$(status_num Selective_log_events_logged)
    failures=$(status_num Selective_log_write_failures)
    errors=$(status_num Selective_log_callback_errors)
    echo "t=${t}s rss_mb=$((rss_kb/1024)) log_kb=${file_kb} logged=${logged} write_failures=${failures} callback_errors=${errors}"
}

END=$(( $(date +%s) + SUSTAIN_SECS ))
NEXT_SAMPLE=$(date +%s)
T0=$(date +%s)
TOTAL_QUERIES=0

sample 0
while [ "$(date +%s)" -lt "$END" ]; do
    $SLAP --create-schema=app_main --no-drop --delimiter=';' \
          --query="$MIX" --concurrency="$CONCURRENCY" \
          --number-of-queries="$QUERIES" >/dev/null
    TOTAL_QUERIES=$((TOTAL_QUERIES + QUERIES))
    NOW=$(date +%s)
    if [ "$NOW" -ge "$((NEXT_SAMPLE + 30))" ]; then
        NEXT_SAMPLE=$NOW
        sample $((NOW - T0))
    fi
done
ELAPSED=$(( $(date +%s) - T0 ))
sample "$ELAPSED"

echo ""
echo "== RESUMO SUSTENTADO =="
echo "duracao=${ELAPSED}s queries_totais=${TOTAL_QUERIES} qps_medio=$((TOTAL_QUERIES/ELAPSED))"
echo "eventos_logados=$(status_num Selective_log_events_logged) (esperado ~1/6 das queries)"
echo "drops=$(status_num Selective_log_events_dropped) write_failures=$(status_num Selective_log_write_failures) callback_errors=$(status_num Selective_log_callback_errors)"
ls -la /var/lib/mysql/selective_profile.json

set_scenario OFF OFF '' FILE
