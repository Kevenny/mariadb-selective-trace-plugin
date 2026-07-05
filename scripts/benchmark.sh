#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# benchmark.sh — Etapa 5: overhead do selective_trace vs general_log
#
# Roda DENTRO do container mariadb-plugin-test (imagem oficial 11.4.4):
#   docker exec mariadb-plugin-test bash /scripts/benchmark.sh
# ou copie e execute. Usa mariadb-slap com uma carga mista INSERT+SELECT.
#
# Scenarios:
#   1 baseline      — selective_trace OFF, general_log OFF
#   2 general_log   — general_log=ON (arquivo)
#   3 sel_miss      — selective_trace ON filtrando bench_hot, carga em bench_cold
#                     ("no-log" path: only the filter cost)
#   4 sel_hit_file  — selective_trace ON, carga em bench_hot, output=FILE
#   5 sel_hit_table — selective_trace ON, carga em bench_hot, output=TABLE
# ---------------------------------------------------------------------------
set -euo pipefail

MYSQL="mariadb -uroot -p${MARIADB_ROOT_PASSWORD:-devpassword}"
SLAP="mariadb-slap -uroot -p${MARIADB_ROOT_PASSWORD:-devpassword}"

CONCURRENCY="${BENCH_CONCURRENCY:-8}"
QUERIES="${BENCH_QUERIES:-20000}"
# 50/50 mix with an indexed SELECT: constant per-query cost, does not grow
# com o tamanho da tabela (evita confundir overhead do log com scan maior)
QUERY_MIX="INSERT INTO t (v) VALUES ('benchmark-row-payload-0123456789');SELECT v FROM t WHERE id = 50"

prepare() {
    $MYSQL -e "
        CREATE DATABASE IF NOT EXISTS bench_hot;
        CREATE DATABASE IF NOT EXISTS bench_cold;
        DROP TABLE IF EXISTS bench_hot.t;  CREATE TABLE bench_hot.t  (id INT AUTO_INCREMENT PRIMARY KEY, v VARCHAR(64));
        DROP TABLE IF EXISTS bench_cold.t; CREATE TABLE bench_cold.t (id INT AUTO_INCREMENT PRIMARY KEY, v VARCHAR(64));
    "
}

# set_config <enabled> <general_log> <schemas_filter> <output>
set_config() {
    $MYSQL -e "
        SET GLOBAL selective_trace_enabled=$1;
        SET GLOBAL general_log=$2;
        SET GLOBAL selective_trace_schemas_to_log='$3';
        SET GLOBAL selective_trace_tables_to_log='';
        SET GLOBAL selective_trace_output='$4';
        SET GLOBAL selective_trace_min_duration_ms=0;
    "
}

# run_slap <schema>  -> prints seconds (average to run all queries)
run_slap() {
    $SLAP --create-schema="$1" --no-drop \
          --delimiter=';' --query="$QUERY_MIX" \
          --concurrency="$CONCURRENCY" --number-of-queries="$QUERIES" \
        | awk '/Average number of seconds/ {print $(NF-1)}'
}

scenario() {
    local name="$1" schema="$2"
    # tables always in the same initial state across scenarios
    $MYSQL -Dbench_hot -e "TRUNCATE TABLE bench_hot.t; TRUNCATE TABLE bench_cold.t;
               INSERT INTO bench_hot.t (v)  SELECT 'seed' FROM seq_1_to_1000;
               INSERT INTO bench_cold.t (v) SELECT 'seed' FROM seq_1_to_1000;"
    # warm-up + measurement
    run_slap "$schema" >/dev/null
    local secs
    secs=$(run_slap "$schema")
    echo "$name;$secs"
}

# --------------------------------------------------------------------------
# "light" suite: DO 1 (no table, minimal execution cost) — isolates the log
# path cost, which becomes proportionally visible.
# --------------------------------------------------------------------------
LIGHT_CONCURRENCY="${BENCH_LIGHT_CONCURRENCY:-32}"
LIGHT_QUERIES="${BENCH_LIGHT_QUERIES:-60000}"

run_slap_light() {
    $SLAP --create-schema="$1" --no-drop \
          --delimiter=';' --query="DO 1" \
          --concurrency="$LIGHT_CONCURRENCY" \
          --number-of-queries="$LIGHT_QUERIES" \
        | awk '/Average number of seconds/ {print $(NF-1)}'
}

scenario_light() {
    local name="$1" schema="$2"
    run_slap_light "$schema" >/dev/null
    local secs
    secs=$(run_slap_light "$schema")
    echo "$name;$secs"
}

prepare

echo "== suite MIX: scenario;seconds (concurrency=$CONCURRENCY, queries=$QUERIES, INSERT+SELECT) =="

set_config OFF OFF '' FILE
scenario baseline bench_cold

set_config OFF ON '' FILE
scenario general_log bench_cold
$MYSQL -e "SET GLOBAL general_log=OFF"

set_config ON OFF 'bench_hot' FILE
scenario sel_miss bench_cold

set_config ON OFF 'bench_hot' FILE
scenario sel_hit_file bench_hot

set_config ON OFF 'bench_hot' TABLE
scenario sel_hit_table bench_hot

echo "== suite LIGHT: scenario;seconds (concurrency=$LIGHT_CONCURRENCY, queries=$LIGHT_QUERIES, DO 1) =="

set_config OFF OFF '' FILE
scenario_light baseline_l bench_cold

set_config OFF ON '' FILE
scenario_light general_log_l bench_cold
$MYSQL -e "SET GLOBAL general_log=OFF"

# session on bench_cold, filter bench_hot => "no-log" path
set_config ON OFF 'bench_hot' FILE
scenario_light sel_miss_l bench_cold

# session on bench_hot (session schema matches) => logs every DO 1
set_config ON OFF 'bench_hot' FILE
scenario_light sel_hit_file_l bench_hot

set_config ON OFF 'bench_hot' TABLE
scenario_light sel_hit_table_l bench_hot

set_config OFF OFF '' FILE
echo "== status =="
$MYSQL -e "SHOW GLOBAL STATUS LIKE 'selective_trace%'"
