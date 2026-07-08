# MariaDB Selective Trace Plugin (`selective_trace`)

[![CI](https://github.com/Kevenny/mariadb-selective-trace-plugin/actions/workflows/ci.yml/badge.svg)](https://github.com/Kevenny/mariadb-selective-trace-plugin/actions/workflows/ci.yml)

Native (open source, GPLv2) plugin for **MariaDB 11.4 and 12.3+** that does
**selective query tracing** — it traces only queries touching specific
schemas/tables (and by command type), unlike `general_log`, which is
all-or-nothing. Low overhead, hot-configurable via `SET GLOBAL`.

Internally it uses the MariaDB Audit Plugin API (the only hook that exposes
resolved schema+table), but the purpose is **diagnostics/observability**,
not compliance.

**Status: implemented and validated** — dynamic filters, FILE output (one
JSON object per line) and TABLE output (`mysql.selective_trace_events`),
millisecond duration, benchmark and Valgrind. Measured overhead: **~0%**
(vs **+10%** for `general_log` in the same synthetic scenario) — see
[docs/BENCHMARKS.md](./docs/BENCHMARKS.md).

> 📖 **How to use the plugin** (variables, JSON format, log table schema,
> limitations): [`docs/USAGE.md`](./docs/USAGE.md)
>
> 🔬 Research on the audit API from the 11.4.4 source (Portuguese):
> [`docs/RESEARCH_NOTES.md`](./docs/RESEARCH_NOTES.md)
>
> ⚖️ Technical decisions — C++ vs C, events used, TABLE-mode anti-loop,
> lessons from real crashes (Portuguese): [`docs/DECISIONS.md`](./docs/DECISIONS.md)
>
> 🔒 Threat model, adversarial test battery and SELinux/OL9 hardening:
> [`docs/SECURITY.md`](./docs/SECURITY.md)
>
> 📜 Code provenance & originality (not a copy of any bundled plugin):
> [`docs/ORIGINALITY.md`](./docs/ORIGINALITY.md)

## Usage TL;DR

```sql
INSTALL PLUGIN selective_trace SONAME 'selective_trace.so';
SET GLOBAL selective_trace_enabled = ON;
SET GLOBAL selective_trace_schemas_to_log = 'sales';             -- by schema
SET GLOBAL selective_trace_tables_to_log  = 'hr.salaries,logs.*'; -- by table
-- => one JSON line per event in selective_trace.json (datadir), or:
SET GLOBAL selective_trace_output = 'TABLE';   -- mysql.selective_trace_events
```

With **both** lists empty, the plugin traces nothing (fail-safe).

## Project layout

```
.
├── src/
│   ├── selective_trace.cc       # entrypoint: audit descriptor, sysvars, capture
│   ├── filter_engine.{h,cc}     # pure filter logic (no MariaDB headers)
│   ├── log_writer_file.{h,cc}   # FILE mode: one JSON line via the logger service
│   ├── log_writer_table.{h,cc}  # TABLE mode: dedicated thread + SQL service
│   └── CMakeLists.txt           # MYSQL_ADD_PLUGIN(selective_trace ... MODULE_ONLY)
├── tests/
│   └── test_filter_logic.cc     # standalone filter_engine tests (plain g++)
├── scripts/
│   ├── setup-dev-env.sh         # bring up the full environment (image + 11.4.4 source)
│   ├── build.sh                 # full | --plugin (incremental) | --package
│   ├── benchmark.sh             # overhead vs general_log (mariadb-slap)
│   └── valgrind-test.sh         # mariadbd under Valgrind + battery
├── docker/                      # dev container (toolchain) + mariadb-test (official)
└── docs/                        # USAGE, RESEARCH_NOTES, DECISIONS, BENCHMARKS, SECURITY
```

## Development

### 1. Bring up the environment

```bash
./scripts/setup-dev-env.sh
```

Builds the development image, starts the `dev` container and clones the
official MariaDB source (tag `mariadb-11.4.4`) into `/opt/mariadb-src`
(volume).

### 2. Build

```bash
# inside the dev container (docker compose -f docker/docker-compose.yml exec dev bash)
./scripts/build.sh full        # first time (full build, 20-60 min)
./scripts/build.sh --plugin    # incremental: plugin only (seconds, with ccache)
./scripts/build.sh --package   # copy the .so to build/plugin_output/
```

### 3. Filter unit tests (no MariaDB)

```bash
g++ -std=c++11 -Wall -Wextra -Werror -I src \
    tests/test_filter_logic.cc src/filter_engine.cc -o test_filter_logic \
  && ./test_filter_logic
```

### 4. Test on a clean, official MariaDB 11.4.4

```bash
docker compose -f docker/docker-compose.yml --profile test up -d mariadb-test
docker compose -f docker/docker-compose.yml exec mariadb-test \
  mariadb -uroot -pdevpassword testdb
```

`docker/test-my.cnf` already loads the plugin
(`plugin-load-add=selective_trace.so` + `plugin-maturity=experimental`) with
the initial filter `selective_trace_schemas_to_log=testdb`.

### 5. Benchmark and Valgrind

```bash
docker exec -i mariadb-plugin-test bash < scripts/benchmark.sh
docker exec -i mariadb-plugin-dev  bash < scripts/valgrind-test.sh
```

### 6. Build for Oracle Linux / RHEL 8+

The `dev` container's `.so` (Ubuntu 22.04) needs glibc ≥ 2.35 and will not
load on EL8/EL9. Use the `dev-ol8` environment (base `oraclelinux:8` +
gcc-toolset-12), which produces a binary requiring only GLIBC_2.17 — loadable
on EL8, EL9 and newer:

```bash
docker compose -f docker/docker-compose.yml --profile ol8 up -d --build dev-ol8
docker exec mariadb-plugin-dev-ol8 bash -lc 'cd /workspace && ./scripts/build.sh full && ./scripts/build.sh --package'
# output: build/plugin_output-ol8/selective_trace.so
# validate on a clean OL8 with MariaDB 11.4 installed via the official RPM:
docker run --rm -i -v "$PWD/build/plugin_output-ol8:/plugin_out:ro" \
    oraclelinux:8 bash < scripts/validate-ol8.sh
```

Validated against MariaDB 11.4.12 (official RPMs) on Oracle Linux 8 **and 9**
(same .so; for OL9 just swap the image to oraclelinux:9) — the plugin built
against the 11.4.4 source is compatible with the whole 11.4.x series.

### 7. Build for MariaDB 12.3+ (Oracle Linux 9)

The audit ABI changed from `0x0302` (11.4) to `0x0303` (12.3), so the 11.4
`.so` will **not** load on a 12.3 server. The same source compiles for both
series (a `MYSQL_VERSION_ID` wrapper handles the logger service change); only
a dedicated build against the 12.3 source is needed:

```bash
docker compose -f docker/docker-compose.yml --profile v123 up -d --build dev-123-ol8
docker exec mariadb-plugin-dev-123-ol8 bash -lc \
  './scripts/download-mariadb-source.sh && ./scripts/build.sh full && ./scripts/build.sh --package'
# output: build/plugin_output-123-ol9/selective_trace.so
docker run --rm -i -v "$PWD/build/plugin_output-123-ol9:/plugin_out:ro" \
    oraclelinux:9 bash < scripts/validate-123-ol9.sh
```

Validated against MariaDB 12.3.2 (official RPM) on Oracle Linux 9: plugin
ACTIVE, full smoke test and 7/7 security battery.

**Windows**: not supported in this version — `log_writer_table` uses pthreads,
POSIX clocks and `__attribute__((constructor))`. A port is feasible
(std::thread/std::chrono + DllMain, as server_audit does), but requires an
MSVC toolchain to build the MariaDB tree on Windows.

## License

GPLv2, compatible with the MariaDB Server license.
