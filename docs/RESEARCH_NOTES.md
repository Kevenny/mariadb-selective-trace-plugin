# RESEARCH_NOTES.md — Etapa 0: Pesquisa no código-fonte do MariaDB 11.4.4

> Todas as citações abaixo foram extraídas do fonte real em `/opt/mariadb-src`
> (tag git `mariadb-11.4.4`, verificada com `git describe --tags`).
>
> Arquivos lidos por completo: `include/mysql/plugin.h` (806 linhas),
> `include/mysql/plugin_audit.h` (181 linhas),
> `plugin/server_audit/server_audit.c` (3130 linhas),
> `plugin/audit_null/audit_null.c` (213 linhas), `cmake/plugin.cmake`,
> `include/mysql/service_logger.h`, `include/mysql/service_sql.h`,
> `include/mysql/service_thd_specifics.h`, e trechos de `sql/sql_audit.h`,
> `sql/log.cc`, `sql/handler.cc`.

---

## 1. Assinatura real da `st_mysql_audit` no 11.4.4

Trecho literal de `include/mysql/plugin_audit.h`:

```c
#define MYSQL_AUDIT_CLASS_MASK_SIZE 1

#define MYSQL_AUDIT_INTERFACE_VERSION 0x0302

struct st_mysql_audit
{
  int interface_version;
  void (*release_thd)(MYSQL_THD);
  void (*event_notify)(MYSQL_THD, unsigned int, const void *);
  unsigned long class_mask[MYSQL_AUDIT_CLASS_MASK_SIZE];
};
```

- `interface_version` deve ser `MYSQL_AUDIT_INTERFACE_VERSION` (**0x0302** nesta versão).
- `event_notify` recebe `(THD*, event_class, ponteiro para struct do evento)`.
- `release_thd` pode ser `NULL` (o `server_audit` passa `NULL`).
- `class_mask` é a máscara de classes de evento que queremos receber.

E de `include/mysql/plugin.h`, as constantes do plugin genérico:

```c
#define MYSQL_PLUGIN_INTERFACE_VERSION 0x0104   /* MySQL  */
#define MARIA_PLUGIN_INTERFACE_VERSION 0x010f   /* MariaDB */
#define MYSQL_AUDIT_PLUGIN           5
```

A declaração MariaDB-nativa usa `maria_declare_plugin(nome) { ... } maria_declare_plugin_end;`
com a struct `st_maria_plugin` (campos extras `version_info` e `maturity`).
O `server_audit` declara **ambas** (`mysql_declare_plugin` + `maria_declare_plugin`)
por compatibilidade com MySQL; para o nosso plugin, **somente MariaDB**, basta a
`maria_declare_plugin` (o `audit_null` também declara só uma).

## 2. Classes e eventos de audit disponíveis (e quais vamos usar)

`plugin_audit.h` do 11.4.4 define **3 classes** (não existe
`MYSQL_AUDIT_TABLE_ACCESS_CLASS` no MariaDB — isso é nomenclatura do MySQL 5.7+;
aqui a classe equivalente chama-se `MYSQL_AUDIT_TABLE_CLASS`):

| Classe | Valor | Subclasses | Structs |
|---|---|---|---|
| `MYSQL_AUDIT_GENERAL_CLASS` | 0 | `LOG` (0), `ERROR` (1), `RESULT` (2), `STATUS` (3), `WARNING` (4) | `mysql_event_general` |
| `MYSQL_AUDIT_CONNECTION_CLASS` | 1 | `CONNECT`, `DISCONNECT`, `CHANGE_USER` | `mysql_event_connection` |
| `MYSQL_AUDIT_TABLE_CLASS` | 15 | `LOCK` (0), `CREATE` (1), `DROP` (2), `RENAME` (3), `ALTER` (4) | `mysql_event_table` |

Structs relevantes (literais do header):

```c
struct mysql_event_general
{
  unsigned int event_subclass;
  int general_error_code;
  unsigned long general_thread_id;
  const char *general_user;          /* formato "user[user] @ host [ip]" */
  unsigned int general_user_length;
  const char *general_command;       /* "Query", "Execute", "Init DB"... */
  unsigned int general_command_length;
  const char *general_query;
  unsigned int general_query_length;
  const struct charset_info_st *general_charset;
  unsigned long long general_time;
  unsigned long long general_rows;
  /* Added in version 0x302 */
  unsigned long long query_id;
  MYSQL_CONST_LEX_STRING database;   /* schema corrente da sessão */
};

struct mysql_event_table
{
  unsigned int event_subclass;
  unsigned long thread_id;
  const char *user;
  const char *priv_user;
  const char *priv_host;
  const char *external_user;
  const char *proxy_user;
  const char *host;
  const char *ip;
  MYSQL_CONST_LEX_STRING database;   /* schema da TABELA (não da sessão) */
  MYSQL_CONST_LEX_STRING table;      /* nome da tabela */
  MYSQL_CONST_LEX_STRING new_database;  /* só para RENAME */
  MYSQL_CONST_LEX_STRING new_table;     /* só para RENAME */
  int read_only;      /* p/ TABLE_LOCK: 1 = leitura, 0 = leitura/escrita */
  /* Added in version 0x302 */
  unsigned long long query_id;
};
```

### Decisão: usaremos `MYSQL_AUDIT_TABLE_CLASS` + `MYSQL_AUDIT_GENERAL_CLASS`

**Por quê (confirmado lendo os pontos de despacho no servidor):**

1. **`TABLE_CLASS` / subclasse `LOCK`** é emitida em `sql/handler.cc:7487`
   (`mysql_audit_external_lock`, chamada de `handler::ha_external_lock`) —
   **uma vez por tabela afetada, por statement**, com `database` + `table`
   resolvidos pelo parser/optimizer. Um `JOIN` de N tabelas gera N eventos
   TABLE_LOCK com o mesmo `query_id`. É exatamente o que precisamos para o
   filtro `schema.tabela` cross-schema, sem parsing manual de SQL.
   Comentário do próprio header:
   > *"LOCK occurs when a connection 'locks' (this does not necessarily mean a
   > table lock and also happens for row-locking engines) the table at the
   > beginning of a statement. This event is generated at the beginning of
   > every statement for every affected table..."*

   As subclasses `CREATE/DROP/RENAME/ALTER` cobrem DDL por tabela.

2. **`GENERAL_CLASS` / subclasse `STATUS`** é emitida ao **final** de cada
   comando (`sql/sql_parse.cc:1920` e `:2446`, fim de `dispatch_command`),
   carregando `general_error_code`, o texto completo da query
   (`thd->query_string`) e `database` = schema corrente da sessão. É o momento
   certo para **escrever** o registro no log: a query terminou, sabemos o
   status e podemos correlacionar com os TABLE_LOCK acumulados durante o
   statement (mesmo `query_id`).

3. O `server_audit` faz o mesmo: só loga query na combinação
   `GENERAL_STATUS` + comando `Query/Execute` (linhas 2197–2203 do
   `server_audit.c`), porque *"Only one subclass is logged"* — as outras
   subclasses (LOG, RESULT) duplicariam o evento.

**Arquitetura de captura resultante (para as Etapas 2–3):**

- Callback recebe `TABLE_LOCK` → testa filtro (schema da tabela / schema.tabela).
  Se casar, marca flag + acumula nome(s) de tabela no estado por-conexão.
- Callback recebe `GENERAL_STATUS` (comando Query/Execute) → se a flag do
  statement estiver marcada **ou** o `event->database` casar com o filtro de
  schemas, monta o registro (com error code, duração, tabelas acumuladas) e
  escreve. Depois limpa o estado do statement.
- `class_mask = MYSQL_AUDIT_GENERAL_CLASSMASK | MYSQL_AUDIT_TABLE_CLASSMASK`.

### Ponto crítico verificado: audit dispara mesmo com `general_log=OFF`

Em `sql/log.cc` (`LOGGER::general_log_write`, linha ~1335):

```c
  mysql_audit_general_log(thd, hrtime_to_time(current_time), ...);  /* SEMPRE */

  if (opt_log && log_command(thd, command))   /* general_log só depois */
  {
    ...
  }
```

O hook de audit é chamado **antes** do teste `opt_log` — nosso plugin funciona
com o `general_log` desligado, que é o cenário-alvo.

### Limitação documentada: duração da query

Em `sql/sql_audit.h`, o evento `STATUS` preenche `event.general_time= my_time(0)`
— ou seja, é o **timestamp do fim** do comando, em segundos (`time_t`), e não a
duração. O evento `GENERAL_LOG` (início do comando) recebe
`hrtime_to_time(current_time)` — também truncado para segundos.

**Consequência**: a API não entrega duração pronta nem timestamps com
sub-segundo. Solução adotada: registrar nós mesmos um clock monotônico de
microssegundos (`my_interval_timer()`/`microsecond_interval_timer()` de
`my_global.h`, ou `clock_gettime`) no **primeiro evento do statement**
(TABLE_LOCK ou GENERAL_LOG) guardado no estado por-conexão, e calcular o delta
no `GENERAL_STATUS`. Precisão de ms garantida para `selective_trace_min_duration_ms`.

## 3. Como o `server_audit` estrutura variáveis de sistema dinâmicas

Padrão observado (literal, linhas 375–380 e 427–428):

```c
static MYSQL_SYSVAR_STR(incl_users, incl_users, PLUGIN_VAR_RQCMDARG,
       "Comma separated list of users to monitor.",
       check_incl_users, update_incl_users, NULL);
...
static MYSQL_SYSVAR_STR(file_path, file_path, PLUGIN_VAR_RQCMDARG,
       "Path to the log file.", NULL, update_file_path, default_file_name);
```

- Todas registradas num array `static struct st_mysql_sys_var* vars[]` passado
  na declaração do plugin. O prefixo do nome (`server_audit_`) vem do nome do
  plugin — as nossas serão automaticamente `selective_trace_*`.
- **`check` func** (opcional): valida e copia o valor para `save` (para STR,
  `value->val_str()` retorna memória temporária do statement — a doc do
  `plugin.h` manda copiar se precisar persistir).
- **`update` func**: roda com o valor já validado; é aqui que o `server_audit`
  faz o parse da lista (`user_coll_fill`) **segurando o write-lock**:

```c
static void update_incl_users(MYSQL_THD thd, ..., const void *save)
{
  char *new_users= (*(char **) save) ? *(char **) save : empty_str;
  ...
  mysql_prlock_wrlock(&lock_operations);
  ...
  memcpy(incl_user_buffer, new_users, new_len - 1);
  ...
  user_coll_fill(&incl_user_coll, incl_users, &excl_user_coll, 1);
  ...
  mysql_prlock_unlock(&lock_operations);
}
```

- A lista parseada vira um array **ordenado** (`qsort`) consultado com
  `bsearch` no hot path (`coll_search`) sob **read-lock**
  (`mysql_prlock_rdlock`) — exatamente o padrão rwlock exigido no nosso
  CLAUDE.md §4.5. Usaremos `mysql_rwlock_t` (API pública p/ plugins) ou
  `mysql_prlock_t` como o server_audit.
- Strings de sysvar **sem** `PLUGIN_VAR_MEMALLOC` são gerenciadas apontando
  para buffers estáticos do plugin (`incl_user_buffer[1024]`); com
  `PLUGIN_VAR_MEMALLOC` o servidor aloca/libera a cópia (usado no
  `syslog_info`). Para nossas listas usaremos `PLUGIN_VAR_MEMALLOC` +
  estruturas parseadas próprias (mais simples e sem limite de 1024 bytes).
- Enum: `MYSQL_SYSVAR_ENUM` + `TYPELIB` (ver `output_type`, linhas 413–426) —
  será o padrão do nosso `selective_trace_output` (`FILE`/`TABLE`).
- Estado por conexão: o `server_audit` usa um hack com
  `MYSQL_THDVAR_STR(loc_info, PLUGIN_VAR_NOSYSVAR|PLUGIN_VAR_NOCMDOPT|PLUGIN_VAR_MEMALLOC, ...)`
  guardando uma `struct connection_info` serializada como "string" thd-local.
  Alternativa mais limpa disponível no 11.4: **`service_thd_specifics.h`**
  (`thd_key_create` / `thd_getspecific` / `thd_setspecific`, sem mutex,
  lookup em array). Decisão final em `DECISIONS.md` na Etapa 1.

## 4. Como o `server_audit` escreve em arquivo e faz rotação

- Usa o **logger service** (`include/mysql/service_logger.h`) — serviço
  oficial exportado para plugins dinâmicos (`mysqlservices`):

```c
LOGGER_HANDLE* logger_open(const char *path, unsigned long long size_limit,
                           unsigned int rotations);
int logger_close(LOGGER_HANDLE *log);
int logger_write(LOGGER_HANDLE *log, const char *buffer, size_t size);
int logger_rotate(LOGGER_HANDLE *log);
```

  O serviço já implementa **rotação por tamanho** (`logfile.1`, `.2`, ...) e é
  **thread-safe** (mutex interno). Doc do header: *"The access is secured with
  the mutex, so the log is threadsafe."*
- `write_log()` do server_audit (linhas 1347–1385): pega `rdlock` para
  escrita normal; se é hora de rotacionar, troca para `wrlock` e rotaciona.
  O lock do plugin protege o handle (`logfile`) contra `stop/start_logging`
  concorrentes das update-funcs, não os bytes (isso é do mutex do service).
- A mensagem é montada num buffer local de 2KB na stack
  (`char message_loc[2048]`), com `malloc` de um buffer maior apenas quando a
  query excede o espaço (`log_statement_ex`, linhas 1845–1857) — bom padrão de
  "zero alocação no caminho comum" a copiar.
- Detalhe: o server_audit em build dinâmica **embute** `file_logger.c` via
  `#include "../../mysys/file_logger.c"` para rodar em MySQL/versões antigas.
  Nós **não** precisamos disso — em MariaDB 11.4 o logger service resolve
  (link com `mysqlservices` é automático via `MYSQL_ADD_PLUGIN`).

## 5. Prevenção de loop de auto-log (crítico para modo TABLE)

- `server_audit` mantém `static volatile int internal_stop_logging` — setado
  (com mutex `lock_atomic`) em volta de qualquer código do próprio plugin que
  possa gerar eventos (ex.: `my_printf_error` das update-funcs). O callback
  `auditing()` retorna imediatamente se a flag está ligada (linha 2146:
  `if (!thd || internal_stop_logging) return;`).
- Para o nosso modo TABLE, além de uma flag análoga, o filtro fail-safe já
  ajuda: os INSERTs internos serão na nossa tabela de log
  (`mysql.selective_trace_events` ou schema próprio) — basta o `should_log_event`
  ignorar sempre eventos cujo alvo é a própria tabela de log.

## 6. Execução de SQL interno por plugin (modo TABLE)

O MariaDB 11.4 tem o **SQL service** oficial (`include/mysql/service_sql.h`,
desde 10.5), com exemplo de uso em `plugin/test_sql_service/test_sql_service.c`:

```c
MYSQL *mysql= mysql_init(NULL);
if (mysql_real_connect_local(mysql) == NULL) ...  /* conexão interna */
mysql_real_query(mysql, STRING_WITH_LEN("CREATE TABLE test.ts_table ..."));
mysql_close(mysql);
```

Doc do header sobre `mysql_real_connect_local`:
> *"The established connection has no user/host associated to it, neither it
> has the current db, so the queries should have database/table name specified."*

- Todos os nomes devem ser qualificados (`schema.tabela`) — ok para nós.
- **Atenção (verificar na Etapa 4)**: a conexão local executa o pipeline SQL
  completo, então os INSERTs internos devem disparar eventos de audit → é
  obrigatório o guard de reentrância da seção 5 (flag + ignorar a própria
  tabela de log).
- A criação automática da tabela de log no `init` do plugin deve respeitar o
  aviso do `plugin.h`: *"Plugin initialisation done here should defer any
  ALTER TABLE queries to after the ddl recovery is done"* — criaremos a tabela
  de forma **lazy** (no primeiro evento com `output=TABLE`, ou na update-func
  da variável), não no `init`.

## 7. Padrão de build (CMake)

`plugin/server_audit/CMakeLists.txt` (completo, 18 linhas):

```cmake
SET(SOURCES server_audit.c test_audit_v4.c plugin_audit_v4.h)
MYSQL_ADD_PLUGIN(server_audit ${SOURCES} MODULE_ONLY RECOMPILE_FOR_EMBEDDED)
```

`plugin/audit_null/CMakeLists.txt`:

```cmake
MYSQL_ADD_PLUGIN(audit_null audit_null.c
  MODULE_ONLY MODULE_OUTPUT_NAME "adt_null" COMPONENT Test)
```

Assinatura da macro (`cmake/plugin.cmake`):

```
MYSQL_ADD_PLUGIN(plugin_name source1...sourceN
  [STORAGE_ENGINE] [STATIC_ONLY|MODULE_ONLY] [MANDATORY|DEFAULT] [DISABLED]
  [NOT_EMBEDDED] [RECOMPILE_FOR_EMBEDDED] [CLIENT]
  [MODULE_OUTPUT_NAME name] [COMPONENT component] [CONFIG cnf_file_name]
  [VERSION version] [LINK_LIBRARIES lib1...libN] [DEPENDS target1...targetN])
```

- **`MODULE_ONLY`** → compila somente como `.so` dinâmica (nosso caso; atende
  o requisito `DYNAMIC` e o `-DPLUGIN_SELECTIVE_TRACE=DYNAMIC` do build.sh).
- Plugins `MODULE_ONLY` ganham `TARGET_LINK_LIBRARIES(target mysqlservices ...)`
  automaticamente (linha 215 do plugin.cmake) → logger service e sql service
  disponíveis sem config extra.
- O define `MYSQL_DYNAMIC_PLUGIN` é adicionado automaticamente para builds
  dinâmicas (é ele que ativa os wrappers `#define logger_open ...` dos services).
- Nosso `src/CMakeLists.txt` será essencialmente:
  `MYSQL_ADD_PLUGIN(selective_trace <fontes> MODULE_ONLY)`.

## 8. Observações do `audit_null.c` (exemplo mínimo)

- Confirma o esqueleto mínimo: descriptor `st_mysql_audit` + callback + masks;
  `init`/`deinit` triviais; status vars via `st_mysql_show_var`.
- Mostra acesso direto a `event_table->database.str` / `table.str` — sem
  parsing de SQL.
- Nota: campos `user`/`host`/`ip` de `mysql_event_table` podem ser `NULL`
  (o server_audit usa `SAFE_STRLEN` em todos) — copiar essa defensividade.

## 9. Riscos/armadilhas anotados para as próximas etapas

1. **Query cache**: `server_audit_init` alerta que com query cache ativo os
   eventos TABLE podem ser "veiled" (resultado servido do cache não gera
   TABLE_LOCK). No 11.4 o query cache é OFF por default; documentar em USAGE.md.
2. **`event->general_user` no GENERAL_CLASS** vem no formato
   `"user[user] @ host [ip]"` (montado por `make_user_name`) — o server_audit
   tem um parser (`get_user_host`). Nos eventos TABLE, `user`/`host`/`ip` já
   vêm separados. Preferir capturar identidade nos eventos TABLE/da conexão.
3. **Statements sem acesso a tabela** (ex.: `SET`, `SHOW STATUS`) nunca geram
   TABLE_LOCK — só serão logados se o filtro de *schema* casar com o schema
   corrente da sessão (comportamento a documentar).
4. **PREPARE/EXECUTE**: comando vem como `"Execute"` no GENERAL_STATUS; os
   TABLE_LOCK disparam normalmente por execução — cobertos.
5. **`database` no `mysql_event_general` é o schema da sessão**, não
   necessariamente o schema dos objetos da query — por isso o filtro por
   tabela usa exclusivamente os eventos TABLE.
6. `MYSQL_CONST_LEX_STRING` (= `{const char *str; size_t length;}`) não é
   NUL-terminated garantido — sempre usar `length`.

## 10. Mapa de decisão para a Etapa 1

| Item | Decisão preliminar (detalhar em DECISIONS.md) |
|---|---|
| Tipo de plugin | `MYSQL_AUDIT_PLUGIN`, só `maria_declare_plugin` |
| class_mask | `GENERAL_CLASSMASK \| TABLE_CLASSMASK` |
| Evento de escrita | `GENERAL_STATUS` (comando Query/Execute) |
| Evento de filtro por tabela | `TABLE_LOCK` (+ CREATE/DROP/RENAME/ALTER) |
| Escrita FILE | logger service (`service_logger.h`), JSON por linha |
| Escrita TABLE | SQL service (`service_sql.h`), criação lazy, guard de reentrância |
| Lock das listas de filtro | rwlock (rdlock no hot path), padrão server_audit |
| Estado por conexão | THDVAR-struct (server_audit) vs `thd_specifics` — avaliar na Etapa 1 |
| Duração ms | clock próprio no 1º evento do statement → delta no STATUS |
