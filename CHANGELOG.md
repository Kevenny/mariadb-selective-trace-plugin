# Changelog

All notable changes to `selective_trace` are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/), and the
project adheres to [Semantic Versioning](https://semver.org/).

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
