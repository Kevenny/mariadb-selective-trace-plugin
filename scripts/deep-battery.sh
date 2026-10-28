#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# deep-battery.sh — adversarial workload for selective_trace, meant to run
# against a server started by asan-test.sh or valgrind-test.sh (any server
# with the plugin installable works).
#
# Environment:
#   CLIENT    mariadb client command incl. connection args (required)
#   DURATION  seconds of the concurrent phase (default 120)
#   PLUGIN_SO soname for INSTALL PLUGIN (default selective_trace.so)
#   LOGF      base path for FILE output (default /tmp/dt_selective.json)
#
# Phases:
#   0  schema setup incl. 61 tables with 64-char names (tables-buffer
#      truncation path) and nested stored procedures
#   1  single-session edge cases in FILE and TABLE mode: prepared
#      statements, routines, triggers, views, DDL, TCL, XA, HANDLER, LOAD
#      DATA, credentials (masking), errors, KILL, 1.5 MB statement, 500
#      statements on one connection, invalid SET GLOBAL values
#   2  concurrent storm: traced DML/DDL sessions, 61-table joins, bare
#      SHOW GLOBAL STATUS / I_S.GLOBAL_STATUS / SHOW VARIABLES monitors,
#      connection churn, sysvar churn (filters, output mode, bad file
#      paths, min_duration), FLUSH TABLES WITH READ LOCK, DROP of the log
#      table, enable toggles, and UNINSTALL/INSTALL cycles in both modes —
#      with connections opened while the plugin is uninstalled, which is
#      the v1.2.6 crash scenario.
#   3  final state and counters
# Exits non-zero if the server stops answering at any point.
# ---------------------------------------------------------------------------
set -u
CLIENT="${CLIENT:?set CLIENT to the mariadb client command}"
DURATION="${DURATION:-120}"
PLUGIN_SO="${PLUGIN_SO:-selective_trace.so}"
LOGF="${LOGF:-/tmp/dt_selective.json}"
LONGDB=dt_long_schema_name_exactly_sixty_four_characters_long_abcdefghij

q() { $CLIENT --force -e "$1" 2>&1 | grep -v "^Deprecated" || true; }
alive() { $CLIENT -e "SELECT 1" >/dev/null 2>&1; }

echo "== phase 0: schema setup =="
q "CREATE DATABASE IF NOT EXISTS dt_hot; CREATE DATABASE IF NOT EXISTS dt_cold; CREATE DATABASE IF NOT EXISTS $LONGDB;"
gen=""
for i in $(seq 1 61); do
  n=$(printf 'tbl_%02d_%s' $i "$(printf 'x%.0s' $(seq 1 57))")
  gen="$gen CREATE TABLE IF NOT EXISTS $LONGDB.\`$n\` (id INT PRIMARY KEY, v INT); INSERT IGNORE INTO $LONGDB.\`$n\` VALUES (1,1);"
done
q "$gen"
join="SELECT COUNT(*) FROM "
for i in $(seq 1 61); do
  n=$(printf 'tbl_%02d_%s' $i "$(printf 'x%.0s' $(seq 1 57))")
  if [ $i -eq 1 ]; then join="$join $LONGDB.\`$n\` t1"; else join="$join JOIN $LONGDB.\`$n\` t$i USING (id)"; fi
done
echo "$join" > /tmp/dt_bigjoin.sql

q "CREATE TABLE IF NOT EXISTS dt_hot.t (id INT AUTO_INCREMENT PRIMARY KEY, v VARCHAR(200), b BLOB);
   CREATE TABLE IF NOT EXISTS dt_cold.t (id INT AUTO_INCREMENT PRIMARY KEY, v VARCHAR(200));
   CREATE OR REPLACE VIEW dt_hot.v AS SELECT id, v FROM dt_hot.t;
   CREATE OR REPLACE TRIGGER dt_hot.trg BEFORE INSERT ON dt_hot.t FOR EACH ROW SET NEW.v = CONCAT('trg:', NEW.v);
   SET GLOBAL selective_trace_enabled=ON;
   SET GLOBAL selective_trace_schemas='dt_hot,$LONGDB,mysql';
   SET GLOBAL selective_trace_tables='dt_cold.t:select';
   SET GLOBAL selective_trace_file_path='$LOGF';
   SET GLOBAL selective_trace_output='FILE';"
$CLIENT --force <<'SQL' >/dev/null 2>&1
DROP PROCEDURE IF EXISTS dt_hot.p_inner;
DROP PROCEDURE IF EXISTS dt_hot.p_outer;
DELIMITER //
CREATE PROCEDURE dt_hot.p_inner(IN n INT) BEGIN INSERT INTO dt_hot.t (v) VALUES (CONCAT('inner', n)); SELECT COUNT(*) FROM dt_cold.t; END//
CREATE PROCEDURE dt_hot.p_outer() BEGIN DECLARE i INT DEFAULT 0; WHILE i < 5 DO CALL dt_hot.p_inner(i); SET i = i + 1; END WHILE; END//
DELIMITER ;
SQL

echo "== phase 1: single-session edge cases (both modes) =="
for mode in FILE TABLE; do
  q "SET GLOBAL selective_trace_output='$mode';"
  q "USE dt_hot; INSERT INTO t (v) VALUES ('a'),('b'); SELECT * FROM v; CALL p_outer(); SELECT * FROM t JOIN dt_cold.t USING (id);
     PREPARE s FROM 'SELECT * FROM dt_hot.t WHERE id = ?'; SET @x=1; EXECUTE s USING @x; EXECUTE s USING @x; DEALLOCATE PREPARE s;
     SELECT * FROM dt_hot.does_not_exist; SELECT 1/0; SET SESSION sql_mode=''; SELECT CAST('x' AS SIGNED);
     INSERT INTO dt_hot.t (v) VALUES (REPEAT('\xF0\x9F\x98\x80', 40)); UPDATE dt_hot.t SET v='\"\\\\\n\r\t\0\x1a' WHERE id=1;
     CREATE USER IF NOT EXISTS dt_u@localhost IDENTIFIED BY 'secretpw'; SET PASSWORD FOR dt_u@localhost = PASSWORD('s2'); ALTER USER dt_u@localhost IDENTIFIED VIA mysql_native_password USING PASSWORD('s3'); DROP USER dt_u@localhost;
     START TRANSACTION; INSERT INTO dt_hot.t (v) VALUES ('tx'); SAVEPOINT sp1; ROLLBACK TO sp1; COMMIT; BEGIN; DELETE FROM dt_hot.t WHERE id > 100000; ROLLBACK;
     LOCK TABLES dt_hot.t WRITE; INSERT INTO dt_hot.t (v) VALUES ('locked'); UNLOCK TABLES;
     ALTER TABLE dt_hot.t ADD COLUMN c INT; ALTER TABLE dt_hot.t DROP COLUMN c; RENAME TABLE dt_hot.t TO dt_hot.t_r; RENAME TABLE dt_hot.t_r TO dt_hot.t; TRUNCATE TABLE dt_cold.t;
     ANALYZE TABLE dt_hot.t; OPTIMIZE TABLE dt_hot.t; CHECK TABLE dt_hot.t; FLUSH TABLES; FLUSH STATUS;
     SHOW GLOBAL STATUS; SHOW STATUS LIKE 'selective%'; SHOW GLOBAL VARIABLES LIKE 'selective%'; SHOW PROCESSLIST; SHOW ENGINE INNODB STATUS; SHOW PLUGINS;
     SELECT * FROM information_schema.GLOBAL_STATUS WHERE VARIABLE_NAME LIKE 'selective%'; SELECT * FROM information_schema.PLUGINS WHERE PLUGIN_NAME='selective_trace';
     /*!40000 ALTER TABLE dt_hot.t DISABLE KEYS */; /*M!100000 SELECT 1 */; -- c
     SELECT 2; # c
     SELECT 3; (SELECT 4) UNION (SELECT 5); WITH c AS (SELECT 1 x) SELECT * FROM c; INSERT INTO dt_hot.t (v) SELECT v FROM dt_hot.t LIMIT 3;
     LOAD DATA LOCAL INFILE '/etc/hostname' INTO TABLE dt_cold.t (v); XA START 'xa1'; INSERT INTO dt_hot.t (v) VALUES ('xa'); XA END 'xa1'; XA ROLLBACK 'xa1';
     HANDLER dt_hot.t OPEN; HANDLER dt_hot.t READ FIRST; HANDLER dt_hot.t CLOSE;
     SET STATEMENT max_statement_time=1 FOR SELECT SLEEP(0.01); DO SLEEP(0.01); SELECT BENCHMARK(1000, MD5('x'));
     KILL QUERY CONNECTION_ID();
     SELECT 'huge', REPEAT('q', 1500000); "
  $CLIENT --force -e "$(cat /tmp/dt_bigjoin.sql)" >/dev/null 2>&1
  for i in $(seq 1 500); do echo "INSERT INTO dt_hot.t (v) VALUES ('m$i'); SELECT COUNT(*) FROM dt_hot.t;"; done | $CLIENT --force dt_hot >/dev/null 2>&1
  q "SET GLOBAL selective_trace_min_duration_ms=50; SELECT SLEEP(0.06); SELECT 1; SET GLOBAL selective_trace_min_duration_ms=0;
     SET GLOBAL selective_trace_mask_passwords=OFF; CREATE USER IF NOT EXISTS dt_u2@localhost IDENTIFIED BY 'plain'; DROP USER dt_u2@localhost; SET GLOBAL selective_trace_mask_passwords=ON;
     SET GLOBAL selective_trace_connections=CONNECTION_ID(); SELECT 'conn-traced'; SET GLOBAL selective_trace_connections='';
     SET GLOBAL selective_trace_schemas='bad.name'; SET GLOBAL selective_trace_tables='nodot'; SET GLOBAL selective_trace_tables='a.b:badcmd'; SET GLOBAL selective_trace_connections='x';
     SET GLOBAL selective_trace_schemas='dt_hot:insert|update,dt_cold:dml,\`Dt_Hot\`:ddl'; SET GLOBAL selective_trace_tables='dt_cold.t:select,dt_hot.*:tcl,\`dt_hot\`.\`t\`:all';
     SET GLOBAL selective_trace_schemas='dt_hot,$LONGDB,mysql'; SET GLOBAL selective_trace_tables='dt_cold.t:select';"
  alive || { echo "!! SERVER DEAD after phase 1 ($mode)"; exit 1; }
  echo "   phase 1 ($mode) ok"
done

echo "== phase 2: concurrent storm for ${DURATION}s =="
END=$(( $(date +%s) + DURATION ))
pids=()
for i in $(seq 1 8); do
  ( while [ $(date +%s) -lt $END ]; do
      $CLIENT --force dt_hot -e "INSERT INTO t (v) VALUES ('s$i'); SELECT COUNT(*) FROM t JOIN dt_cold.t USING (id); CALL p_outer(); UPDATE t SET v='u' WHERE id=$i; DELETE FROM t WHERE id > 5000; SELECT * FROM does_not_exist;" >/dev/null 2>&1
    done ) & pids+=($!)
done
for i in 1 2 3; do
  ( while [ $(date +%s) -lt $END ]; do $CLIENT --force -e "$(cat /tmp/dt_bigjoin.sql)" >/dev/null 2>&1; done ) & pids+=($!)
done
for i in $(seq 1 6); do
  ( while [ $(date +%s) -lt $END ]; do $CLIENT -e "SHOW GLOBAL STATUS; SELECT * FROM information_schema.GLOBAL_STATUS; SHOW GLOBAL VARIABLES; SHOW STATUS LIKE 'selective%'" >/dev/null 2>&1; done ) & pids+=($!)
done
( while [ $(date +%s) -lt $END ]; do for k in 1 2 3 4 5; do $CLIENT dt_hot -e "PREPARE s FROM 'SELECT * FROM t WHERE id=?'; SET @a=1; EXECUTE s USING @a;" >/dev/null 2>&1 & done; wait; done ) & pids+=($!)
( n=0; while [ $(date +%s) -lt $END ]; do n=$((n+1))
    $CLIENT --force -e "SET GLOBAL selective_trace_output='TABLE'" >/dev/null 2>&1; sleep 0.5
    $CLIENT --force -e "SET GLOBAL selective_trace_schemas='dt_hot:dml|select,dt_cold'; SET GLOBAL selective_trace_tables='dt_cold.t,dt_hot.*:ddl'; SET GLOBAL selective_trace_min_duration_ms=$((n % 3)); SET GLOBAL selective_trace_file_path='/nonexistent/x$n.json'" >/dev/null 2>&1; sleep 0.3
    $CLIENT --force -e "SET GLOBAL selective_trace_output='FILE'; SET GLOBAL selective_trace_file_path='${LOGF}.$((n % 4))'; SET GLOBAL selective_trace_schemas='dt_hot,$LONGDB,mysql'; SET GLOBAL selective_trace_tables='dt_cold.t:select'" >/dev/null 2>&1; sleep 0.5
    $CLIENT --force -e "SET GLOBAL selective_trace_output='TABLE'; FLUSH TABLES WITH READ LOCK; SELECT SLEEP(0.3); UNLOCK TABLES; DROP TABLE IF EXISTS mysql.selective_trace_events; SELECT SLEEP(0.2); SET GLOBAL selective_trace_enabled=OFF; SET GLOBAL selective_trace_enabled=ON" >/dev/null 2>&1; sleep 0.3
  done ) & pids+=($!)
# UNINSTALL/INSTALL cycles; between them, connections are opened and kept
# alive so that they run their next statement only after the re-INSTALL
( c=0; while [ $(date +%s) -lt $END ]; do c=$((c+1)); sleep 6
    $CLIENT --force -e "UNINSTALL PLUGIN selective_trace" >/dev/null 2>&1
    $CLIENT -e "SELECT SLEEP(1.5); SHOW GLOBAL STATUS; SELECT COUNT(*) FROM dt_hot.t" >/dev/null 2>&1 &
    $CLIENT dt_hot -e "SELECT SLEEP(1.5); INSERT INTO t (v) VALUES ('window'); SELECT * FROM t LIMIT 1" >/dev/null 2>&1 &
    sleep 0.3
    $CLIENT --force -e "INSTALL PLUGIN selective_trace SONAME '$PLUGIN_SO'; SET GLOBAL selective_trace_enabled=ON; SET GLOBAL selective_trace_schemas='dt_hot,$LONGDB,mysql'; SET GLOBAL selective_trace_tables='dt_cold.t:select'; SET GLOBAL selective_trace_file_path='$LOGF'; SET GLOBAL selective_trace_output='$( [ $((c % 2)) -eq 0 ] && echo TABLE || echo FILE)'" >/dev/null 2>&1
    wait
    echo "   uninstall/install cycle $c done"
  done ) & pids+=($!)
while [ $(date +%s) -lt $END ]; do sleep 5; alive || { echo "!! SERVER DEAD during storm"; exit 1; }; done
wait "${pids[@]}" 2>/dev/null
alive || { echo "!! SERVER DEAD after storm"; exit 1; }

echo "== phase 3: final state =="
q "INSTALL PLUGIN selective_trace SONAME '$PLUGIN_SO'"
q "SHOW GLOBAL STATUS LIKE 'selective%'; SELECT COUNT(*) AS table_rows FROM mysql.selective_trace_events; SELECT PLUGIN_STATUS FROM information_schema.PLUGINS WHERE PLUGIN_NAME='selective_trace'"
ls -la ${LOGF}* 2>/dev/null | head
echo "battery finished, server alive"
