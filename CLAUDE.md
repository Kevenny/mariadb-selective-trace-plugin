# CLAUDE.md — Especificação para Implementação do Plugin `selective_log`

> Este arquivo é o briefing principal para o **Claude Code**. Leia-o por completo
> antes de escrever qualquer código. Ele contém contexto, requisitos, restrições
> técnicas e o passo a passo esperado de implementação.

---

## 1. Objetivo do Projeto

Desenvolver um **plugin nativo para MariaDB 11.4.4**, open source, que substitua
o uso do `general_log` para casos onde só se deseja auditar/logar queries de:

- **Um ou mais schemas (databases) específicos**, e/ou
- **Uma ou mais tabelas específicas**, independente do schema em que estejam.

O plugin deve ter **overhead muito menor que o `general_log`**, pois filtra
antes de gravar, e deve permitir configuração **em tempo de execução** (sem
reiniciar o `mysqld`), via variáveis de sistema (`SET GLOBAL ...`).

---

## 2. Contexto Já Disponível no Ambiente

Este projeto já vem com:

- `/opt/mariadb-src` → código-fonte oficial do **MariaDB 11.4.4**, baixado via
  `scripts/download-mariadb-source.sh` (branch/tag `mariadb-11.4.4`).
- `/opt/mariadb-build` → diretório de build (CMake + Ninja).
- Um container Docker com todas as dependências de build já instaladas
  (veja `docker/Dockerfile`).
- `docker-compose.yml` com um segundo serviço (`mariadb-test`, imagem oficial
  `mariadb:11.4.4`) para testar o plugin já compilado, como um usuário final
  faria.

**Antes de implementar, você (Claude Code) deve investigar o código-fonte real**
nos caminhos abaixo — não assuma a API, confirme lendo o código:

```
/opt/mariadb-src/include/mysql/plugin.h            # API genérica de plugins
/opt/mariadb-src/include/mysql/plugin_audit.h      # API de Audit Plugin (a mais provável de usarmos)
/opt/mariadb-src/sql/sql_class.h                   # struct THD (thread/sessão)
/opt/mariadb-src/sql/log.cc                        # como o general_log é implementado hoje
/opt/mariadb-src/sql/sql_parse.cc                  # onde queries são despachadas
/opt/mariadb-src/plugin/                           # plugins de referência já existentes
/opt/mariadb-src/plugin/server_audit/               # plugin de auditoria oficial (MELHOR REFERÊNCIA)
/opt/mariadb-src/plugin/audit_null/                 # plugin de auditoria mínimo de exemplo
```

> ⚠️ **Ponto crítico**: no MariaDB moderno, a forma correta e suportada de
> observar/logar todas as queries executadas (com schema e tabela disponíveis)
> é através da **Audit Plugin API** (`MYSQL_AUDIT_PLUGIN`,
> `include/mysql/plugin_audit.h`), especificamente os eventos da classe
> `MYSQL_AUDIT_GENERAL_CLASS` (evento `MYSQL_AUDIT_GENERAL_LOG`) e/ou
> `MYSQL_AUDIT_TABLE_ACCESS_CLASS` (eventos de leitura/escrita por tabela,
> disponíveis a partir da mesma família de audit events).
>
> **NÃO** tente fazer parsing manual de SQL com regex/string matching para
> descobrir schema/tabela (a implementação anterior conversada no chat era só
> ilustrativa). O parser do MariaDB já resolve isso e a Audit API expõe
> `database` e, para eventos de table access, a tabela envolvida diretamente
> como structs, de forma muito mais confiável.
>
> Leia `plugin/server_audit/server_audit.c` inteiro — é a referência mais
> próxima do que queremos construir (ele já filtra por eventos, já escreve em
> arquivo/tabela/syslog, já tem variáveis de sistema dinâmicas). Nosso plugin é
> essencialmente um **fork simplificado e focado em filtro por
   schema/tabela** desse conceito.

---

## 3. Decisão de Linguagem (pesquise e confirme)

O MariaDB server é escrito em **C e C++** (a maior parte do core em C++,
muitos plugins em C puro para simplicidade/ABI estável). Plugins são
compilados como bibliotecas dinâmicas (`.so` no Linux) carregadas via
`dlopen`, respeitando a **Plugin ABI** definida em `plugin.h`.

**Não existe suporte a plugins em outras linguagens** (Python, Go, Rust, etc.)
de forma nativa — a ABI é C. Portanto:

- **Linguagem a usar: C++ (preferencialmente) ou C**, seguindo o padrão do
  código-fonte já existente em `plugin/server_audit/` (que é C) e outros
  plugins de auditoria/autenticação em `plugin/` (verifique alguns em C++
  também, como storage engines, para efeito de comparação de padrão de
  código).
- Justifique na sua análise (documentar em `docs/DECISIONS.md`, veja seção 7)
  qual dos dois (C vs C++) escolheu e por quê, após ler pelo menos 2 plugins
  de referência.
- **Padrão de build**: usar `MYSQL_ADD_PLUGIN(...)` no `CMakeLists.txt`,
  igual aos demais plugins do repositório — confirme a sintaxe exata lendo
  `plugin/server_audit/CMakeLists.txt` e `cmake/plugin.cmake`.

---

## 4. Requisitos Funcionais

### 4.1 Filtros de captura
- `selective_log_schemas_to_log` (string, dinâmica, `SET GLOBAL`): lista de
  schemas separados por vírgula. Vazio = não filtra por schema.
- `selective_log_tables_to_log` (string, dinâmica, `SET GLOBAL`): lista de
  tabelas no formato `schema.tabela` separadas por vírgula. Vazio = não
  filtra por tabela.
- Se **ambas** as listas estiverem vazias, o plugin não loga nada (fail-safe,
  evita virar um general_log acidental).
- Se uma query referenciar múltiplas tabelas (ex: `JOIN`), logar se
  **qualquer uma** delas casar com o filtro.
- Suporte a **wildcard `*` no nome da tabela** dentro de um schema filtrado
  (ex: `meuschema.*` é redundante com filtrar só o schema, mas documente o
  comportamento).

### 4.2 O que logar por evento
Para cada query capturada, registrar no mínimo:
- Timestamp (com precisão de milissegundos)
- `connection_id`
- Usuário (`user@host`)
- Schema (`database` no momento da execução)
- Tabela(s) envolvida(s), quando aplicável
- Tipo de comando (`SELECT`, `INSERT`, `UPDATE`, `DELETE`, `DDL`, etc.)
- Texto da query completa
- **Tempo de execução** (duration), se o evento de audit usado permitir
  capturar start/end — senão, documentar a limitação.
- Status/erro (se a query falhou, incluir código de erro)

### 4.3 Destino do log — suportar 2 modos, configuráveis via variável
`selective_log_output` (`FILE` | `TABLE`):

- **Modo `FILE`**: escreve em arquivo texto (path configurável via
  `selective_log_log_file_path`), uma linha JSON por evento (facilita
  ingestão por ferramentas externas tipo Filebeat/Fluentd).
- **Modo `TABLE`**: insere em uma tabela própria do plugin, por exemplo
  `mysql.selective_log_events` (schema/nome definidos por você, mas
  documentados), criada automaticamente na inicialização do plugin caso não
  exista.
- Pesquise em `server_audit.c` como eles tratam concorrência e evitam que a
  própria escrita do log gere um novo evento de log (loop infinito) quando o
  destino é uma tabela — trate esse caso.

### 4.4 Variáveis de sistema esperadas (mínimo)
```sql
selective_log_enabled            -- BOOL, dinâmica, GLOBAL
selective_log_schemas_to_log     -- VARCHAR, dinâmica, GLOBAL
selective_log_tables_to_log      -- VARCHAR, dinâmica, GLOBAL
selective_log_output             -- ENUM('FILE','TABLE'), dinâmica, GLOBAL
selective_log_log_file_path      -- VARCHAR, dinâmica, GLOBAL (usado se output=FILE)
selective_log_min_duration_ms    -- INT, dinâmica, GLOBAL (só loga queries mais lentas que X ms; 0 = todas)
```

### 4.5 Performance
- O caminho de "não deve logar" (schema/tabela fora do filtro) precisa ser
  **o mais barato possível** — comparações de string com early-return, sem
  alocação de memória desnecessária, sem I/O.
- Proteger estruturas compartilhadas (as listas de schemas/tabelas) com
  `mysql_rwlock_t` (leitura concorrente livre, escrita exclusiva só quando o
  admin muda a variável) — não usar mutex simples que serialize todas as
  queries.

---

## 5. Requisitos Não-Funcionais

- **Licença**: GPLv2, compatível com o MariaDB (adicionar cabeçalho de
  licença em todos os arquivos-fonte).
- **Compatibilidade**: compilar como plugin `DYNAMIC` (não estático), para
  poder ser instalado com `INSTALL PLUGIN ... SONAME '...'` sem recompilar o
  servidor inteiro.
- **Sem dependências externas** além do que já está disponível no ambiente
  de build do MariaDB (evitar libs externas de JSON pesadas; se precisar
  gerar JSON, usar algo simples e leve, ou a lib JSON já usada internamente
  pelo MariaDB, se existir e for acessível a plugins).
- **Sem vazamento de memória**: todo `malloc`/`new` correspondente deve ter
  seu `free`/`delete`; validar com Valgrind (script de teste deve incluir
  isso, ver seção 6).
- **Thread-safety**: MariaDB é multi-thread (uma thread por conexão por
  padrão); todo estado compartilhado do plugin deve ser protegido.

---

## 6. Passo a Passo Esperado de Implementação

Siga esta ordem. Ao final de cada etapa, rode os comandos de validação antes
de seguir para a próxima.

### Etapa 0 — Pesquisa (não pule esta etapa)
1. Ler `include/mysql/plugin.h` e `include/mysql/plugin_audit.h` por completo.
2. Ler `plugin/server_audit/server_audit.c` por completo.
3. Ler `plugin/audit_null/audit_null.c` (exemplo mínimo).
4. Documentar em `docs/RESEARCH_NOTES.md` (criar este arquivo):
   - Quais eventos de audit existem e qual(is) vamos usar.
   - Como `server_audit` estrutura as variáveis de sistema dinâmicas.
   - Como `server_audit` lida com escrita em arquivo vs. rotação de log.
   - Assinatura exata da struct de plugin de auditoria
     (`st_mysql_audit`) nesta versão específica (11.4.4) — **cole o trecho
     real do header**, versões diferentes do MariaDB mudam esses campos.

### Etapa 1 — Esqueleto do plugin
1. Criar `src/CMakeLists.txt` usando `MYSQL_ADD_PLUGIN`.
2. Criar `src/selective_log.cc` (ou `.c`, conforme decisão da seção 3) com:
   - Struct `st_mysql_plugin` mínima, tipo `MYSQL_AUDIT_PLUGIN`.
   - Funções `init`/`deinit` vazias (só retornando 0).
   - Registro das variáveis de sistema da seção 4.4 (ainda sem lógica).
3. Rodar `./scripts/build.sh full` (primeira vez, build completo).
4. Validar que o plugin aparece: subir o `mariadb-test` (com o `.so` copiado
   via `./scripts/build.sh --package`) e rodar `SHOW PLUGINS;` /
   `INSTALL PLUGIN selective_log SONAME 'selective_log.so';`.

### Etapa 2 — Lógica de filtro
1. Implementar parsing das listas de schemas/tabelas (na callback de
   atualização da variável de sistema — `update` function do
   `MYSQL_SYSVAR`).
2. Implementar a função de decisão `should_log_event(...)`.
3. Escrever testes unitários simples (fora da árvore do MariaDB, um pequeno
   `tests/test_filter_logic.cc` com um `main()` próprio, sem depender de todo
   o server) para essa função de decisão — extraia a lógica pura de
   parsing/matching para um arquivo separado (`filter_engine.cc`/`.h`) que
   **não dependa de headers do MariaDB**, exatamente para viabilizar esse
   teste isolado e rápido.

### Etapa 3 — Captura e escrita do evento (modo FILE)
1. Implementar o callback do evento de audit escolhido.
2. Implementar escrita em arquivo (JSON por linha), com lock apropriado.
3. Testar manualmente: configurar `selective_log_schemas_to_log=testdb`,
   rodar queries em `testdb` e em outro schema, confirmar que só `testdb`
   aparece no arquivo de log.

### Etapa 4 — Escrita em tabela (modo TABLE)
1. Implementar criação automática da tabela de log na inicialização, se
   `output=TABLE` e a tabela não existir.
2. Implementar inserção do evento via API interna apropriada (pesquisar como
   plugins fazem operações SQL internamente sem re-disparar o próprio audit
   event — ver `server_audit.c` para padrão de "internal session" ou
   flag de reentrância).

### Etapa 5 — Testes de carga e memória
1. Rodar `sysbench` (ou script simples de benchmark) comparando:
   - Sem plugin habilitado.
   - Com `general_log=ON`.
   - Com `selective_log` habilitado filtrando 1 schema entre vários.
2. Rodar Valgrind no `mysqld` de teste com o plugin instalado, executar uma
   bateria de queries, confirmar zero leaks atribuíveis ao plugin.
3. Documentar os números em `docs/BENCHMARKS.md`.

### Etapa 6 — Documentação final
1. Atualizar `README.md` do projeto com instruções de uso reais (não as
   hipotéticas do início do projeto).
2. Gerar `docs/USAGE.md` com exemplos de `SET GLOBAL`, `INSTALL PLUGIN`,
   formato do JSON de log e schema da tabela de log.

---

## 7. Estrutura de Arquivos Esperada ao Final

```
src/
├── CMakeLists.txt
├── selective_log.cc          # entrypoint do plugin, struct st_mysql_plugin, sysvars
├── filter_engine.h           # lógica pura de filtro (sem deps do MariaDB)
├── filter_engine.cc
├── log_writer_file.h/.cc     # escrita em arquivo (modo FILE)
├── log_writer_table.h/.cc    # escrita em tabela (modo TABLE)
└── LICENSE_HEADER.txt

tests/
└── test_filter_logic.cc      # teste unitário standalone do filter_engine

docs/
├── RESEARCH_NOTES.md         # criado na Etapa 0
├── DECISIONS.md              # C vs C++, e outras decisões técnicas relevantes
├── BENCHMARKS.md             # criado na Etapa 5
└── USAGE.md                  # criado na Etapa 6
```

---

## 8. Critérios de Aceite (Definition of Done)

- [ ] Plugin compila como `DYNAMIC` sem warnings tratados como erro.
- [ ] `INSTALL PLUGIN` / `UNINSTALL PLUGIN` funcionam sem crash do `mysqld`.
- [ ] Filtro por schema funciona (confirmado com queries reais).
- [ ] Filtro por tabela (cross-schema) funciona.
- [ ] Ambos os filtros vazios = nada é logado.
- [ ] Modo `FILE` gera JSON válido, uma linha por evento.
- [ ] Modo `TABLE` cria e popula a tabela corretamente, sem loop de
      auto-log.
- [ ] `selective_log_min_duration_ms` filtra corretamente queries rápidas.
- [ ] Zero leaks relevantes no Valgrind.
- [ ] Overhead medido é sensivelmente menor que `general_log=ON` no
      benchmark da Etapa 5 (documentar percentual real).
- [ ] Toda decisão de API/design está documentada em `docs/`.

---

## 9. Como Você (Claude Code) Deve Trabalhar Neste Projeto

- Sempre que tiver dúvida sobre a API do MariaDB, **leia o código-fonte em
  `/opt/mariadb-src`** antes de assumir comportamento — não confie apenas em
  conhecimento geral sobre MySQL/MariaDB, pois assinaturas mudam entre
  versões.
- Depois de cada etapa da seção 6, rode o build (`./scripts/build.sh
  --plugin` para incremental) e reporte erros de compilação reais antes de
  prosseguir.
- Use `grep`/busca no diretório `/opt/mariadb-src/plugin/` sempre que
  precisar de um segundo exemplo de uso de alguma API específica.
- Commits pequenos e frequentes (se houver um git configurado no projeto),
  um por etapa da seção 6.
- Pergunte ao usuário apenas se houver uma decisão de produto ambígua (por
  exemplo, formato exato do JSON); decisões puramente técnicas de
  implementação (qual struct usar, como travar um mutex) você resolve
  pesquisando o próprio código-fonte do MariaDB.
