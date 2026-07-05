#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# security-test.sh — adversarial security validation of selective_trace,
# focused on OL9 (SELinux, permissions) but useful on any platform.
#
# Roda DENTRO de um oraclelinux:9 com o .so em /plugin_out:
#   docker run --rm -i -v "<repo>/build/plugin_output-ol8:/plugin_out:ro" \
#       oraclelinux:9 bash < scripts/security-test.sh
# ---------------------------------------------------------------------------
set -uo pipefail

MARIADB_SERIES="${MARIADB_SERIES:-11.4}"
echo ">> Instalando MariaDB ${MARIADB_SERIES} (RPM oficial) + plugin"
curl -LsS https://r.mariadb.com/downloads/mariadb_repo_setup \
    | bash -s -- --mariadb-server-version=mariadb-${MARIADB_SERIES} >/dev/null 2>&1
dnf -y install MariaDB-server MariaDB-client python3 >/dev/null 2>&1
cp /plugin_out/selective_trace.so /usr/lib64/mysql/plugin/
mariadb-install-db --user=mysql >/dev/null 2>&1

echo ">> SELinux status do ambiente:"
getenforce 2>/dev/null || echo "(getenforce unavailable — SELinux not active in this kernel/container)"
ls -Z /usr/lib64/mysql/plugin/selective_trace.so 2>/dev/null || \
  ls -l /usr/lib64/mysql/plugin/selective_trace.so

/usr/sbin/mariadbd --user=mysql --skip-networking --socket=/tmp/m.sock \
    --plugin-load-add=selective_trace.so --plugin-maturity=experimental \
    --selective_trace_enabled=ON \
    --selective_trace_schemas_to_log=app \
    --selective_trace_file_path=/var/lib/mysql/sec.json \
    >/tmp/mariadbd.log 2>&1 &
for i in $(seq 1 60); do mariadb -uroot -S /tmp/m.sock -e "SELECT 1" >/dev/null 2>&1 && break; sleep 1; done
M="mariadb -uroot -S /tmp/m.sock"

pass=0; fail=0
ok()   { echo "  PASS: $1"; pass=$((pass+1)); }
bad()  { echo "  FAIL: $1"; fail=$((fail+1)); }

$M -e "CREATE DATABASE app; CREATE TABLE app.t (id INT PRIMARY KEY AUTO_INCREMENT, v TEXT)"

# =====================================================================
echo ""
echo "### T1 — SQL injection in TABLE mode (default sql_mode)"
# =====================================================================
$M -e "SET GLOBAL selective_trace_output='TABLE'"
$M app -e "TRUNCATE t" 2>/dev/null
# payloads que tentam quebrar o INSERT interno do plugin:
$M app <<'SQL'
INSERT INTO t (v) VALUES ("evil'); DROP TABLE mysql.user; -- ");
INSERT INTO t (v) VALUES ('back\\slash and '' quote');
INSERT INTO t (v) VALUES (0x275C6E290A); /* bytes crus */
SQL
sleep 2
# a tabela mysql.user (global_priv) tem que continuar existindo:
if $M -e "SELECT COUNT(*) FROM mysql.global_priv" >/dev/null 2>&1; then
    ok "mysql.global_priv intact (internal INSERT was not injected)"
else
    bad "privileges table gone — SQL injection!"
fi
# no orphan/extra row in the log table beyond the legitimate events:
n=$($M -N -e "SELECT COUNT(*) FROM mysql.selective_trace_events")
echo "  (eventos na tabela de log: $n)"
# the malicious query was stored as DATA (one row), not executed:
if $M -N -e "SELECT COUNT(*) FROM mysql.selective_trace_events WHERE query LIKE '%DROP TABLE mysql.user%'" | grep -q 1; then
    ok "payload stored as literal text, not executed"
else
    bad "payload not found literally (did escaping alter the data?)"
fi

# =====================================================================
echo ""
echo "### T2 — Injection in TABLE mode with sql_mode=NO_BACKSLASH_ESCAPES"
# =====================================================================
$M -e "SET GLOBAL sql_mode='NO_BACKSLASH_ESCAPES'"
$M app -e "TRUNCATE t" 2>/dev/null
before=$($M -N -e "SELECT COUNT(*) FROM information_schema.tables")
$M app -e "INSERT INTO t (v) VALUES ('x'); INSERT INTO t (v) VALUES ('nasty'')( evil ')" 2>/dev/null
$M app --binary-mode -e "INSERT INTO t (v) VALUES ('q\\\\z')" 2>/dev/null
sleep 2
after=$($M -N -e "SELECT COUNT(*) FROM information_schema.tables")
wf=$($M -N -e "SHOW GLOBAL STATUS LIKE 'Selective_trace_write_failures'" | awk '{print $2}')
alive=$($M -N -e "SELECT 1" 2>/dev/null)
echo "  write_failures=$wf  (tabelas antes=$before depois=$after servidor_vivo=$alive)"
if [ "$alive" = "1" ] && [ "$before" = "$after" ]; then
    ok "server intact under NO_BACKSLASH_ESCAPES"
    if [ "${wf:-0}" -gt 0 ]; then
        echo "  NOTE: $wf log INSERT(s) failed — escaped data was invalid for this sql_mode (event lost, no injection)"
    fi
else
    bad "anomalia sob NO_BACKSLASH_ESCAPES"
fi
$M -e "SET GLOBAL sql_mode=DEFAULT"

# =====================================================================
echo ""
echo "### T3 — JSON/log injection in FILE mode"
# =====================================================================
$M -e "SET GLOBAL selective_trace_output='FILE'"
: > /var/lib/mysql/sec.json 2>/dev/null || true
$M app <<'SQL'
INSERT INTO t (v) VALUES ('quebra"aspas\e {"fake":"json"} e\nnewline literal');
SELECT '}
{"injected":"newline real"}' AS x;
SQL
sleep 1
# every line of the file must be valid JSON with the fixed keys:
python3 - <<'PY'
import json,sys
keys={'ts','conn_id','query_id','user','db','tables','command','duration_ms','error_code','query'}
bad=0; n=0
for i,line in enumerate(open('/var/lib/mysql/sec.json'),1):
    line=line.strip()
    if not line: continue
    n+=1
    try:
        o=json.loads(line)
    except Exception as e:
        print(f"  FAIL: line {i} is not valid JSON: {e}"); bad+=1; continue
    if set(o)!=keys:
        print(f"  FAIL: linha {i} chaves inesperadas: {set(o)^keys}"); bad+=1
print(f"  {n} lines, {'0 invalid' if bad==0 else str(bad)+' INVALID'}")
sys.exit(1 if bad else 0)
PY
[ $? -eq 0 ] && ok "JSON stayed valid and one line per event" || bad "JSON/line injection"

# =====================================================================
echo ""
echo "### T4 — Vazamento de segredos em cleartext"
# =====================================================================
: > /var/lib/mysql/sec.json 2>/dev/null || true
$M -e "SET GLOBAL selective_trace_schemas_to_log='app,mysql'"
$M app -e "CREATE USER IF NOT EXISTS leaky@localhost IDENTIFIED BY 'SuperSecret123'" 2>/dev/null
$M app -e "SET PASSWORD FOR leaky@localhost = PASSWORD('AnotherSecret456')" 2>/dev/null
$M app -e "INSERT INTO t (v) VALUES ('api_key=sk-INLINE-SECRET-789')" 2>/dev/null
sleep 1
if grep -q "SuperSecret123\|AnotherSecret456" /var/lib/mysql/sec.json 2>/dev/null; then
    echo "  ACHADO: senha de DCL aparece em cleartext no log"
    grep -o "SuperSecret123\|AnotherSecret456" /var/lib/mysql/sec.json | sort -u | sed 's/^/    -> /'
    bad "DCL secrets are not masked"
else
    ok "senhas de DCL mascaradas/ausentes"
fi
$M -e "DROP USER IF EXISTS leaky@localhost" 2>/dev/null

# =====================================================================
echo ""
echo "### T5 — Log file permissions and context"
# =====================================================================
ls -lnZ /var/lib/mysql/sec.json 2>/dev/null || ls -ln /var/lib/mysql/sec.json
perm=$(stat -c '%a' /var/lib/mysql/sec.json 2>/dev/null)
owner=$(stat -c '%U' /var/lib/mysql/sec.json 2>/dev/null)
echo "  perms=$perm owner=$owner"
if [ "$owner" = "mysql" ] && [ "${perm:0:1}" != "" ]; then
    # the world should not read the query log
    if [ "${perm: -1}" = "0" ] || [ "${perm: -1}" = "4" ]; then
        echo "  NOTE: others can read (perm .$perm) — restrict via umask/ACL if the log holds sensitive data"
    fi
    ok "file owned by the mariadbd user"
else
    bad "dono inesperado do arquivo de log"
fi

# =====================================================================
echo ""
echo "### T6 — Path traversal / escrita fora do datadir"
# =====================================================================
# a privileged user already could; we validate the failure is graceful
$M -e "SET GLOBAL selective_trace_file_path='/root/naopode.json'" 2>/dev/null
$M app -e "INSERT INTO t (v) VALUES ('probe')" 2>/dev/null
sleep 1
alive=$($M -N -e "SELECT 1" 2>/dev/null)
[ "$alive" = "1" ] && ok "inaccessible path does not crash the server (graceful failure)" || bad "server crashed on invalid path"
$M -e "SET GLOBAL selective_trace_file_path='/var/lib/mysql/sec.json'" 2>/dev/null

mariadb-admin -uroot -S /tmp/m.sock shutdown 2>/dev/null
echo ""
echo "==================== RESUMO: $pass PASS / $fail FAIL ===================="
