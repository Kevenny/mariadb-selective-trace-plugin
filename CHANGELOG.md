# Changelog

All notable changes to `selective_trace` are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/), and the
project adheres to [Semantic Versioning](https://semver.org/).

## [1.2.4] - 2026-08-20

### Added

- **Unit tests expanded from 157 to 242**, covering DDL classification (every
  DDL verb from real statement text, including versioned-comment wrappers, and
  the invariant that a `:ddl` filter can never match DML), SELECT
  classification (`UNION`/subquery/`JOIN`/CTE/hint forms, and that
  `INSERT ... SELECT` or `CREATE TABLE ... AS SELECT` are *not* reads), and
  edge/robustness inputs against the fixed-size command buffer in the audit
  hot path (60 KB statement, 5 KB keyword, 9 KB leading comment, unterminated
  comment, embedded NUL, 200-char identifiers).

### Verified (no code change required)

Full TABLE-mode battery on MariaDB 11.4.4 — see docs/DECISIONS.md D28:

- 1,200-statement DDL storm: all captured and correctly classified.
- 1.66M-event soak (SELECT + JOIN + UPDATE, concurrency 8): **RSS plateaus at
  547 MB** — the final 570k events produced zero RSS growth. 0 write failures.
- 1.52M rows written: 0 malformed, 0 from an unfiltered schema, `CHECK TABLE`
  OK, no crash or signal in the error log.
- Valgrind over the full lifecycle (DDL + SELECT + DML + error paths + filter
  churn + FILE/TABLE switch + toggle + UNINSTALL/INSTALL): 0 bytes lost, 0
  still reachable, no plugin frame in any record.

### Note on burst behaviour

Under an extreme burst (~42k statements/s) the TABLE writer cannot keep up and
the queue cap (10,000 events) drops the excess, counted in
`Selective_trace_events_dropped`. This is deliberate backpressure — dropping is
what prevents the unbounded growth fixed in 1.2.2. **TABLE output is not
lossless under extreme burst**; `FILE` output has no queue and does not drop.

## [1.2.3] - 2026-08-19

### Fixed

- **TABLE output silently stopped writing after the log table was left in a
  crashed state, and never recovered — not even across `enabled=OFF/ON` or
  `UNINSTALL`/`INSTALL PLUGIN`.** The writer only knew how to recover from a
  *missing* table or a dropped connection; a corrupted one (common after the
  OOM crash fixed in 1.2.2, since the log table is `Aria`/`TRANSACTIONAL=0` and
  not crash-safe) made every INSERT fail forever. It looked healthy from the
  outside because `Selective_trace_events_logged` counts enqueued events, not
  committed rows.

  The writer now issues `REPAIR TABLE` and retries when it sees a
  corrupted-table error. Note these arrive as *raw handler* codes (144/145/126/
  127/180), not the mapped `ER_CRASHED_ON_USAGE` (1194) one might expect — a
  fix written against the mapped codes alone would not have covered the
  reported case. New status counter `Selective_trace_table_repairs`; a non-zero
  value means the server did not shut down cleanly at some point.

  Conditions that need an operator decision — table full (1114), permissions,
  schema mismatch after someone `ALTER`s the log table — are still only counted
  and logged, never "fixed" by dropping data. Those resume on their own once
  resolved externally. Details: docs/DECISIONS.md D27.

### Verified

- Toggling `selective_trace_enabled` OFF/ON is **not** itself affected: it
  works correctly standalone, within a single persistent session, and across
  repeated toggles under concurrent load (collection resumed every cycle, RSS
  flat, 0 write failures, 0 dropped events).

## [1.2.2] - 2026-08-19

### Fixed

- **TABLE output mode: unbounded RSS growth under sustained load, ending in an
  OOM kill.** The writer's single long-lived internal connection accumulated
  heap fragmentation across millions of `INSERT`s (~11–12 KB/event, not a
  pointer leak — confirmed with Valgrind: 0 bytes lost across a full clean
  run). Reproduced: 1.5M events at concurrency 16 grew RSS from 212 MB to
  17.8 GB. Fixed by recycling the writer's internal connection every 20,000
  events; same reproduction now holds RSS flat around ~500 MB. New status
  counter `Selective_trace_writer_reconnects`. `FILE` output was unaffected
  and needed no change. Full root-cause writeup: docs/DECISIONS.md D26.

  If you hit `Table was marked as crashed and should be repaired` for
  `mysql.selective_trace_events` after an OOM/unclean shutdown on an older
  version, run `REPAIR TABLE mysql.selective_trace_events;` — the table is
  `Aria`/`TRANSACTIONAL=0` by design (fast, not crash-safe by itself).

## [1.2.1] - 2026-08-19

### Changed

- Set the plugin author to **Kevenny Ferraz** (the `PLUGIN_AUTHOR` field shown
  in `SHOW PLUGINS` / `information_schema.PLUGINS`) and updated the source-file
  copyright headers accordingly. Metadata only — no behavior, output, or ABI
  change.

## [1.2.0] - 2026-08-06

### Added

- **Windows 10/11 support.** The plugin now compiles under MSVC and loads into
  a Windows MariaDB server as a native `.dll`. A GitHub Actions workflow
  (`.github/workflows/windows.yml`) builds one `.dll` per server series
  (11.4 / 12.3) on a `windows-2022` runner and uploads it as an artifact.
  Runtime homologation guide: `docs/WINDOWS.md`.

### Changed

- Replaced the OS-specific code paths with MariaDB's portable wrappers, with no
  behavior change on Linux (MTR unchanged):
  - `clock_gettime(CLOCK_MONOTONIC)` → `my_interval_timer()` (monotonic ns);
  - `gettimeofday()` → `my_hrtime()` (wall-clock µs);
  - the table-writer's `pthread` mutex/cond are now initialized at runtime
    (the static `PTHREAD_*_INITIALIZER` forms do not exist on Windows), using
    the pthread API that `my_pthread.h` emulates on Windows.

## [1.1.0] - 2026-08-06

### Changed (BREAKING)

- Dropped the redundant `_to_log` suffix from the three filter variables.
  They already live under the `selective_trace_` namespace, so the suffix
  only added noise.

  | Before (≤ 1.0.x) | Now (1.1.0) |
  |---|---|
  | `selective_trace_schemas_to_log` | `selective_trace_schemas` |
  | `selective_trace_tables_to_log` | `selective_trace_tables` |
  | `selective_trace_connections_to_log` | `selective_trace_connections` |

  No behavior, JSON output format, or log-table schema changed. The filter
  engine and its 157 unit tests are unaffected.

### Migration

Any `my.cnf`, init-file, or automation referencing the old names must be
updated, or the server reports **"Unknown system variable"**. The rename is
a pure suffix removal:

```bash
sed -i 's/_to_log//g' /etc/my.cnf.d/selective_trace.cnf
```

```sql
-- old
SET GLOBAL selective_trace_schemas_to_log = 'app';
-- new
SET GLOBAL selective_trace_schemas = 'app';
```

The other five variables (`selective_trace_enabled`, `_output`,
`_log_file_path`, `_min_duration_ms`, `_mask_passwords`) are unchanged.

## [1.0.1] - 2026-07-30

### Fixed

- `START SLAVE` / `START REPLICA` / `START ALL SLAVES` were misclassified as a
  transaction `BEGIN` and captured by a `:begin` command filter. Command
  disambiguation moved to the full-query parser: a leading `START` maps to
  `BEGIN` only when followed by `TRANSACTION`; otherwise it is `other`.
  (See docs/DECISIONS.md D24.)

## [1.0.0] - 2026-07-05

### Added

- First stable release. Selective query trace for MariaDB 11.4 and 12.3+:
  filter by schema, table (cross-schema), command type, transaction command,
  or connection id. FILE (one JSON object per line) and TABLE output modes,
  hot-configurable via `SET GLOBAL`. Declares STABLE maturity (installs with
  no flag, no restart). GPLv2.
