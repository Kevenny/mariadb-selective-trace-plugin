# MariaDB Selective Trace Plugin (`selective_trace`)

[![CI](https://github.com/Kevenny/mariadb-selective-log-plugin/actions/workflows/ci.yml/badge.svg)](https://github.com/Kevenny/mariadb-selective-log-plugin/actions/workflows/ci.yml)

Plugin nativo (open source, GPLv2) para **MariaDB 11.4 e 12.3+** que faz
**trace seletivo de queries** — rastreia apenas queries de schemas/tabelas
específicos (e por tipo de comando), ao contrário do `general_log`, que só
tem o modo tudo-ou-nada. Baixo overhead, ativação a quente por `SET GLOBAL`.

Internamente usa a Audit Plugin API do MariaDB (o único hook que expõe
schema+tabela resolvidos), mas o propósito é **diagnóstico/observabilidade**,
não compliance.

**Status: implementado e validado** — filtros dinâmicos, saída FILE (JSON por
linha) e TABLE (`mysql.selective_trace_events`), duração em ms, benchmark e
Valgrind. Overhead medido: **~0%** (vs **+10%** do `general_log` no mesmo
cenário sintético) — ver [docs/BENCHMARKS.md](./docs/BENCHMARKS.md).

> 📖 **Como usar o plugin** (variáveis, formato do JSON, schema da tabela,
> limitações): [`docs/USAGE.md`](./docs/USAGE.md)
>
> 🔬 Pesquisa da API de audit no fonte do 11.4.4:
> [`docs/RESEARCH_NOTES.md`](./docs/RESEARCH_NOTES.md)
>
> ⚖️ Decisões técnicas (C++ vs C, eventos usados, anti-loop do modo TABLE,
> lições de crashes reais): [`docs/DECISIONS.md`](./docs/DECISIONS.md)
>
> 🔒 Modelo de ameaças, bateria adversarial e hardening SELinux/OL9:
> [`docs/SECURITY.md`](./docs/SECURITY.md)
>
> 📋 Especificação original usada com o Claude Code: [`CLAUDE.md`](./CLAUDE.md)

## TL;DR de uso

```sql
INSTALL PLUGIN selective_trace SONAME 'selective_trace.so';
SET GLOBAL selective_trace_enabled = ON;
SET GLOBAL selective_trace_schemas_to_log = 'vendas';            -- por schema
SET GLOBAL selective_trace_tables_to_log  = 'rh.salarios,logs.*'; -- por tabela
-- => JSON por linha em selective_trace.json (datadir), ou:
SET GLOBAL selective_trace_output = 'TABLE';   -- mysql.selective_trace_events
```

Com **ambas** as listas vazias o plugin não loga nada (fail-safe).

## Estrutura do Projeto

```
.
├── src/
│   ├── selective_trace.cc         # entrypoint: descriptor de audit, sysvars, captura
│   ├── filter_engine.{h,cc}     # lógica pura de filtro (sem headers do MariaDB)
│   ├── log_writer_file.{h,cc}   # modo FILE: JSON por linha via logger service
│   ├── log_writer_table.{h,cc}  # modo TABLE: thread própria + SQL service
│   └── CMakeLists.txt           # MYSQL_ADD_PLUGIN(selective_trace ... MODULE_ONLY)
├── tests/
│   └── test_filter_logic.cc     # testes standalone do filter_engine (g++ puro)
├── scripts/
│   ├── setup-dev-env.sh         # sobe o ambiente completo (imagem + fonte 11.4.4)
│   ├── build.sh                 # full | --plugin (incremental) | --package
│   ├── benchmark.sh             # Etapa 5: overhead vs general_log (mariadb-slap)
│   └── valgrind-test.sh         # Etapa 5: mariadbd sob Valgrind + bateria
├── docker/                      # container dev (toolchain) + mariadb-test (oficial)
└── docs/                        # USAGE, RESEARCH_NOTES, DECISIONS, BENCHMARKS
```

## Desenvolvimento

### 1. Subir o ambiente

```bash
./scripts/setup-dev-env.sh
```

Builda a imagem de desenvolvimento, sobe o container `dev` e clona o fonte
oficial do MariaDB (tag `mariadb-11.4.4`) em `/opt/mariadb-src` (volume).

### 2. Compilar

```bash
# dentro do container dev (docker compose -f docker/docker-compose.yml exec dev bash)
./scripts/build.sh full        # primeira vez (build completo, 20-60 min)
./scripts/build.sh --plugin    # incremental: só o plugin (segundos, com ccache)
./scripts/build.sh --package   # copia o .so para build/plugin_output/
```

### 3. Testes unitários do filtro (sem MariaDB)

```bash
g++ -std=c++11 -Wall -Wextra -Werror -I src \
    tests/test_filter_logic.cc src/filter_engine.cc -o test_filter_logic \
  && ./test_filter_logic
```

### 4. Testar num MariaDB 11.4.4 oficial "limpo"

```bash
docker compose -f docker/docker-compose.yml --profile test up -d mariadb-test
docker compose -f docker/docker-compose.yml exec mariadb-test \
  mariadb -uroot -pdevpassword testdb
```

O `docker/test-my.cnf` já carrega o plugin
(`plugin-load-add=selective_trace.so` + `plugin-maturity=experimental`) com o
filtro inicial `selective_trace_schemas_to_log=testdb`.

### 5. Benchmark e Valgrind

```bash
docker exec -i mariadb-plugin-test bash < scripts/benchmark.sh
docker exec -i mariadb-plugin-dev  bash < scripts/valgrind-test.sh
```

### 6. Build para Oracle Linux / RHEL 8+

O `.so` do container `dev` (Ubuntu 22.04) exige glibc ≥ 2.35 e não carrega em
EL8/EL9. Use o ambiente `dev-ol8` (base `oraclelinux:8` + gcc-toolset-12),
que gera um binário exigindo apenas GLIBC_2.17 — carregável em EL8, EL9 e
mais novos:

```bash
docker compose -f docker/docker-compose.yml --profile ol8 up -d --build dev-ol8
docker exec mariadb-plugin-dev-ol8 bash -lc 'cd /workspace && ./scripts/build.sh full && ./scripts/build.sh --package'
# saída: build/plugin_output-ol8/selective_trace.so
# validação num OL8 limpo com MariaDB 11.4 instalado via RPM oficial:
docker run --rm -i -v "$PWD/build/plugin_output-ol8:/plugin_out:ro" \
    oraclelinux:8 bash < scripts/validate-ol8.sh
```

Validado contra MariaDB 11.4.12 (RPMs oficiais) em Oracle Linux 8 **e 9**
(mesmo .so; para OL9 basta trocar a imagem para oraclelinux:9) — o plugin
compilado contra o fonte 11.4.4 é compatível com toda a série 11.4.x.

### 7. Build para MariaDB 12.3+ (Oracle Linux 9)

A audit ABI mudou de `0x0302` (11.4) para `0x0303` (12.3), então o `.so` de
11.4 **não** carrega num servidor 12.3. O mesmo código-fonte compila para as
duas séries (wrapper por `MYSQL_VERSION_ID` para a mudança do logger
service); só é preciso um build dedicado contra o fonte 12.3:

```bash
docker compose -f docker/docker-compose.yml --profile v123 up -d --build dev-123-ol8
docker exec mariadb-plugin-dev-123-ol8 bash -lc \
  './scripts/download-mariadb-source.sh && ./scripts/build.sh full && ./scripts/build.sh --package'
# saída: build/plugin_output-123-ol9/selective_trace.so
docker run --rm -i -v "$PWD/build/plugin_output-123-ol9:/plugin_out:ro" \
    oraclelinux:9 bash < scripts/validate-123-ol9.sh
```

Validado contra MariaDB 12.3.2 (RPM oficial) em Oracle Linux 9: plugin
ACTIVE, smoke test completo e bateria de segurança 7/7.

**Windows**: não suportado nesta versão — `log_writer_table` usa pthread,
relógios POSIX e `__attribute__((constructor))`. O porte é viável
(std::thread/std::chrono + DllMain, como o server_audit faz), mas exige
toolchain MSVC para compilar a árvore do MariaDB no Windows.

## Licença

GPLv2, compatível com a licença do MariaDB Server.
