#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# security-test.sh — validação de segurança adversarial do selective_log,
# focada em OL9 (SELinux, permissões) mas útil em qualquer plataforma.
#
# Roda DENTRO de um oraclelinux:9 com o .so em /plugin_out:
#   docker run --rm -i -v "<repo>/build/plugin_output-ol8:/plugin_out:ro" \
#       oraclelinux:9 bash < scripts/security-test.sh
# ---------------------------------------------------------------------------
set -uo pipefail

echo ">> Instalando MariaDB 11.4 (RPM oficial) + plugin"
curl -LsS https://r.mariadb.com/downloads/mariadb_repo_setup \
    | bash -s -- --mariadb-server-version=mariadb-11.4 >/dev/null 2>&1
dnf -y install MariaDB-server MariaDB-client python3 >/dev/null 2>&1
cp /plugin_out/selective_log.so /usr/lib64/mysql/plugin/
mariadb-install-db --user=mysql >/dev/null 2>&1

echo ">> SELinux status do ambiente:"
getenforce 2>/dev/null || echo "(getenforce indisponível — SELinux não ativo neste kernel/container)"
ls -Z /usr/lib64/mysql/plugin/selective_log.so 2>/dev/null || \
  ls -l /usr/lib64/mysql/plugin/selective_log.so

/usr/sbin/mariadbd --user=mysql --skip-networking --socket=/tmp/m.sock \
    --plugin-load-add=selective_log.so --plugin-maturity=experimental \
    --selective_log_enabled=ON \
    --selective_log_schemas_to_log=app \
    --selective_log_log_file_path=/var/lib/mysql/sec.json \
    >/tmp/mariadbd.log 2>&1 &
for i in $(seq 1 60); do mariadb -uroot -S /tmp/m.sock -e "SELECT 1" >/dev/null 2>&1 && break; sleep 1; done
M="mariadb -uroot -S /tmp/m.sock"

pass=0; fail=0
ok()   { echo "  PASS: $1"; pass=$((pass+1)); }
bad()  { echo "  FAIL: $1"; fail=$((fail+1)); }

$M -e "CREATE DATABASE app; CREATE TABLE app.t (id INT PRIMARY KEY AUTO_INCREMENT, v TEXT)"

# =====================================================================
echo ""
echo "### T1 — Injeção de SQL no modo TABLE (sql_mode default)"
# =====================================================================
$M -e "SET GLOBAL selective_log_output='TABLE'"
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
    ok "mysql.global_priv intacta (INSERT interno não foi injetado)"
else
    bad "tabela de privilégios sumiu — injeção de SQL!"
fi
# nenhuma linha órfã/extra na tabela de log além dos eventos legítimos:
n=$($M -N -e "SELECT COUNT(*) FROM mysql.selective_log_events")
echo "  (eventos na tabela de log: $n)"
# a query maliciosa foi gravada como DADO (uma linha), não executada:
if $M -N -e "SELECT COUNT(*) FROM mysql.selective_log_events WHERE query LIKE '%DROP TABLE mysql.user%'" | grep -q 1; then
    ok "payload gravado como texto literal, não executado"
else
    bad "payload não encontrado literalmente (escaping alterou o dado?)"
fi

# =====================================================================
echo ""
echo "### T2 — Injeção no modo TABLE com sql_mode=NO_BACKSLASH_ESCAPES"
# =====================================================================
$M -e "SET GLOBAL sql_mode='NO_BACKSLASH_ESCAPES'"
$M app -e "TRUNCATE t" 2>/dev/null
before=$($M -N -e "SELECT COUNT(*) FROM information_schema.tables")
$M app -e "INSERT INTO t (v) VALUES ('x'); INSERT INTO t (v) VALUES ('nasty'')( evil ')" 2>/dev/null
$M app --binary-mode -e "INSERT INTO t (v) VALUES ('q\\\\z')" 2>/dev/null
sleep 2
after=$($M -N -e "SELECT COUNT(*) FROM information_schema.tables")
wf=$($M -N -e "SHOW GLOBAL STATUS LIKE 'Selective_log_write_failures'" | awk '{print $2}')
alive=$($M -N -e "SELECT 1" 2>/dev/null)
echo "  write_failures=$wf  (tabelas antes=$before depois=$after servidor_vivo=$alive)"
if [ "$alive" = "1" ] && [ "$before" = "$after" ]; then
    ok "servidor íntegro sob NO_BACKSLASH_ESCAPES"
    if [ "${wf:-0}" -gt 0 ]; then
        echo "  NOTA: $wf INSERT(s) de log falharam — dado escapado ficou inválido para esse sql_mode (evento perdido, sem injeção)"
    fi
else
    bad "anomalia sob NO_BACKSLASH_ESCAPES"
fi
$M -e "SET GLOBAL sql_mode=DEFAULT"

# =====================================================================
echo ""
echo "### T3 — Injeção de JSON/log no modo FILE"
# =====================================================================
$M -e "SET GLOBAL selective_log_output='FILE'"
: > /var/lib/mysql/sec.json 2>/dev/null || true
$M app <<'SQL'
INSERT INTO t (v) VALUES ('quebra"aspas\e {"fake":"json"} e\nnewline literal');
SELECT '}
{"injected":"newline real"}' AS x;
SQL
sleep 1
# toda linha do arquivo tem que ser JSON válido e as chaves fixas:
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
        print(f"  FAIL: linha {i} não é JSON válido: {e}"); bad+=1; continue
    if set(o)!=keys:
        print(f"  FAIL: linha {i} chaves inesperadas: {set(o)^keys}"); bad+=1
print(f"  {n} linhas, {'0 inválidas' if bad==0 else str(bad)+' INVÁLIDAS'}")
sys.exit(1 if bad else 0)
PY
[ $? -eq 0 ] && ok "JSON permaneceu válido e uma linha por evento" || bad "injeção de JSON/linha"

# =====================================================================
echo ""
echo "### T4 — Vazamento de segredos em cleartext"
# =====================================================================
: > /var/lib/mysql/sec.json 2>/dev/null || true
$M -e "SET GLOBAL selective_log_schemas_to_log='app,mysql'"
$M app -e "CREATE USER IF NOT EXISTS leaky@localhost IDENTIFIED BY 'SuperSecret123'" 2>/dev/null
$M app -e "SET PASSWORD FOR leaky@localhost = PASSWORD('AnotherSecret456')" 2>/dev/null
$M app -e "INSERT INTO t (v) VALUES ('api_key=sk-INLINE-SECRET-789')" 2>/dev/null
sleep 1
if grep -q "SuperSecret123\|AnotherSecret456" /var/lib/mysql/sec.json 2>/dev/null; then
    echo "  ACHADO: senha de DCL aparece em cleartext no log"
    grep -o "SuperSecret123\|AnotherSecret456" /var/lib/mysql/sec.json | sort -u | sed 's/^/    -> /'
    bad "segredos de DCL não são mascarados"
else
    ok "senhas de DCL mascaradas/ausentes"
fi
$M -e "DROP USER IF EXISTS leaky@localhost" 2>/dev/null

# =====================================================================
echo ""
echo "### T5 — Permissões do arquivo de log e contexto"
# =====================================================================
ls -lnZ /var/lib/mysql/sec.json 2>/dev/null || ls -ln /var/lib/mysql/sec.json
perm=$(stat -c '%a' /var/lib/mysql/sec.json 2>/dev/null)
owner=$(stat -c '%U' /var/lib/mysql/sec.json 2>/dev/null)
echo "  permissões=$perm dono=$owner"
if [ "$owner" = "mysql" ] && [ "${perm:0:1}" != "" ]; then
    # o mundo não deveria ler o log de queries
    if [ "${perm: -1}" = "0" ] || [ "${perm: -1}" = "4" ]; then
        echo "  NOTA: outros têm leitura (perm .$perm) — restringir via umask/ACL se o log contém dados sensíveis"
    fi
    ok "arquivo pertence ao usuário do mariadbd"
else
    bad "dono inesperado do arquivo de log"
fi

# =====================================================================
echo ""
echo "### T6 — Path traversal / escrita fora do datadir"
# =====================================================================
# usuário com privilégio já poderia; validamos que a falha é graciosa
$M -e "SET GLOBAL selective_log_log_file_path='/root/naopode.json'" 2>/dev/null
$M app -e "INSERT INTO t (v) VALUES ('probe')" 2>/dev/null
sleep 1
alive=$($M -N -e "SELECT 1" 2>/dev/null)
[ "$alive" = "1" ] && ok "path inacessível não derruba o servidor (falha graciosa)" || bad "servidor caiu ao usar path inválido"
$M -e "SET GLOBAL selective_log_log_file_path='/var/lib/mysql/sec.json'" 2>/dev/null

mariadb-admin -uroot -S /tmp/m.sock shutdown 2>/dev/null
echo ""
echo "==================== RESUMO: $pass PASS / $fail FAIL ===================="
