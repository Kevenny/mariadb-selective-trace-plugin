# Metadados do repositório GitHub — para colar na interface

Estes campos **não** ficam versionados no repositório; são configurados na
página do GitHub. Copie daqui.

## 1. Descrição "About"

> Ícone de engrenagem ⚙️ ao lado de "About", no topo direito da página do repo.
> Limite: 350 caracteres.

**Português:**

```
Trace seletivo de queries para MariaDB 11.4/12.3+: rastreia só os schemas/tabelas que você quer, ao contrário do general_log (tudo-ou-nada). Baixo overhead, ativação a quente por SET GLOBAL, saída em JSON ou tabela.
```

**Inglês** (recomendado para alcance na comunidade MariaDB):

```
Selective query trace plugin for MariaDB 11.4/12.3+ — trace only the schemas/tables you care about, unlike general_log (all-or-nothing). Low overhead, hot-configurable via SET GLOBAL, JSON-file or table output.
```

## 2. Topics (tags)

> No mesmo painel "About", campo "Topics". Cole separados por espaço/Enter.

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

## 3. Website (opcional)

Campo "Website" no painel About — pode apontar para a doc de uso:

```
https://github.com/<owner>/<repo>/blob/main/docs/USAGE.md
```

## 4. Renomear o repositório (opcional, recomendado)

Para coerência total do nome (`...-log-...` → `...-trace-...`):

- **Settings → General → Repository name**: de
  `mariadb-selective-log-plugin` para `mariadb-selective-trace-plugin`.
- O GitHub cria um **redirect automático** do nome antigo — clones e links
  existentes continuam funcionando.
- Depois de renomear, atualize a URL do badge de CI no `README.md` (a única
  referência ao nome antigo do repo no código versionado).

## 5. Release inicial (quando for publicar)

- Criar tag `v0.7.0` e uma release no GitHub.
- Anexar os binários das plataformas validadas:
  - `selective_trace-11.4-el.so` (build EL8, de `plugin_output-ol8/`)
  - `selective_trace-12.3-el.so` (build EL9/12.3, de `plugin_output-123-ol9/`)
  - `selective_trace-11.4-ubuntu.so` (de `plugin_output/`)
- Corpo da release: resumo + link para `docs/USAGE.md` e `docs/SECURITY.md`.
