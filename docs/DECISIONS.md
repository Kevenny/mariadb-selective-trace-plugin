# DECISIONS.md — Decisões técnicas do plugin `selective_trace`

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
- `selective_trace_output` como `MYSQL_SYSVAR_ENUM` + `TYPELIB`
  (`FILE`/`TABLE`), padrão idêntico ao `output_type` do server_audit.
- `min_duration_ms` como `MYSQL_SYSVAR_UINT` (range 0..2^31-1). O nome fala em
  "INT" na spec; UINT evita valores negativos sem sentido.
- Parsing das listas acontece na callback `update` (Etapa 2), sob write-lock,
  seguindo o padrão `update_incl_users` do server_audit.

## D5. Build: `MYSQL_ADD_PLUGIN(selective_trace ... MODULE_ONLY)`

`MODULE_ONLY` = só build dinâmica (`.so`), atendendo o requisito de
`INSTALL PLUGIN ... SONAME`. A macro (cmake/plugin.cmake:215) linka
`mysqlservices` automaticamente para módulos dinâmicos → logger service e SQL
service disponíveis. O `build.sh` do projeto já passa
`-DPLUGIN_SELECTIVE_TRACE=DYNAMIC`, coerente com o nome do target.

## D6. Nome do plugin em minúsculas (`selective_trace`)

`INSTALL PLUGIN` e `SHOW PLUGINS` são case-insensitive; a spec e o README usam
`selective_trace` minúsculo. Mantido assim na struct para a saída de
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
`Selective_trace_events_dropped`) consumida por uma thread própria do plugin.
Benefícios verificados no fonte: thread sem `current_thd` ganha THD interna
dedicada com `sql_log_bin=0` (não replica) e `skip_grants`; o loop de
auto-log é impossível porque o callback ignora eventos vindos da própria
thread (`table_writer_is_self()`). A tabela `mysql.selective_trace_events`
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

## D14. Build multi-plataforma: ambiente dedicado EL8 (Oracle Linux 8+)

O `.so` é binário nativo: o build do container `dev` (Ubuntu 22.04) exige
glibc ≥ 2.35 e não carrega em EL8 (glibc 2.28). Em vez de tentar um binário
"universal", o projeto ganhou um segundo ambiente de build
(`docker/Dockerfile.ol8`, serviço `dev-ol8`, volumes de build/ccache
separados, mesmo volume de fonte): base `oraclelinux:8` com gcc-toolset-12
(o gcc 8.5 nativo do EL8 é antigo demais para o MariaDB 11.4). O binário
resultante exige apenas GLIBC_2.17 (verificado com `objdump -T`), cobrindo
EL8, EL9 e distros mais novas.

Validação real (`scripts/validate-ol8.sh`): container `oraclelinux:8` limpo,
MariaDB **11.4.12** instalado via RPM oficial (mariadb.org), plugin ACTIVE e
smoke test completo (FILE, TABLE, JOIN cross-schema, UNINSTALL/INSTALL) —
confirmando também compatibilidade dentro da série 11.4.x (build contra
11.4.4 rodando no 11.4.12).

Windows ficou fora do escopo desta versão: `log_writer_table` usa pthread,
os relógios são POSIX (`clock_gettime`/`gettimeofday`) e o blob da THDVAR é
preenchido em `__attribute__((constructor))`. Porte mapeado (std::thread/
std::chrono/DllMain, padrão do server_audit), mas exige toolchain MSVC para
compilar a árvore do MariaDB.

## D15. Filtro por tipo de comando (v0.5.0): qualificador por entrada

Requisito: coletar só INSERT (ou UPDATE, DELETE...) por schema **e/ou** por
tabela, com múltiplas entradas. Em vez de uma variável global de comandos
(que não daria granularidade por entrada), cada item das listas ganhou um
qualificador opcional `:cmd1|cmd2` (`vendas:insert|update`,
`app.pedidos:delete`, `logs.*:dml`), com grupos `dml`/`ddl`/`all` e merge de
máscaras em entradas duplicadas.

Mecânica: o match de tabela/schema não devolve mais booleano e sim a
**união das máscaras de comandos** das entradas que casaram (`CommandBits`,
16 bits). Como os eventos TABLE acontecem antes de o comando ser conhecido
com certeza, o estado por statement acumula a máscara permitida e a decisão
final acontece no `GENERAL_STATUS`: `extract_command()` → `command_bit()` →
loga se `(máscara_tabelas | máscara_schema_da_sessão) & bit`. Custo no hot
path inalterado (mesmos locks; a comparação vira um AND).

Limitação documentada: `WITH` (CTE) classifica como `select` — um
`WITH ... UPDATE` seria tratado como select para fins de filtro por comando.

## D16. Guards de exceção nas fronteiras C/C++ (v0.5.1)

O risco residual apontado na análise de riscos: o plugin usa `std::string`
na montagem do JSON/INSERT e no parse dos filtros; sob esgotamento de
memória, um `bad_alloc` atravessando a fronteira `extern "C"` viraria
`std::terminate` → abort do mariadbd inteiro. Todas as fronteiras ganharam
`try/catch (...)`:

1. `selective_trace_notify` (callback de eventos) — descarta o evento;
2. `check_*`/`update_*` das sysvars — o SET falha limpo / regras antigas
   permanecem ativas (o unlock do rwlock fica fora do try, sempre executa);
3. `table_writer_enqueue` — evento descartado e contado;
4. loop da thread do writer — a thread sobrevive a qualquer exceção de um
   INSERT individual.

Exceções engolidas são contadas em `Selective_trace_callback_errors`
(SHOW GLOBAL STATUS) — valor diferente de zero indica pressão de memória ou
bug a investigar, sem derrubar o servidor.

## D17. Segurança (v0.6.0): correções da bateria adversarial no OL9

Validação de segurança (scripts/security-test.sh) num oraclelinux:9 com
MariaDB 11.4.12 via RPM apontou 6/7 ok e um achado; corrigidos:

1. **Mascaramento de credenciais** (achado): o plugin loga o texto completo
   da query, expondo senhas de DCL (`IDENTIFIED BY`, `SET PASSWORD`,
   `PASSWORD()`, `IDENTIFIED WITH ... AS`). `mask_secrets()` (no
   filter_engine, testável) substitui esses literais por `***`, com
   fronteira de palavra (não afeta coluna `password_hash` nem
   `'my password'` em INSERT comum). Controlado por
   `selective_trace_mask_passwords` (default ON).
2. **sql_mode do writer** (defesa em profundidade): o escaping do INSERT no
   modo TABLE usa backslashes, inválido sob `NO_BACKSLASH_ESCAPES`. A
   conexão interna do writer passou a fixar `SET SESSION sql_mode=''` ao
   conectar, tornando o escaping independente do sql_mode global — fecha um
   vetor teórico de injeção de SQL na tabela de log.

Injeção de SQL (modo default), injeção de JSON/nova-linha e falha graciosa
de path já passavam. SELinux não é exercido no container de teste (kernel
sem SELinux); o procedimento de rótulo do .so e do path de log está em
docs/SECURITY.md para o host OL9 real com enforcing.

## D18. Suporte a MariaDB 12.3+ (OL9): duas mudanças de ABI

Investigação no fonte 12.3.2 (via build real, não suposição) revelou duas
quebras de ABI frente à 11.4, tratadas sem tocar na lógica do plugin:

1. **Audit interface 0x0302 → 0x0303**: as structs de evento ganharam
   campos (`port` em general/table/connection; `tls_version` em connection),
   inseridos **antes** de `database` em general/table — justamente os campos
   que o plugin lê. Como usamos o macro `MYSQL_AUDIT_INTERFACE_VERSION` (não
   o literal) e lemos os campos por nome, recompilar contra os headers 12.3
   resolve versão e offsets automaticamente. Consequência: o `.so` do 11.4
   (0x0302, struct menor) **não** é ABI-compatível com o servidor 12.3 — é
   preciso um build dedicado (novo serviço `dev-123-ol8`, fonte/volumes
   próprios, saída `build/plugin_output-123-ol9/`).

2. **Logger service**: `logger_open()` ganhou um 4º argumento `buffer_size`
   (o service passou a bufferizar internamente) e `logger_write` passou de
   `const char*` para `const void*`. Tratado com um wrapper
   `SELECTIVE_TRACEGER_OPEN` selecionado por `#if MYSQL_VERSION_ID >= 120000`
   (buffer_size=0 mantém o comportamento não-bufferizado); `line.data()`
   (`const char*`) converte implicitamente para `const void*`, então o
   `write` serve nas duas séries. O mesmo código-fonte compila em 11.4 e
   12.3 — confirmado recompilando ambos.

Matriz de artefatos: `plugin_output/` (Ubuntu/11.4), `plugin_output-ol8/`
(EL8+/11.4), `plugin_output-123-ol9/` (EL8+/12.3+). Cada série do servidor
precisa do binário compilado contra o fonte daquela série.

## D19. CI (GitHub Actions): unit rápido + integração matricial por série

Objetivo de comunidade: dar o selo "testado em CI" que faltava frente ao
server_audit. Estrutura em .github/workflows/ci.yml:

- **unit-tests** (gate rápido, todo push/PR): compila e roda os testes
  standalone do filter_engine com g++ — segundos, sem MariaDB.
- **integration** (matriz 11.4.4 × 12.3.2, gated a main/PR/schedule/manual):
  builda o servidor+plugin contra o fonte real de cada série (via
  Dockerfile.ol8 + build.sh) e roda a suíte MTR (run-mtr.sh), publicando o
  .so como artefato. É caro (~40 min/série), daí o gating e o cache de
  ccache entre execuções.

Por que build de fonte no CI, e não pacotes: verifiquei que `MariaDB-devel`
**não** inclui os headers de plugin de servidor (`plugin_audit.h` etc.) —
eles só existem na árvore de fonte. Portanto não há atalho: compilar o
plugin exige o fonte, como todo o setup Docker do projeto já assumia.

Descobertas ao validar o caminho crítico (corrigidas):
- O mtr do **12.3** passou a exigir módulos Perl que não vinham na imagem
  OL8: `Memoize`, `Time::HiRes`, `JSON::PP` (+ Data-Dumper/Getopt/Env por
  segurança). Adicionados ao Dockerfile.ol8.
- `run-mtr.sh` agora exige `mariadbd` presente (o MTR precisa do servidor e
  clientes locais, não só do .so) — falha cedo com mensagem clara se o build
  for parcial.

Prova de qualidade obtida: a **mesma** suíte MTR (mesmo .result) passa em
11.4.4 e 12.3.2 — o comportamento observável do plugin é idêntico entre
séries; só a ABI de compilação difere (D18).

## D20. Renomeação para `selective_trace` — reposicionamento como trace (v0.7.0)

O plugin nasceu rotulado como "auditoria", mas o propósito real sempre foi
o que o `general_log` não faz: **rastrear parcialmente** — só queries de
schemas/tabelas específicos, para diagnóstico. Auditoria carrega expectativas
de compliance (registro imutável, foco em quem/quando) que não são o caso.
Renomeado `selective_log` → `selective_trace` em todo o projeto:

- Nome do plugin, namespace C++, include guards, arquivo `selective_trace.cc`.
- Variáveis `selective_trace_*` (mantido `_to_log` em schemas/tables — o
  sufixo descreve a ação de rastrear; `log_file_path` virou `file_path`,
  removendo o "log" redundante).
- Tabela `mysql.selective_trace_events`, status `Selective_trace_*`.
- Suíte MTR `suite/selective_trace/`, docs e scripts.

**Não** mudou: a Audit Plugin API interna (é só o mecanismo — o único hook
que dá schema+tabela resolvidos; serve para trace tanto quanto para audit),
o comportamento observável, os defaults (mask_passwords segue ON — mesmo num
trace, proteger segredo por padrão é prudente; quem quer a query crua
desliga). `CLAUDE.md` preservado com o nome antigo como documento histórico.
Validado: 108 unitários, MTR, Valgrind sem leaks, funcional no 11.4 e 12.3.

## D21. Code-review pass (v0.7.1): mask_secrets password-leak fix

Adversarial code review + edge-case testing found one real defect (plus
confirmed several non-bugs). Fixed:

- **mask_secrets leaked credentials in three forms** the original code
  didn't cover: unquoted hex hashes (`IDENTIFIED BY PASSWORD 0x1234...`,
  `IDENTIFIED BY 0xDEAD...`), the MariaDB `VIA` connector (vs `WITH`), and
  the `OLD_PASSWORD()` function. `mask_following_quote` now also recognizes
  `0x...` hex literals; `mask_secrets` handles the `VIA` connector and
  `OLD_PASSWORD`. Guarded against false positives: a `0x` literal outside a
  credential context (e.g. `WHERE id = 0xFF`) is left untouched. 7 new unit
  tests (115 total); validated end-to-end on a live server (hash absent from
  the log, `***` present).

Confirmed NOT bugs (verified against the MariaDB source, not assumed):

- **deinit not holding filter_lock while freeing active_rules**: safe. The
  server acquires an audit plugin per-THD (`my_plugin_lock` in
  `sql_audit.cc:acquire_plugins`) and only lets UNINSTALL/deinit run once
  every THD has released it — no callback can be in flight during deinit.
- **file_writer_write lazy-open lock upgrade**: safe. A reopen/close needs
  the wrlock, which is exclusive with the rdlock held around `logger_write`.
- Server survived 14 adversarial edge cases (prepared statements,
  multi-statement, unicode/emoji, transactions, USE, nested stored routines,
  100k-char queries, syntax/runtime errors, a 300-reconnect storm, a
  3200-insert concurrent TABLE-mode burst) with 0 crashes, 0 drops, 0
  callback errors and 100% valid JSON.

All three platform builds regenerated at 0.7.1 (Ubuntu, EL8, 12.3/EL9) and
the hash-masking fix validated end-to-end on both live series — MariaDB
11.4.4 (test container) and 12.3.2 (OL9): the hash literal is absent from the
log and `***` is present. Fixing the 12.3 incremental build surfaced a stale
`plugin/selective_log` symlink left by the rename (two links to the same
`src/` → duplicate CMake target); `build.sh` now removes it.

Known minor (not fixed): the status counters (`events_logged` etc.) are
plain globals incremented without atomics, so under heavy concurrency the
reported totals can slightly under-count. Diagnostic-only; never corrupts
state. Would move to atomics if precise counts become a requirement.

## D22. Per-connection tracing (v0.8.0): connections_to_log

Trace requirement: capture everything a specific connection runs (a session
spotted in SHOW PROCESSLIST), regardless of schema/table — the diagnostic
counterpart of "general_log, but for one connection".

- New sysvar `selective_trace_connections_to_log`: comma-separated decimal
  connection ids. Parsed (in filter_engine, testable) into a sorted,
  de-duplicated vector; matched with binary_search in the hot path.
- Semantics (chosen with the user): a listed connection is traced **in
  full** — the decision at GENERAL_STATUS sets `allowed = CMD_ALL` when
  `match_connection(general_thread_id)` hits, so even table-less statements
  (SET, SHOW, DO, SELECT 1) that would otherwise fall through the
  schema/table filters are captured. It's effectively OR-ed with the other
  filters. `min_duration_ms` still applies to everyone (consistency).
- The connection id is the same `conn_id` already in the output and the
  `general_thread_id`/`thread_id` of the audit events (= SHOW PROCESSLIST id).
- Non-numeric tokens are rejected at SET time (ER_WRONG_VALUE_FOR_VAR).

Validated: 6 new unit tests (138 total), an MTR case tracing the test's own
connection (DO/SELECT captured with empty schema/table filters), a live test
confirming a full connection is captured and that other connections are not,
and Valgrind clean.
