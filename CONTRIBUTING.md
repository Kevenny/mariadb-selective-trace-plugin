# Contribuindo com o `selective_trace`

Obrigado pelo interesse! Este é um plugin de **trace seletivo de queries**
para MariaDB (11.4 e 12.3+) — permite rastrear queries de schemas/tabelas
específicos, ao contrário do `general_log`, que é tudo-ou-nada. Licenciado
sob **GPLv2** (a mesma licença do MariaDB Server, para permitir eventual
inclusão upstream).

## Como reportar bugs / pedir features

Abra uma issue no repositório descrevendo:

- Versão do plugin (`SHOW GLOBAL STATUS` / `PLUGIN_AUTH_VERSION` em
  `information_schema.PLUGINS`), série do MariaDB e distro/arquitetura.
- Configuração relevante (`SHOW GLOBAL VARIABLES LIKE 'selective_trace%'`).
- Passos de reprodução. Para bugs de crash, o trecho do error log.

Questões de **segurança** sensíveis: contato privado ao mantenedor antes de
abrir issue pública (ver [docs/SECURITY.md](docs/SECURITY.md)).

## Ambiente de desenvolvimento

Todo o fluxo roda em Docker (nenhuma dependência no host além do Docker):

```bash
./scripts/setup-dev-env.sh          # imagem de build + fonte 11.4.4
./scripts/build.sh full             # 1ª vez (build completo do servidor)
./scripts/build.sh --plugin         # incremental (só o plugin, com ccache)
```

Detalhes de build por plataforma/série (Ubuntu, EL8/EL9, MariaDB 12.3+) em
[README.md](README.md) e [docs/USAGE.md](docs/USAGE.md).

## Antes de abrir um Pull Request

Rode a bateria completa — todos devem passar:

```bash
# 1. Testes unitários da lógica de filtro (sem MariaDB)
g++ -std=c++11 -Wall -Wextra -Werror -I src \
    tests/test_filter_logic.cc src/filter_engine.cc -o /tmp/tfl && /tmp/tfl

# 2. Suíte de integração MTR (formato oficial do MariaDB)
./scripts/run-mtr.sh                 # copia src/mysql-test/ p/ o source tree e roda

# 3. Sem vazamentos de memória
docker exec -i mariadb-plugin-dev bash < scripts/valgrind-test.sh

# 4. Sanidade de segurança (num OL9 limpo)
docker run --rm -i -v "$PWD/build/plugin_output-ol8:/plugin_out:ro" \
    oraclelinux:9 bash < scripts/security-test.sh
```

## Padrões de código

- **C++11**, estilo do código existente (ver [docs/DECISIONS.md](docs/DECISIONS.md), D1).
- A lógica pura de filtro/parsing vive em `src/filter_engine.{h,cc}` e **não
  depende de headers do MariaDB** — para permanecer testável standalone.
  Toda nova regra de filtro/classificação entra aqui, com testes em
  `tests/test_filter_logic.cc`.
- Nenhuma exceção C++ pode atravessar as fronteiras `extern "C"` (callback de
  audit, callbacks de sysvar, thread do writer) — use guards, como o código
  atual (ver D16).
- Cabeçalho de licença GPLv2 em todo arquivo-fonte novo (copie de um
  existente).
- Documente decisões de design/ABI não óbvias em `docs/DECISIONS.md`.

## Sign-off (DCO)

Assine os commits (`git commit -s`) atestando o
[Developer Certificate of Origin](https://developercertificate.org/). Para
uma eventual submissão upstream ao MariaDB Server, a fundação pode exigir
também o [MCA](https://mariadb.com/kb/en/mca/).
