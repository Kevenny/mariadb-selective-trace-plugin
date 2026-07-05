# USAGE.md — `selective_trace` plugin usage guide

Selective query trace plugin for **MariaDB 11.4 and 12.3+** that logs only
queries touching the configured schemas/tables — a low-overhead alternative
to `general_log`.

---

## 0. Supported platforms

The plugin is a native binary — use the `.so` compiled for the server's
platform:

| Platform / server series | Build |
|---|---|
| Ubuntu 22.04+/Debian · MariaDB 11.4 (glibc ≥ 2.35) | `build/plugin_output/selective_trace.so` (`dev` container) |
| Oracle Linux / RHEL / Rocky / Alma **8 and 9** · MariaDB **11.4** | `build/plugin_output-ol8/selective_trace.so` (`dev-ol8` container, glibc ≥ 2.17) |
| Oracle Linux / RHEL 8+ · MariaDB **12.3+** | `build/plugin_output-123-ol9/selective_trace.so` (`dev-123-ol8` container) |
| Windows | not supported in this version (POSIX code; port feasible, see README) |

The EL builds require only GLIBC_2.17+, so they load on EL8, EL9 and newer
distros. **Validated on Oracle Linux 8 and 9** with the official RPMs
(mariadb.org), full smoke test (FILE, TABLE, cross-schema JOIN,
UNINSTALL/INSTALL):

- **MariaDB 11.4.12** → `plugin_output-ol8/` (OL8 and OL9)
- **MariaDB 12.3.2** → `plugin_output-123-ol9/` (OL9)

> ⚠️ **The server series matters**: the audit ABI changed from `0x0302`
> (11.4) to `0x0303` (12.3), so the 11.4 `.so` will **not** load on a 12.3
> server and vice versa. Use the build for the matching series. For other
> series (10.11, 11.8...), recompile against that series' source.

Build for 12.3+:

```bash
docker compose -f docker/docker-compose.yml --profile v123 up -d --build dev-123-ol8
docker exec mariadb-plugin-dev-123-ol8 bash -lc \
  './scripts/download-mariadb-source.sh && ./scripts/build.sh full && ./scripts/build.sh --package'
# validate on a clean OL9 with MariaDB 12.3 via official RPM:
docker run --rm -i -v "$PWD/build/plugin_output-123-ol9:/plugin_out:ro" \
    oraclelinux:9 bash < scripts/validate-123-ol9.sh
```

To validate on OL9, the same script works (only the image changes):

```bash
docker run --rm -i -v "$PWD/build/plugin_output-ol8:/plugin_out:ro" \
    oraclelinux:9 bash < scripts/validate-ol8.sh
```

How to produce the EL8 build:

```bash
docker compose -f docker/docker-compose.yml --profile ol8 up -d --build dev-ol8
docker exec mariadb-plugin-dev-ol8 bash -lc 'cd /workspace && ./scripts/build.sh full && ./scripts/build.sh --package'
# validate on a clean OL8 with MariaDB 11.4 via official RPM:
docker run --rm -i -v "$PWD/build/plugin_output-ol8:/plugin_out:ro" \
    oraclelinux:8 bash < scripts/validate-ol8.sh
```

On the target server (OL8+ with MariaDB via RPM), the `plugin_dir` is
`/usr/lib64/mysql/plugin/` — copy the `.so` there.

## 1. Installation

Copy `selective_trace.so` to the server's `plugin_dir` (check it with
`SHOW GLOBAL VARIABLES LIKE 'plugin_dir'`) and:

```sql
INSTALL PLUGIN selective_trace SONAME 'selective_trace.so';
```

Or via configuration (loads at startup):

```ini
[mysqld]
plugin-load-add=selective_trace.so
# the plugin declares "experimental" maturity; allow it if the server uses
# the default (gamma):
plugin-maturity=experimental
```

To remove it:

```sql
UNINSTALL PLUGIN selective_trace;
```

> The plugin does **not** require `general_log=ON` — the audit events are
> emitted by the server regardless of the general log.

## 2. System variables

All dynamic (`SET GLOBAL`), no restart needed:

| Variable | Type | Default | Description |
|---|---|---|---|
| `selective_trace_enabled` | BOOL | `OFF` | Enable/disable capture |
| `selective_trace_schemas_to_log` | VARCHAR | `''` | Comma-separated list of schemas |
| `selective_trace_tables_to_log` | VARCHAR | `''` | Comma-separated `schema.table` list (cross-schema); `schema.*` = the whole schema |
| `selective_trace_output` | ENUM | `FILE` | `FILE` (one JSON line) or `TABLE` (`mysql.selective_trace_events`) |
| `selective_trace_file_path` | VARCHAR | `selective_trace.json` | Log file in FILE mode (relative = datadir) |
| `selective_trace_min_duration_ms` | INT | `0` | Log only queries slower than N ms (0 = all) |
| `selective_trace_mask_passwords` | BOOL | `ON` | Replace DCL secrets (`IDENTIFIED BY`, `SET PASSWORD`, `PASSWORD()`) with `***` before logging |

### Filter by command type (per entry)

Every entry in both lists accepts an optional `:cmd1|cmd2` qualifier
restricting **which commands** are logged for that schema/table:

```sql
-- schema "sales" only INSERT and UPDATE; schema "hr" everything
SET GLOBAL selective_trace_schemas_to_log = 'sales:insert|update, hr';

-- table app.orders only DELETE; the whole "logs" schema only DML
SET GLOBAL selective_trace_tables_to_log = 'app.orders:delete, logs.*:dml';
```

Valid tokens: `select`, `insert`, `update`, `delete`, `replace`, `load`,
`call`, `create`, `alter`, `drop`, `truncate`, `rename`, `other`, and the
groups `dml` (insert|update|delete|replace|load), `ddl`
(create|alter|drop|truncate|rename) and `all`. No qualifier = all commands.
An unknown token makes the `SET GLOBAL` fail. Duplicate entries have their
masks merged (`a:insert, a:update` ≡ `a:insert|update`).

The statement's command is the same as the `command` field (first SQL
keyword, ignoring comments); `WITH` (CTE) counts as `select`. Statements that
cannot be classified fall under `other`.

### Filter semantics

- **Both lists empty ⇒ nothing is logged** (fail-safe: the plugin never
  accidentally becomes a general_log).
- A query is logged if: **some touched table** matches `tables_to_log`, **or**
  the touched table/schema or the **session's current schema** matches
  `schemas_to_log`.
- Multi-table `JOIN`: a single matching table is enough to log (the record
  carries all touched tables).
- Matching is **case-insensitive** (ASCII); optional backticks are accepted.
- Statements that touch no table (`SET`, `SHOW`, `SELECT 1`) are logged only
  if the session's current schema (`USE ...`) matches the schema filter.
- Invalid values are rejected at `SET GLOBAL` time:

```
SET GLOBAL selective_trace_tables_to_log='nodot';
ERROR 1231: selective_trace: invalid entry 'nodot' in tables_to_log
            (expected schema.table or schema.*)
```

### Examples

```sql
-- Trace everything touching the production schema "sales"
SET GLOBAL selective_trace_schemas_to_log = 'sales';
SET GLOBAL selective_trace_enabled = ON;

-- Trace only two sensitive tables, regardless of the session schema
SET GLOBAL selective_trace_schemas_to_log = '';
SET GLOBAL selective_trace_tables_to_log = 'hr.salaries,finance.payments';

-- The whole "logs" schema + one standalone table
SET GLOBAL selective_trace_tables_to_log = 'logs.*,app.orders';

-- Only slow queries (>250ms) of the app schema
SET GLOBAL selective_trace_schemas_to_log = 'app';
SET GLOBAL selective_trace_min_duration_ms = 250;
```

## 3. FILE mode (default)

One JSON line per event in `selective_trace_file_path`:

```json
{"ts":"2026-07-04 03:33:44.401","conn_id":4,"query_id":7,
 "user":"root@localhost","db":"testdb","tables":["testdb.t1"],
 "command":"SELECT","duration_ms":0.391,"error_code":0,
 "query":"SELECT * FROM testdb.t1"}
```

Fields:

| Field | Meaning |
|---|---|
| `ts` | Local timestamp with milliseconds |
| `conn_id` | The session's `connection_id` |
| `query_id` | Internal statement id (correlates with the log table) |
| `user` | `user@host` |
| `db` | Session's current schema (empty if no `USE`) |
| `tables` | Tables touched by the statement (`schema.table`) |
| `command` | First SQL keyword (`SELECT`, `INSERT`, `CREATE`...) |
| `duration_ms` | Duration with ms precision (`null` if the start wasn't seen) |
| `error_code` | 0 = success; otherwise the error code (e.g. 1146) |
| `query` | Full query text |

Notes:
- Internal statistics bookkeeping tables (`mysql.table_stats`,
  `mysql.column_stats`, `mysql.index_stats`, `mysql.innodb_table_stats`,
  `mysql.innodb_index_stats`) are touched as a side effect of ordinary DML
  and do **not** appear in `tables` — unless they are explicitly listed in
  `selective_trace_tables_to_log`.
- `command` ignores leading comments of every flavor (`-- `, `#`, `/* */`,
  `/*! */`, `/*M! */`) and parentheses — an `INSERT` sent with an attached
  comment (DBeaver's default behavior) is classified as `INSERT`. The `query`
  field preserves the exact text received, comments included (trace fidelity,
  like general_log).
- If a statement touches more tables than the per-connection buffer holds
  (~3.9 KB of names), the JSON gains `"tables_truncated":true` (in the log
  table, the list ends with `,...`).
- Statements inside stored procedures/functions produce their own events
  (one per sub-statement, with their tables), in addition to the `CALL`
  event.
- The file has no size-based rotation — use logrotate/Fluentd/Filebeat.
- The path is reopened automatically when `selective_trace_file_path`
  changes.

## 4. TABLE mode

```sql
SET GLOBAL selective_trace_output = 'TABLE';
```

Events are inserted into **`mysql.selective_trace_events`** (created
automatically on first use):

```sql
CREATE TABLE mysql.selective_trace_events (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  ts DATETIME(3) NOT NULL,
  conn_id BIGINT UNSIGNED NOT NULL,
  query_id BIGINT UNSIGNED NOT NULL,
  user VARCHAR(384) NOT NULL DEFAULT '',
  db VARCHAR(192) NOT NULL DEFAULT '',
  tables_involved TEXT NOT NULL,
  command VARCHAR(32) NOT NULL DEFAULT '',
  duration_ms DOUBLE NULL,
  error_code INT NOT NULL DEFAULT 0,
  query MEDIUMTEXT NOT NULL,
  KEY idx_selective_trace_ts (ts)
) ENGINE=Aria TRANSACTIONAL=0 DEFAULT CHARSET=utf8mb4;
```

How it works under the hood:
- Writing is **asynchronous**: a dedicated plugin thread consumes a queue
  (up to 10000 events) and runs the INSERTs on an internal connection with
  `sql_log_bin=0` (does not replicate). Events may take a few ms to appear.
- If the queue fills up (burst above the INSERT throughput), events are
  dropped and counted in `Selective_trace_events_dropped`.
- The plugin **never logs its own INSERTs** (per-thread reentrancy guard) —
  no self-log loop, even with `mysql` in the filter.
- If the table is dropped, it is recreated on the next INSERT.

## 5. Status (`SHOW GLOBAL STATUS LIKE 'selective_trace%'`)

| Variable | Meaning |
|---|---|
| `Selective_trace_events_logged` | Events accepted (written or queued) |
| `Selective_trace_write_failures` | Write failures (file + table) |
| `Selective_trace_events_dropped` | Events dropped due to a full queue (TABLE mode) |
| `Selective_trace_callback_errors` | Exceptions swallowed at the C boundaries (memory pressure/bug) |

## 6. Known limitations

- `duration_ms` is measured by the plugin itself (monotonic clock between
  dispatch start and statement end); if the plugin is enabled mid-statement,
  the first event comes out with a null `duration_ms`.
- With the query cache on (OFF by default in 11.4), SELECTs served from the
  cache produce no table events.
- The table filter uses per-statement lock events; commands that touch no
  table depend on the session schema filter.
- Identifiers containing `.` or `,` in the name are not supported in the
  lists.
- Identifier matching is ASCII case-insensitive (does not use collation).
