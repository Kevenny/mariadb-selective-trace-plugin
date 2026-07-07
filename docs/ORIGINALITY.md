# ORIGINALITY.md — Code provenance and originality

This plugin was written from scratch. It is **not** a fork or a copy of any
existing MariaDB plugin. It uses the same public plugin APIs as the bundled
`server_audit` plugin (they are the only supported way to observe queries),
but the implementation is original.

Both this plugin and `server_audit` are GPLv2. Under GPLv2 copying with
attribution would be permitted, but we deliberately keep the code original —
cleaner for an eventual upstream submission and free of any attribution
questions.

## Method

We ran an automated code-similarity check (contiguous runs of normalized
lines — comments stripped, whitespace collapsed, lowercased) between every
source file of this plugin and `plugin/server_audit/server_audit.c` from the
MariaDB source tree.

- **Runs of ≥ 4 identical normalized lines: 0**
- **Runs of ≥ 3 identical normalized lines: 0**
- Runs of exactly 2 lines: 3, all unavoidable API boilerplate (see below).

## The three unavoidable 2-line overlaps

These cannot and should not be rewritten — they are the mandatory vocabulary
of the MariaDB plugin ABI, identical in every audit plugin (including
`audit_null`):

1. **Plugin API includes** — `#include <mysql/plugin.h>` followed by
   `#include <mysql/plugin_audit.h>`. Every audit plugin includes these.
2. **The `st_mysql_audit` descriptor** — `MYSQL_AUDIT_INTERFACE_VERSION,`
   followed by `NULL,`. The struct layout is fixed by `plugin_audit.h`; the
   field order is dictated by the ABI, not by us.
3. A generic `break; } else` inside an unrelated parsing loop — a trivial C
   control-flow coincidence, semantically unrelated (ours splits a CSV list;
   server_audit's escapes passwords).

## Rewrites applied for clear authorship

Two spots had similar *expression* (not just API idiom). Both were rewritten
to be unambiguously our own while keeping identical behavior:

- **`is_query_command`** (selective_trace.cc) — detecting whether a
  GENERAL_STATUS event carries SQL text. Reworked from a chained boolean
  expression into a small lookup table iterated with `array_elements`.
- **PSI rwlock registration** (`file_writer_init`, `selective_trace_init`) —
  the `#ifdef HAVE_PSI_INTERFACE` / `if (PSI_server)` check is mandatory, but
  we extracted it into named helpers (`register_psi_rwlock`,
  `register_filter_psi`) and reordered the init sequence so no contiguous run
  matches.

## Architectural differences from `server_audit`

Beyond line-level originality, the design differs substantially:

| Aspect | server_audit | selective_trace |
|---|---|---|
| Purpose | audit/compliance | selective query **trace** |
| Filtering | by user (incl/excl) + event class | by **schema / table / command type / connection** |
| Pure filter logic | inline in one .c file | isolated in `filter_engine.{h,cc}`, no MariaDB headers, standalone unit-tested |
| TABLE output | — | dedicated writer thread + SQL service queue |
| Concurrency | `mysql_prlock_t` | `mysql_rwlock_t` |
| Language | C | C++11 |
| Layout | single 3130-line file | 7 files split by responsibility |

The comparison is reproducible with the script used during review (normalize
+ longest-common-run detection); see the code-review notes.
