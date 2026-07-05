# SECURITY.md — `selective_trace` threat model and hardening

Adversarial security validation of the plugin, focused on Oracle Linux 9.
Reproducible battery in [`scripts/security-test.sh`](../scripts/security-test.sh)
(clean `oraclelinux:9` container, MariaDB 11.4.12 via official RPM).

---

## Attack surface

Per audit event, the plugin receives client-controlled data (query text,
`user@host`, schema/table names resolved by the parser) and writes it to two
destinations: a JSON file (FILE mode) or
`INSERT INTO mysql.selective_trace_events` (TABLE mode). The vectors are
**injection** of that data into the output format and **leakage** of
sensitive data present in the queries themselves.

Changing the configuration requires server privilege (`SUPER` /
`SET_USER` / `SESSION_VARIABLES_ADMIN` for `SET GLOBAL`;
`INSERT_PLUGIN`/`CREATE PLUGIN` to install) — outside the plugin's control,
enforced by MariaDB.

## Battery results (OL9, plugin v0.6.0+)

Run against two server series, **7/7 on both**: MariaDB 11.4.12 and MariaDB
12.3.2 (official RPMs on `oraclelinux:9`).

| Test | Vector | Result |
|---|---|---|
| T1 | SQL injection into the internal INSERT (TABLE mode), default `sql_mode` | **PASS** — payload stored as literal data; `mysql.global_priv` intact |
| T2 | Same with `sql_mode=NO_BACKSLASH_ESCAPES` | **PASS** — the writer pins its own `sql_mode` (see mitigation below) |
| T3 | JSON/newline injection (FILE mode) | **PASS** — every line stays valid JSON, one event per line |
| T4 | Cleartext DCL password leakage | **PASS** after fix (masking; the only FAIL on the first run) |
| T5 | Log file permissions | **PASS** — owner `mysql`, mode `0660` |
| T6 | Inaccessible path (e.g. `/root`) | **PASS** — graceful failure, server intact |

## Implemented mitigations

### 1. SQL injection in TABLE mode (defense in depth)

- `sql_escape_append()` escapes `'`, `\`, NUL, `\n`, `\r`, `Ctrl-Z` when
  building the INSERT.
- **Backslash escaping is only valid without `NO_BACKSLASH_ESCAPES`.** To not
  depend on the global `sql_mode` (which a `SET GLOBAL sql_mode=...` could
  change under the writer's feet), the writer's internal connection runs
  `SET SESSION sql_mode=''` on connect — the escaping stays consistent.
  Without this, a `'` in the query text could break out of the INSERT
  literal.
- The INSERTs run on a dedicated internal connection (`sql_log_bin=0`,
  `skip_grants`), never on the user's session.

### 2. JSON injection in FILE mode

`json_escape_append()` escapes quotes, backslash and all controls < 0x20
(via `\uXXXX`) — it is impossible to inject a newline (which would break the
"one event per line" invariant) or to close/reopen the JSON object.

### 3. Credential masking (`selective_trace_mask_passwords`, default ON)

Because the plugin records the full query text (trace fidelity, like
general_log), DCL statements would expose passwords. `mask_secrets()`
replaces the literals of the following with `***`:

- `IDENTIFIED BY '...'`
- `IDENTIFIED BY PASSWORD '...'`
- `IDENTIFIED WITH <plugin> {BY|AS|USING} '...'`
- `PASSWORD('...')` / `PASSWORD '...'`
- `SET PASSWORD ... = '...'`

Case-insensitive matching, respects word boundaries (a `password_hash` column
or the text `'my password'` in an ordinary INSERT does **not** trigger
masking) and handles escaped quotes inside the secret. Can be turned off with
`SET GLOBAL selective_trace_mask_passwords=OFF` if you need the intact text in
a controlled environment.

> **Honest limitation**: masking covers the standard authentication clauses.
> It does **not** understand your application's semantics — if you trace a
> schema where the application itself runs `INSERT INTO users(pass) VALUES
> ('text')`, that value is business data and will be logged. Treat the log as
> a sensitive artifact (permissions + retention), as you would the
> general_log or the binlog.

## Oracle Linux 9-specific hardening

### SELinux (enforcing by default on OL9)

`mariadbd` runs confined in the `mysqld_t` domain. Two points:

1. **`.so` context**: when copying the plugin to `plugin_dir`, apply the
   correct context or SELinux blocks the `dlopen`:

   ```bash
   cp selective_trace.so /usr/lib64/mysql/plugin/
   restorecon -v /usr/lib64/mysql/plugin/selective_trace.so
   # (expected label: system_u:object_r:lib_t or mysqld_plugin_exec_t)
   ```

2. **Log path (FILE mode)**: `mysqld_t` only writes to locations labeled
   `mysqld_db_t` / `mysqld_log_t`. **Keep the log inside the datadir**
   (`/var/lib/mysql/…`, default) or a directory with a suitable label. An
   arbitrary path (e.g. `/root`, `/tmp`) will be **denied by SELinux**, not by
   a plugin bug — the plugin merely records the failure
   (`Selective_trace_write_failures`) without crashing the server (validated
   in T6). For a dedicated directory:

   ```bash
   semanage fcontext -a -t mysqld_log_t "/var/log/mariadb/selective(/.*)?"
   restorecon -Rv /var/log/mariadb
   ```

   > Note: the automated battery runs in a container whose kernel reports
   > SELinux `Disabled` (test-host limitation), so the **denial** itself is
   > not exercised there — the commands above are the correct procedure for
   > the real OL9 host with `enforcing`.

### Log file permissions and retention

- The file is created with the process owner (`mysql`) and mode `0660` (group
  `mysql`). Do not add other users to the `mysql` group.
- The log **contains query text** (potentially sensitive data, even with
  passwords masked). Restrict and rotate it:

  ```bash
  chmod 640 /var/lib/mysql/selective_trace.json   # to drop the group bit
  # logrotate: rotate with  create 0640 mysql mysql
  ```

- TABLE mode: `mysql.selective_trace_events` inherits the `mysql` schema's
  permissions (already restricted to admins). Purge it periodically.

## Operational best practices

- **Minimal filter**: trace only what you need — reduces volume and exposure.
- **Monitor** `SHOW GLOBAL STATUS LIKE 'selective_trace%'`:
  `write_failures` (path/SELinux/disk), `events_dropped` (full TABLE queue),
  `callback_errors` (memory pressure).
- **Kill switch**: `SET GLOBAL selective_trace_enabled=OFF` at runtime.
- **EXPERIMENTAL maturity**: the server refuses to load it by default;
  whoever installs it opts in consciously with
  `plugin-maturity=experimental`.

## Reporting vulnerabilities

Open an issue on the repository (or privately contact the maintainer for
sensitive matters) describing the version, platform and reproduction steps.
