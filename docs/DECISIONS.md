# DECISIONS.md — Decisões técnicas do plugin `selective_log`

Registro das decisões de design, com a justificativa e as referências no
código-fonte do MariaDB 11.4.4 que as embasam (ver também
[RESEARCH_NOTES.md](./RESEARCH_NOTES.md)).

---

## D1. Linguagem: C++ (arquivos `.cc`), estilo "C com classes"

**Contexto**: a Plugin ABI do MariaDB é C (`plugin.h` expõe structs C e a
declaração via macros `maria_declare_plugin`). Os dois plugins de auditoria de
referência lidos por completo (`plugin/server_audit/server_audit.c` e
`plugin/audit_null/audit_null.c`) são C puro. Por outro lado, boa parte dos
plugins mais novos da árvore é C++ (`plugin/type_uuid`, `plugin/versioning`,
`plugin/func_test`), e o core do servidor é C++.

**Decisão**: **C++**, pelos motivos:

1. O requisito de testes unitários standalone do `filter_engine`
   (CLAUDE.md §6, Etapa 2) fica trivial com `std::string`/`std::vector` e sem
   nenhuma dependência de headers do MariaDB — um `main()` simples compila com
   `g++` puro.
2. RAII para o estado por statement (containers de tabelas acumuladas) reduz
   risco de vazamento de memória (requisito §5 / Valgrind).
3. As macros da Plugin API (`maria_declare_plugin`, `MYSQL_SYSVAR_*`) são
   projetadas para funcionar em C++ — `plugin.h` faz `extern "C"` via
   `MYSQL_PLUGIN_EXPORT` (linhas 36–40 do header).
4. Custo zero de interop: o entrypoint continua sendo funções `static` com a
   assinatura C esperada pelos ponteiros da `st_mysql_audit`.

Restrições autoimpostas (para manter o binário simples e a ABI limpa):
sem exceções atravessando o callback de audit, sem RTTI, sem dependências além
da STL básica e dos services oficiais do MariaDB.

## D2. Declaração do plugin: somente `maria_declare_plugin`

O `server_audit` declara as duas structs (`mysql_declare_plugin` +
`maria_declare_plugin`) porque precisa carregar também em MySQL/Percona,
com gambiarras de detecção de versão em runtime (offsets hardcoded de THD em
`auditing_v8` etc.). Nosso escopo é **MariaDB 11.4** — declarar apenas
`maria_declare_plugin` (interface `0x010f`), como os plugins MariaDB-only da
árvore. Maturity inicial: `MariaDB_PLUGIN_MATURITY_EXPERIMENTAL`.

## D3. Eventos de audit: `TABLE_CLASS` para filtro, `GENERAL_STATUS` para escrita

Detalhado em RESEARCH_NOTES.md §2. Resumo:

- `TABLE_LOCK` (emitido por `handler::ha_external_lock` → `sql/handler.cc:7487`)
  entrega `database` + `table` por tabela afetada, já resolvidos pelo parser —
  filtro por tabela cross-schema sem parsing de SQL.
- `GENERAL_STATUS` (fim de `dispatch_command`, `sql/sql_parse.cc:1920/2446`)
  entrega error code + texto da query → momento único de escrita do registro.
- Statements que não tocam tabela (`SET`, `SHOW`) só casam pelo filtro de
  schema (schema corrente da sessão no `event->database`).

## D4. Variáveis de sistema

- Strings com `PLUGIN_VAR_MEMALLOC` (o servidor gerencia a cópia da string
  crua; diferente do server_audit, que usa buffers estáticos de 1024 bytes e
  herda o limite). As estruturas parseadas (listas de filtro) são nossas e
  protegidas por rwlock.
- `selective_log_output` como `MYSQL_SYSVAR_ENUM` + `TYPELIB`
  (`FILE`/`TABLE`), padrão idêntico ao `output_type` do server_audit.
- `min_duration_ms` como `MYSQL_SYSVAR_UINT` (range 0..2^31-1). O nome fala em
  "INT" na spec; UINT evita valores negativos sem sentido.
- Parsing das listas acontece na callback `update` (Etapa 2), sob write-lock,
  seguindo o padrão `update_incl_users` do server_audit.

## D5. Build: `MYSQL_ADD_PLUGIN(selective_log ... MODULE_ONLY)`

`MODULE_ONLY` = só build dinâmica (`.so`), atendendo o requisito de
`INSTALL PLUGIN ... SONAME`. A macro (cmake/plugin.cmake:215) linka
`mysqlservices` automaticamente para módulos dinâmicos → logger service e SQL
service disponíveis. O `build.sh` do projeto já passa
`-DPLUGIN_SELECTIVE_LOG=DYNAMIC`, coerente com o nome do target.

## D6. Nome do plugin em minúsculas (`selective_log`)

`INSTALL PLUGIN` e `SHOW PLUGINS` são case-insensitive; a spec e o README usam
`selective_log` minúsculo. Mantido assim na struct para a saída de
`SHOW PLUGINS` ficar igual à documentação.

## D7. Link do módulo: `RECOMPILE_FOR_EMBEDDED` para escapar do `--no-undefined`

Primeira tentativa de usar `mysql_rwlock_t` falhou no link: no Linux,
`cmake/plugin.cmake:234-235` adiciona `-Wl,--no-undefined` a módulos
dinâmicos, e símbolos PSI/mysys (`PSI_server`, `psi_rwlock_*`) e funções
`thd_*` só existem dentro do executável `mariadbd` (resolvidos em runtime).
Verificado com `nm -D server_audit.so`: o plugin oficial também tem esses
símbolos indefinidos — ele escapa porque usa `RECOMPILE_FOR_EMBEDDED`, que
cai no branch da linha 228 do plugin.cmake e **não** aplica `--no-undefined`.
Adotado o mesmo flag. Custo zero com `-DWITH_EMBEDDED_SERVER=OFF`.

## D8. Matching case-insensitive (ASCII) nos filtros

`lower_case_table_names=0` (default Linux) torna identificadores
case-sensitive, mas para um filtro de auditoria um falso positivo (logar
`TestDB` quando o filtro diz `testdb`) é inofensivo, enquanto um falso
negativo é perda de auditoria. As listas são normalizadas para minúsculas no
parse e comparadas com fold ASCII no hot path (sem alocação). Backticks
opcionais são aceitos por parte do identificador (`` `a`.`b` ``).

## D9. Estado por conexão: THDVAR-blob (padrão server_audit), não thd_specifics

O estado por statement (query_id, clock de início, tabelas acumuladas, flag
de match) é um POD `StatementState` guardado numa `MYSQL_THDVAR_STR` oculta
(`PLUGIN_VAR_NOSYSVAR|PLUGIN_VAR_NOCMDOPT|PLUGIN_VAR_MEMALLOC`) cujo default
é um blob sem NULs — o servidor faz `strdup` por THD e **libera sozinho** no
fim da conexão (zero risco de leak). Armadilha encontrada na prática: o blob
precisa estar preenchido **antes** do registro das sysvars (que acontece
antes do `init()` do plugin), senão o `strdup` copia uma string vazia e o
primeiro acesso corrompe o heap (`free(): invalid pointer` no startup — foi
um crash real durante a Etapa 3). Por isso o preenchimento vive num
construtor da shared library (`__attribute__((constructor))`), como o
`so_init()` do server_audit. `thd_specifics` foi descartado por exigir
liberação manual por THD (não há callback confiável de destruição de
conexão sem assinar a classe CONNECTION só para isso).

## D10. Duração: clock monotônico próprio (ns) entre GENERAL_LOG e GENERAL_STATUS

A API entrega `general_time` apenas em segundos (RESEARCH_NOTES §2). O
plugin grava `clock_gettime(CLOCK_MONOTONIC)` no evento `GENERAL_LOG`
(início do dispatch) e calcula o delta no `GENERAL_STATUS`. Se o início não
foi visto (plugin habilitado no meio de um statement), `duration_ms` sai
`null`/`NULL` — e, com `min_duration_ms > 0`, o evento **não** é logado
(sem medição não há como afirmar que passou do limiar).

## D11. Formato JSON (modo FILE)

Uma linha por evento, chaves fixas:

```json
{"ts":"2026-07-04 03:33:44.401","conn_id":4,"query_id":7,
 "user":"root@localhost","db":"testdb","tables":["testdb.t1"],
 "command":"SELECT","duration_ms":0.391,"error_code":0,
 "query":"SELECT * FROM testdb.t1"}
```

- `tables` = tabelas realmente tocadas pelo statement (via eventos
  TABLE_LOCK). Desde a v0.4.0, tabelas internas de bookkeeping de
  estatísticas são excluídas, salvo filtro explícito — ver D13.
- `command` = primeira palavra-chave do SQL (uppercase), `OTHER` se não
  identificável. Escrita via logger service, sem rotação por tamanho
  (retenção fica com logrotate externo).

## D12. Modo TABLE: thread dedicada + fila, nunca SQL no THD do usuário

Leitura de `sql/sql_prepare.cc:mysql_real_connect_local` mostrou que a
conexão local **reusa a THD corrente** quando ela existe e não tem locks —
rodar INSERT de dentro do callback de audit executaria SQL dentro da sessão
do usuário. Em vez disso os eventos viram INSERTs prontos numa fila
(limite 10000; excedente é descartado e contado em
`Selective_log_events_dropped`) consumida por uma thread própria do plugin.
Benefícios verificados no fonte: thread sem `current_thd` ganha THD interna
dedicada com `sql_log_bin=0` (não replica) e `skip_grants`; o loop de
auto-log é impossível porque o callback ignora eventos vindos da própria
thread (`table_writer_is_self()`). A tabela `mysql.selective_log_events`
(ENGINE=Aria) é criada lazy pela thread e recriada se sumir (errno
1146/1049), atendendo à restrição de não rodar DDL no `init()` do plugin.

## D13. Precisão da coleta (v0.4.0) — correções pós-uso real

Ajustes motivados por um INSERT via DBeaver logado com `command=OTHER`:

1. **Classificação de comando comment-aware**: o DBeaver (e outros clients)
   envia o statement com o comentário de linha anexado
   (`-- comentário\nINSERT ...`) — SQL válido que o servidor executa como
   texto único. O extrator de comando (`extract_command`, movido para o
   `filter_engine` para ser testável standalone) agora pula comentários
   `-- `, `#`, `/* */` e os executáveis `/*! */` e `/*M! */` (nesses dois,
   o conteúdo É o statement). O campo `query` continua fiel ao texto
   recebido — auditoria não reescreve o que o cliente mandou.
2. **Acumulação por statement, não por query_id**: sub-statements de stored
   routines avançam o `query_id` da THD; o reset por mudança de id podia
   descartar tabelas/match no meio de um `CALL`. O estado agora abre no
   `GENERAL_LOG` (início do dispatch) e fecha no `GENERAL_STATUS`
   (`in_statement`), acumulando todos os eventos TABLE no intervalo.
   Verificado na prática: cada sub-statement de SP também emite seu próprio
   STATUS e vira evento individual com suas tabelas — cobertura dupla.
3. **Exclusão de bookkeeping**: `mysql.{table,column,index}_stats` e
   `mysql.innodb_{table,index}_stats` são lockadas como efeito colateral de
   DML (EITS/InnoDB persistent stats) e poluíam `tables_involved`. Agora só
   contam se listadas explicitamente em `tables_to_log`.
4. **Truncamento visível**: buffer de tabelas por conexão subiu para ~3,9 KB
   e o estouro é sinalizado (`"tables_truncated":true` no JSON, `,...` na
   tabela) em vez de silencioso.
