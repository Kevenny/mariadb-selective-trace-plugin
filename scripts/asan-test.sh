#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# asan-test.sh — builds selective_trace with AddressSanitizer +
# UndefinedBehaviorSanitizer, starts the dev-built mariadbd with libasan
# preloaded, runs scripts/deep-battery.sh against it and reports every
# sanitizer finding.
#
# Runs INSIDE the development container (mariadb-plugin-dev), which has the
# mariadbd built with symbols in /opt/mariadb-build:
#   docker exec -i mariadb-plugin-dev bash < scripts/asan-test.sh
#   DURATION=300 docker exec -i mariadb-plugin-dev bash < scripts/asan-test.sh
#
# Why not only Valgrind: the plugin's own loads/stores are instrumented, so
# ASan catches use-after-free and overflows that only manifest under
# concurrency (Valgrind serializes threads), and UBSan catches NULL
# dereferences and signed overflows that a release build turns into a
# SIGSEGV much later. This harness found the v1.2.6 bugs (NULL THDVAR slot
# after UNINSTALL/INSTALL; sysvar storage freed while still readable).
#
# mariadbd itself is not instrumented: only bugs in the plugin's code are
# reported. server_audit is loaded alongside to mirror a typical production
# plugin set.
# ---------------------------------------------------------------------------
set -u
BUILD_DIR="${BUILD_DIR:-/opt/mariadb-build}"
SRC_DIR="${MARIADB_SRC_DIR:-/opt/mariadb-src}"
WORKSPACE="${WORKSPACE_DIR:-/workspace}"
DURATION="${DURATION:-150}"
OUT=/tmp/asan_plugin
DATADIR=/tmp/asandata
SOCK=/tmp/asan.sock
LIBASAN="${LIBASAN:-$(gcc -print-file-name=libasan.so)}"

echo ">> [1/4] Building the plugin with -fsanitize=address,undefined"
mkdir -p "$OUT"
cd "$BUILD_DIR"
CMD=$(ninja -t commands selective_trace | grep -m1 "selective_trace.cc")
INC=$(echo "$CMD" | tr ' ' '\n' | grep -E '^-(I|D)' | grep -v '_EXPORTS' | tr '\n' ' ')
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -O1 -g"
for f in selective_trace filter_engine log_writer_file log_writer_table; do
  c++ -fPIC -std=gnu++11 -Wall -Wextra $SAN $INC -c "$WORKSPACE/src/$f.cc" -o "$OUT/$f.o"
done
# libmysqlservices provides the *_service symbols, exactly like MYSQL_ADD_PLUGIN
c++ -shared $SAN -o "$OUT/selective_trace.so" "$OUT"/*.o \
    "$BUILD_DIR/libservices/libmysqlservices.a" -ldl
ln -sf "$BUILD_DIR/plugin/server_audit/server_audit.so" "$OUT/server_audit.so"
nm -D "$OUT/selective_trace.so" | grep -q __asan || { echo "!! ASan runtime not linked"; exit 1; }

echo ">> [2/4] Starting mariadbd with libasan preloaded"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0:halt_on_error=0:log_path=/tmp/asan_report:handle_segv=0:handle_abort=0:alloc_dealloc_mismatch=0:new_delete_type_mismatch=0:check_initialization_order=0"
export UBSAN_OPTIONS="print_stacktrace=1:log_path=/tmp/ubsan_report:halt_on_error=0"
rm -rf "$DATADIR" /tmp/asan_report* /tmp/ubsan_report* /tmp/asan_mysqld.log /tmp/dt_selective.json*
mkdir -p "$DATADIR"
scripts/mariadb-install-db --no-defaults --srcdir="$SRC_DIR" --datadir="$DATADIR" \
    --auth-root-authentication-method=normal >/dev/null 2>&1
LD_PRELOAD="$LIBASAN" "$BUILD_DIR/sql/mariadbd" --no-defaults --datadir="$DATADIR" \
    --socket="$SOCK" --skip-networking --plugin-dir="$OUT" \
    --plugin-load-add=selective_trace.so --plugin-load-add=server_audit.so \
    --server_audit_logging=ON --server_audit_file_path=/nonexistent/server_audit.log \
    --loose-innodb-buffer-pool-size=256M --max-allowed-packet=64M \
    --performance-schema=ON --log-warnings=3 \
    >/tmp/asan_mysqld.log 2>&1 &
PID=$!
CLIENT="$BUILD_DIR/client/mariadb -uroot -S $SOCK"
for i in $(seq 1 120); do
  $CLIENT -e "SELECT 1" >/dev/null 2>&1 && break
  kill -0 $PID 2>/dev/null || { echo "!! mariadbd died at startup"; tail -30 /tmp/asan_mysqld.log; exit 1; }
  sleep 1
done
$CLIENT -e "SELECT VERSION(); SELECT PLUGIN_NAME, PLUGIN_STATUS, PLUGIN_AUTH_VERSION
            FROM information_schema.PLUGINS WHERE PLUGIN_NAME IN ('selective_trace','SERVER_AUDIT')"

echo ">> [3/4] Deep battery (${DURATION}s concurrent phase)"
export CLIENT DURATION PLUGIN_SO=selective_trace.so LOGF=/tmp/dt_selective.json
bash "$WORKSPACE/scripts/deep-battery.sh"
RC=$?

echo ">> [4/4] Shutdown and report"
"$BUILD_DIR/client/mariadb-admin" -uroot -S "$SOCK" shutdown 2>/dev/null
for i in $(seq 1 60); do kill -0 $PID 2>/dev/null || break; sleep 1; done
kill -0 $PID 2>/dev/null && { echo "!! mariadbd did not stop in 60s"; kill -9 $PID; RC=1; }

FOUND=0
for f in /tmp/asan_report* /tmp/ubsan_report*; do
  [ -f "$f" ] || continue
  FOUND=1
  echo "================ $f"
  grep -n "ERROR: AddressSanitizer\|runtime error\|SUMMARY" "$f" | head -20
  echo "--- first 60 lines ---"; head -60 "$f"
done
[ $FOUND -eq 0 ] && echo "No sanitizer findings."
echo "--- mysqld log (signals / plugin lines) ---"
grep -i "signal\|Assertion\|Sanitizer\|runtime error\|selective_trace: plugin" /tmp/asan_mysqld.log | tail -12
echo "battery rc=$RC, sanitizer findings=$FOUND"
[ $RC -eq 0 ] && [ $FOUND -eq 0 ]
