# GitHub repository metadata — to paste into the UI

These fields are **not** versioned in the repository; they are configured on
the GitHub page. Copy from here.

## 1. "About" description

> Gear icon ⚙️ next to "About", top right of the repo page. Limit: 350 chars.

**English** (recommended for reach in the MariaDB community):

```
Selective query trace plugin for MariaDB 11.4/12.3+ — trace only the schemas/tables you care about, unlike general_log (all-or-nothing). Low overhead, hot-configurable via SET GLOBAL, JSON-file or table output.
```

**Portuguese:**

```
Trace seletivo de queries para MariaDB 11.4/12.3+: rastreia só os schemas/tabelas que você quer, ao contrário do general_log (tudo-ou-nada). Baixo overhead, ativação a quente por SET GLOBAL, saída em JSON ou tabela.
```

## 2. Topics (tags)

> Same "About" panel, "Topics" field. Paste separated by space/Enter.

```
mariadb
mariadb-plugin
query-trace
observability
database-monitoring
audit-plugin-api
oracle-linux
cpp
gplv2
```

## 3. Website (optional)

"Website" field in the About panel — can point to the usage docs:

```
https://github.com/<owner>/<repo>/blob/main/docs/USAGE.md
```

## 4. Renaming the repository (optional, recommended)

For full name consistency (`...-log-...` → `...-trace-...`):

- **Settings → General → Repository name**: from
  `mariadb-selective-log-plugin` to `mariadb-selective-trace-plugin`.
- GitHub creates an **automatic redirect** from the old name — existing clones
  and links keep working.
- After renaming, update the CI badge URL in `README.md` (the only reference
  to the old repo name in the versioned code).

## 5. Initial release (when publishing)

- Create a `v0.7.0` tag and a GitHub release.
- Attach the binaries for the validated platforms:
  - `selective_trace-11.4-el.so` (EL8 build, from `plugin_output-ol8/`)
  - `selective_trace-12.3-el.so` (EL9/12.3 build, from `plugin_output-123-ol9/`)
  - `selective_trace-11.4-ubuntu.so` (from `plugin_output/`)
- Release body: summary + links to `docs/USAGE.md` and `docs/SECURITY.md`.
