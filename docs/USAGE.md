# USAGE.md — Guia de uso do plugin `selective_log`

Plugin de auditoria seletiva para **MariaDB 11.4** que loga apenas queries
que tocam schemas/tabelas configurados — uma alternativa de baixo overhead ao
`general_log`.

---

## 1. Instalação

Copie `selective_log.so` para o `plugin_dir` do servidor (confira com
`SHOW GLOBAL VARIABLES LIKE 'plugin_dir'`) e:

```sql
INSTALL PLUGIN selective_log SONAME 'selective_log.so';
```

Ou via configuração (carrega no startup):

```ini
[mysqld]
plugin-load-add=selective_log.so
# o plugin declara maturity "experimental"; libere se o servidor usar o
# default (gamma):
plugin-maturity=experimental
```

Para remover:

```sql
UNINSTALL PLUGIN selective_log;
```

> O plugin **não** exige `general_log=ON` — os eventos de audit são
> gerados pelo servidor independentemente do general log.

## 2. Variáveis de sistema

Todas dinâmicas (`SET GLOBAL`), sem restart:

| Variável | Tipo | Default | Descrição |
|---|---|---|---|
| `selective_log_enabled` | BOOL | `OFF` | Liga/desliga a captura |
| `selective_log_schemas_to_log` | VARCHAR | `''` | Lista de schemas separados por vírgula |
| `selective_log_tables_to_log` | VARCHAR | `''` | Lista `schema.tabela` separada por vírgula (cross-schema); `schema.*` = todo o schema |
| `selective_log_output` | ENUM | `FILE` | `FILE` (JSON por linha) ou `TABLE` (`mysql.selective_log_events`) |
| `selective_log_log_file_path` | VARCHAR | `selective_log.json` | Arquivo de log no modo FILE (relativo = datadir) |
| `selective_log_min_duration_ms` | INT | `0` | Só loga queries mais lentas que N ms (0 = todas) |

### Semântica dos filtros

- **Ambas as listas vazias ⇒ nada é logado** (fail-safe: o plugin nunca vira
  um general_log acidental).
- Query loga se: **alguma tabela tocada** casa com `tables_to_log`, **ou** a
  tabela/schema tocado ou o **schema corrente da sessão** casa com
  `schemas_to_log`.
- `JOIN` multi-tabela: basta **uma** tabela casar para logar (o registro traz
  todas as tabelas tocadas).
- Matching **case-insensitive** (ASCII); backticks opcionais são aceitos.
- Statements que não tocam tabela (`SET`, `SHOW`, `SELECT 1`) só são logados
  se o schema corrente da sessão (`USE ...`) casar com o filtro de schemas.
- Valores inválidos são rejeitados na hora do `SET GLOBAL`:

```
SET GLOBAL selective_log_tables_to_log='nodot';
ERROR 1231: selective_log: invalid entry 'nodot' in tables_to_log
            (expected schema.table or schema.*)
```

### Exemplos

```sql
-- Auditar tudo que tocar o schema de produção "vendas"
SET GLOBAL selective_log_schemas_to_log = 'vendas';
SET GLOBAL selective_log_enabled = ON;

-- Auditar só duas tabelas sensíveis, independente do schema da sessão
SET GLOBAL selective_log_schemas_to_log = '';
SET GLOBAL selective_log_tables_to_log = 'rh.salarios,financeiro.pagamentos';

-- Todo o schema "logs" + uma tabela avulsa
SET GLOBAL selective_log_tables_to_log = 'logs.*,app.pedidos';

-- Só queries lentas (>250ms) do schema app
SET GLOBAL selective_log_schemas_to_log = 'app';
SET GLOBAL selective_log_min_duration_ms = 250;
```

## 3. Modo FILE (default)

Uma linha JSON por evento em `selective_log_log_file_path`:

```json
{"ts":"2026-07-04 03:33:44.401","conn_id":4,"query_id":7,
 "user":"root@localhost","db":"testdb","tables":["testdb.t1"],
 "command":"SELECT","duration_ms":0.391,"error_code":0,
 "query":"SELECT * FROM testdb.t1"}
```

Campos:

| Campo | Significado |
|---|---|
| `ts` | Timestamp local com milissegundos |
| `conn_id` | `connection_id` da sessão |
| `query_id` | Id interno do statement (correlaciona com a tabela de log) |
| `user` | `usuario@host` |
| `db` | Schema corrente da sessão (vazio se não houver `USE`) |
| `tables` | Tabelas tocadas pelo statement (`schema.tabela`) |
| `command` | Primeira palavra-chave do SQL (`SELECT`, `INSERT`, `CREATE`...) |
| `duration_ms` | Duração com precisão de ms (`null` se o início não foi visto) |
| `error_code` | 0 = sucesso; senão o código do erro (ex.: 1146) |
| `query` | Texto completo da query |

Notas:
- Tabelas internas de bookkeeping de estatísticas (`mysql.table_stats`,
  `mysql.column_stats`, `mysql.index_stats`, `mysql.innodb_table_stats`,
  `mysql.innodb_index_stats`) são tocadas como efeito colateral de DML comum
  e **não** entram em `tables` — a menos que estejam explicitamente em
  `selective_log_tables_to_log`.
- `command` ignora comentários iniciais de todos os sabores (`-- `, `#`,
  `/* */`, `/*! */`, `/*M! */`) e parênteses — um `INSERT` enviado com
  comentário anexado (comportamento padrão do DBeaver) classifica como
  `INSERT`. O campo `query` preserva o texto exato recebido, comentários
  incluídos (fidelidade de auditoria, como o general_log).
- Se um statement tocar tabelas demais para o buffer por conexão (~3,9 KB de
  nomes), o JSON ganha `"tables_truncated":true` (na tabela de log, a lista
  termina em `,...`).
- Statements dentro de stored procedures/functions geram eventos próprios
  (um por sub-statement, com suas tabelas), além do evento do `CALL`.
- O arquivo não tem rotação por tamanho — use logrotate/Fluentd/Filebeat.
- O caminho é reaberto automaticamente ao mudar
  `selective_log_log_file_path`.

## 4. Modo TABLE

```sql
SET GLOBAL selective_log_output = 'TABLE';
```

Os eventos são inseridos em **`mysql.selective_log_events`** (criada
automaticamente no primeiro uso):

```sql
CREATE TABLE mysql.selective_log_events (
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
  KEY idx_selective_log_ts (ts)
) ENGINE=Aria TRANSACTIONAL=0 DEFAULT CHARSET=utf8mb4;
```

Como funciona por baixo:
- A escrita é **assíncrona**: uma thread interna do plugin consome uma fila
  (até 10000 eventos) e executa os INSERTs numa conexão interna com
  `sql_log_bin=0` (não replica). Eventos podem levar alguns ms para aparecer.
- Se a fila encher (burst maior que a vazão de INSERT), eventos são
  descartados e contados em `Selective_log_events_dropped`.
- O plugin **nunca loga os próprios INSERTs** (guard de reentrância por
  thread) — sem loop de auto-log, mesmo com `mysql` no filtro.
- Se a tabela for dropada, é recriada no INSERT seguinte.

## 5. Status (`SHOW GLOBAL STATUS LIKE 'selective_log%'`)

| Variável | Significado |
|---|---|
| `Selective_log_events_logged` | Eventos aceitos (escritos ou enfileirados) |
| `Selective_log_write_failures` | Falhas de escrita (arquivo + tabela) |
| `Selective_log_events_dropped` | Eventos descartados por fila cheia (modo TABLE) |

## 6. Limitações conhecidas

- `duration_ms` é medido pelo próprio plugin (clock monotônico entre o
  início do dispatch e o fim do statement); se o plugin for habilitado no
  meio de um statement, o primeiro evento sai com `duration_ms` nulo.
- Com query cache ativo (OFF por default no 11.4), SELECTs servidos do cache
  não geram eventos de tabela.
- O filtro por tabela usa os eventos de lock por statement; comandos que não
  tocam tabelas dependem do filtro de schema da sessão.
- Identificadores com `.` ou `,` no nome não são suportados nas listas.
- Matching de identificadores é ASCII case-insensitive (não usa collation).
