# Windows 10/11 homologation

`selective_trace` is portable to Windows: the only OS-specific code (the
monotonic timer, the wall clock, and the table-writer's background thread) uses
MariaDB's own portable wrappers, so it compiles under MSVC and loads into a
Windows MariaDB server as a native `.dll`.

This document covers **runtime** homologation on a real Windows server. The
`.dll` itself is produced by CI — see [Getting the .dll](#getting-the-dll).

## Portability summary

| Concern | POSIX (Linux) | Windows path |
|---|---|---|
| Monotonic timer (duration) | `clock_gettime(CLOCK_MONOTONIC)` | `my_interval_timer()` — portable, both platforms |
| Wall clock (timestamp) | `gettimeofday()` | `my_hrtime()` — portable, both platforms |
| Local time formatting | `localtime_r()` | `localtime_r()` shim in `my_pthread.h` (maps to `localtime_s`) |
| Writer thread + queue | raw `pthread_*` | same calls, emulated on Windows by `my_pthread.h` (CRITICAL_SECTION / CONDITION_VARIABLE); mutex/cond initialized at runtime |
| File output | logger service | logger service — already cross-platform |
| Table output | `mysql_real_connect_local` | same server API — cross-platform |

No behavior, JSON output, or log-table schema differs between platforms.

## Getting the .dll

The **Windows build** GitHub Actions workflow (`.github/workflows/windows.yml`)
builds one `.dll` per server series against the real MariaDB source on a
`windows-2022` runner and uploads it as an artifact:

- `selective_trace-11.4.4-win64.dll`
- `selective_trace-12.3.2-win64.dll`

Trigger it from the **Actions** tab (**Run workflow**) or by pushing a `v*`
tag. Download the artifact for your server series.

> The audit ABI differs between series (0x0302 on 11.4, 0x0303 on 12.3): use
> the `.dll` that matches your server, or `INSTALL PLUGIN` fails to load it.

## Install on Windows 10/11

1. Find the plugin directory: `SHOW VARIABLES LIKE 'plugin_dir';`
   (typically `C:\Program Files\MariaDB 11.4\lib\plugin\`).
2. Copy the artifact there, renamed to `selective_trace.dll`.
3. Load it (hot, no restart, no maturity flag):

   ```sql
   INSTALL PLUGIN selective_trace SONAME 'selective_trace.dll';
   ```

## Functional homologation matrix

Run the same checks the Linux MTR suite covers, against the loaded plugin:

```sql
SET GLOBAL selective_trace_enabled = ON;

-- 1. schema filter
SET GLOBAL selective_trace_schemas = 'app';
-- run queries in `app` and in another schema → only `app` is traced

-- 2. cross-schema table filter (wildcard too)
SET GLOBAL selective_trace_schemas = '';
SET GLOBAL selective_trace_tables  = 'app.orders, ops.*';

-- 3. command-type filter
SET GLOBAL selective_trace_tables  = 'app.orders:dml';   -- only DML on the table

-- 4. connection filter (full trace of one session)
SET GLOBAL selective_trace_connections = '<conn_id from SHOW PROCESSLIST>';

-- 5. both filters empty → nothing is logged (fail-safe)
SET GLOBAL selective_trace_schemas = '';
SET GLOBAL selective_trace_tables  = '';
```

Verify each output mode:

- **FILE**: `SET GLOBAL selective_trace_output = 'FILE';` then check the file at
  `selective_trace_log_file_path`. On Windows the default resolves under the
  data directory; **paths contain backslashes** — confirm they appear correctly
  escaped in the JSON (`"\\"`), which `json_escape_append` already handles.
- **TABLE**: `SET GLOBAL selective_trace_output = 'TABLE';` then
  `SELECT * FROM mysql.selective_trace_events ORDER BY id DESC LIMIT 10;`
  and confirm no self-logging loop.

`UNINSTALL PLUGIN selective_trace;` must not crash the server.

## Memory validation (no Valgrind on Windows)

Windows has no Valgrind. Use one of:

- **MSVC AddressSanitizer** — configure the build with `/fsanitize=address`
  (add `-DCMAKE_CXX_FLAGS="/fsanitize=address"` to the CMake configure step),
  load the `.dll`, exercise the query matrix, and watch for ASan reports.
- **Application Verifier** (`appverif.exe`) enabled for `mariadbd.exe` — catches
  heap corruption and leaks.
- **Dr. Memory** — `drmemory -- mariadbd.exe ...`.

Exercise: install, run the full functional matrix above under load, uninstall.
Expect zero leaks attributable to the plugin.

## Notes / known differences

- MTR on Windows is not run in CI yet (it needs the full client set built and is
  flaky in CI); the Linux MTR suite already validates the platform-independent
  logic. Running `mysql-test-run.pl --suite=selective_trace` on a local Windows
  build is a valid additional check.
- The `.dll` needs the same Visual C++ runtime as the server; the MariaDB MSI
  already ships it, so no extra redistributable is required.
