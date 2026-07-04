# USAGE.md — Guia de uso do plugin `selective_log`

Plugin de auditoria seletiva para **MariaDB 11.4** que loga apenas queries
que tocam schemas/tabelas configurados — uma alternativa de baixo overhead ao
`general_log`.

---

## 0. Plataformas suportadas

O plugin é um binário nativo — use o `.so` compilado para a plataforma do
servidor:

| Plataforma do servidor MariaDB | Build |
|---|---|
| Ubuntu 22.04+/Debian (glibc ≥ 2.35) | `build/plugin_output/selective_log.so` (container `dev`) |
| Oracle Linux / RHEL / Rocky / Alma **8 e 9** | `build/plugin_output-ol8/selective_log.so` (container `dev-ol8`, glibc ≥ 2.17) |
| Windows | não suportado nesta versão (código usa POSIX; porte viável, ver README) |

O build EL8 requer só GLIBC_2.17+, então carrega em EL8, EL9 e distros mais
novas. **Validado em Oracle Linux 8 e Oracle Linux 9**, ambos com MariaDB
11.4.12 instalado via RPM oficial (mariadb.org), rodando o mesmo `.so` —
smoke test completo (FILE, TABLE, JOIN cross-schema, UNINSTALL/INSTALL) nos
dois. Compatível com qualquer servidor MariaDB **11.4.x**. Para outra série
(10.11, 11.8...), recompile contra o fonte da série correspondente.

Para validar em OL9, o mesmo script serve (só muda a imagem):

```bash
docker run --rm -i -v "$PWD/build/plugin_output-ol8:/plugin_out:ro" \
    oraclelinux:9 bash < scripts/validate-ol8.sh
```

Como gerar o build EL8:

```bash
docker compose -f docker/docker-compose.yml --profile ol8 up -d --build dev-ol8
docker exec mariadb-plugin-dev-ol8 bash -lc 'cd /workspace && ./scripts/build.sh full && ./scripts/build.sh --package'
# valida num OL8 limpo com MariaDB 11.4 via RPM oficial:
docker run --rm -i -v "$PWD/build/plugin_output-ol8:/plugin_out:ro" \
    oraclelinux:8 bash < scripts/validate-ol8.sh
```

No servidor de destino (OL8+ com MariaDB via RPM), o `plugin_dir` é
`/usr/lib64/mysql/plugin/` — copie o `.so` para lá.

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

### Filtro por tipo de comando (por entrada)

Toda entrada das duas listas aceita um qualificador opcional `:cmd1|cmd2`
restringindo **quais comandos** são logados para aquele schema/tabela:

```sql
-- schema "vendas" só INSERT e UPDATE; schema "rh" tudo
SET GLOBAL selective_log_schemas_to_log = 'vendas:insert|update, rh';

-- tabela app.pedidos só DELETE; todo o schema logs só DML
SET GLOBAL selective_log_tables_to_log = 'app.pedidos:delete, logs.*:dml';
```

Tokens válidos: `select`, `insert`, `update`, `delete`, `replace`, `load`,
`call`, `create`, `alter`, `drop`, `truncate`, `rename`, `other`, e os
grupos `dml` (insert|update|delete|replace|load), `ddl`
(create|alter|drop|truncate|rename) e `all`. Sem qualificador = todos os
comandos. Token desconhecido faz o `SET GLOBAL` falhar. Entradas duplicadas
têm as máscaras mescladas (`a:insert, a:update` ≡ `a:insert|update`).

O comando do statement é o mesmo do campo `command` (primeira palavra-chave
do SQL, ignorando comentários); `WITH` (CTE) conta como `select`. Statements
que não são classificáveis caem em `other`.

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
