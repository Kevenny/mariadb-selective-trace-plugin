# Changelog

All notable changes to `selective_trace` are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/), and the
project adheres to [Semantic Versioning](https://semver.org/).

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
