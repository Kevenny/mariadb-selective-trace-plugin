# Contributing to `selective_trace`

Thanks for your interest! This is a **selective query trace** plugin for
MariaDB (11.4 and 12.3+) — it lets you trace queries from specific
schemas/tables, unlike `general_log`, which is all-or-nothing. Licensed under
**GPLv2** (the same license as MariaDB Server, to allow eventual upstream
inclusion).

## Reporting bugs / requesting features

Open an issue on the repository describing:

- Plugin version (`PLUGIN_AUTH_VERSION` in `information_schema.PLUGINS`),
  MariaDB series and distro/architecture.
- Relevant configuration (`SHOW GLOBAL VARIABLES LIKE 'selective_trace%'`).
- Reproduction steps. For crash bugs, the relevant error-log excerpt.

Sensitive **security** matters: contact the maintainer privately before
opening a public issue (see [docs/SECURITY.md](docs/SECURITY.md)).

## Development environment

The whole flow runs in Docker (no host dependency besides Docker):

```bash
./scripts/setup-dev-env.sh          # build image + 11.4.4 source
./scripts/build.sh full             # first time (full server build)
./scripts/build.sh --plugin         # incremental (plugin only, with ccache)
```

Per-platform/series build details (Ubuntu, EL8/EL9, MariaDB 12.3+) in
[README.md](README.md) and [docs/USAGE.md](docs/USAGE.md).

## Before opening a Pull Request

Run the full battery — everything must pass:

```bash
# 1. Filter-logic unit tests (no MariaDB)
g++ -std=c++11 -Wall -Wextra -Werror -I src \
    tests/test_filter_logic.cc src/filter_engine.cc -o /tmp/tfl && /tmp/tfl

# 2. MTR integration suite (official MariaDB format)
./scripts/run-mtr.sh                 # copies src/mysql-test/ into the source tree and runs

# 3. No memory leaks
docker exec -i mariadb-plugin-dev bash < scripts/valgrind-test.sh

# 4. Security sanity (on a clean OL9)
docker run --rm -i -v "$PWD/build/plugin_output-ol8:/plugin_out:ro" \
    oraclelinux:9 bash < scripts/security-test.sh
```

## Code style

- **C++11**, following the existing code style (see
  [docs/DECISIONS.md](docs/DECISIONS.md), D1 — Portuguese).
- The pure filter/parsing logic lives in `src/filter_engine.{h,cc}` and does
  **not** depend on MariaDB headers — so it stays standalone-testable. Every
  new filter/classification rule goes here, with tests in
  `tests/test_filter_logic.cc`.
- No C++ exception may cross the `extern "C"` boundaries (audit callback,
  sysvar callbacks, writer thread) — use guards, as the current code does
  (see D16).
- GPLv2 license header in every new source file (copy from an existing one).
- Document non-obvious design/ABI decisions in `docs/DECISIONS.md`.

## Sign-off (DCO)

Sign your commits (`git commit -s`) certifying the
[Developer Certificate of Origin](https://developercertificate.org/). For an
eventual upstream submission to MariaDB Server, the foundation may also
require the [MCA](https://mariadb.com/kb/en/mca/).
