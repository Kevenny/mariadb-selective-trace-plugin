# SECURITY.md — Modelo de ameaças e hardening do `selective_trace`

Validação de segurança adversarial do plugin, com foco em Oracle Linux 9.
Bateria reproduzível em [`scripts/security-test.sh`](../scripts/security-test.sh)
(container `oraclelinux:9` limpo, MariaDB 11.4.12 via RPM oficial).

---

## Superfície de ataque

O plugin recebe, por evento de audit, dados controlados pelo cliente
(texto da query, `user@host`, nomes de schema/tabela resolvidos pelo parser)
e os grava em dois destinos: arquivo JSON (modo FILE) ou
`INSERT INTO mysql.selective_trace_events` (modo TABLE). Os vetores são a
**injeção** desses dados no formato de saída e o **vazamento** de dados
sensíveis presentes nas próprias queries.

Mudar a configuração exige privilégio de servidor (`SUPER` /
`SET_USER` / `SESSION_VARIABLES_ADMIN` para `SET GLOBAL`;
`INSERT_PLUGIN`/`CREATE PLUGIN` para instalar) — fora do controle do plugin,
garantido pelo MariaDB.

## Resultados da bateria (OL9, plugin v0.6.0)

Bateria executada em duas séries do servidor, **7/7 em ambas**:
MariaDB 11.4.12 e MariaDB 12.3.2 (RPMs oficiais em `oraclelinux:9`).

| Teste | Vetor | Resultado |
|---|---|---|
| T1 | Injeção de SQL no INSERT interno (modo TABLE), `sql_mode` default | **PASS** — payload gravado como dado literal; `mysql.global_priv` intacta |
| T2 | Idem com `sql_mode=NO_BACKSLASH_ESCAPES` | **PASS** — writer fixa o próprio `sql_mode` (ver mitigação abaixo) |
| T3 | Injeção de JSON/nova-linha (modo FILE) | **PASS** — toda linha continua JSON válido, um evento por linha |
| T4 | Vazamento de senhas de DCL em cleartext | **PASS** após correção (mascaramento; era o único FAIL na primeira rodada) |
| T5 | Permissões do arquivo de log | **PASS** — dono `mysql`, modo `0660` |
| T6 | Path inacessível (ex.: `/root`) | **PASS** — falha graciosa, servidor íntegro |

## Mitigações implementadas

### 1. Injeção de SQL no modo TABLE (defesa em profundidade)

- `sql_escape_append()` escapa `'`, `\`, NUL, `\n`, `\r`, `Ctrl-Z` ao montar
  o INSERT.
- **Backslash-escaping só é válido sem `NO_BACKSLASH_ESCAPES`.** Para não
  depender do `sql_mode` global (que um `SET GLOBAL sql_mode=...` poderia
  mudar sob os pés do writer), a conexão interna do writer executa
  `SET SESSION sql_mode=''` ao conectar — o escaping fica sempre coerente.
  Sem isso, um `'` no texto da query poderia quebrar o literal do INSERT.
- Os INSERTs rodam numa conexão interna dedicada (`sql_log_bin=0`,
  `skip_grants`), nunca na sessão do usuário.

### 2. Injeção de JSON no modo FILE

`json_escape_append()` escapa aspas, barra, e todos os controles < 0x20
(via `\uXXXX`) — impossível injetar nova-linha (quebraria o "um evento por
linha") ou fechar/reabrir o objeto JSON.

### 3. Mascaramento de credenciais (`selective_trace_mask_passwords`, default ON)

Como o plugin registra o texto completo da query (fidelidade de trace,
como o general_log), statements de DCL exporiam senhas. `mask_secrets()`
substitui por `***` os literais de:

- `IDENTIFIED BY '...'`
- `IDENTIFIED BY PASSWORD '...'`
- `IDENTIFIED WITH <plugin> {BY|AS|USING} '...'`
- `PASSWORD('...')` / `PASSWORD '...'`
- `SET PASSWORD ... = '...'`

Matching case-insensitive, respeita fronteira de palavra (uma coluna
`password_hash` ou o texto `'my password'` num INSERT comum **não** disparam
mascaramento) e lida com aspas escapadas dentro do segredo. Desligável com
`SET GLOBAL selective_trace_mask_passwords=OFF` se você precisar do texto
íntegro num ambiente controlado.

> **Limitação honesta**: o mascaramento cobre as cláusulas de autenticação
> padrão. Ele **não** conhece a semântica da sua aplicação — se você audita
> um schema onde a própria aplicação faz `INSERT INTO users(pass) VALUES
> ('texto')`, esse valor é dado de negócio e será logado. Trate o log como
> um artefato sensível (permissões + retenção), como faria com o
> general_log ou o binlog.

## Hardening específico de Oracle Linux 9

### SELinux (enforcing por default no OL9)

O `mariadbd` roda confinado no domínio `mysqld_t`. Dois pontos:

1. **Contexto do `.so`**: ao copiar o plugin para o `plugin_dir`, aplique o
   contexto correto senão o SELinux bloqueia o `dlopen`:

   ```bash
   cp selective_trace.so /usr/lib64/mysql/plugin/
   restorecon -v /usr/lib64/mysql/plugin/selective_trace.so
   # (rótulo esperado: system_u:object_r:lib_t ou mysqld_plugin_exec_t)
   ```

2. **Path do log (modo FILE)**: `mysqld_t` só escreve em locais rotulados
   `mysqld_db_t` / `mysqld_log_t`. **Mantenha o log dentro do datadir**
   (`/var/lib/mysql/…`, default) ou de um diretório com rótulo adequado. Um
   path arbitrário (ex.: `/root`, `/tmp`) será **negado pelo SELinux**, não
   por bug do plugin — o plugin apenas registra a falha
   (`Selective_trace_write_failures`) sem derrubar o servidor (validado em T6).
   Para um diretório dedicado:

   ```bash
   semanage fcontext -a -t mysqld_log_t "/var/log/mariadb/selective(/.*)?"
   restorecon -Rv /var/log/mariadb
   ```

   > Nota: a bateria automatizada roda em container cujo kernel reporta
   > SELinux `Disabled` (limitação do host de teste), então a **negação**
   > em si não é exercida ali — os comandos acima são o procedimento
   > correto para o host OL9 real com `enforcing`.

### Permissões e retenção do arquivo de log

- O arquivo é criado com dono do processo (`mysql`) e modo `0660` (grupo
  `mysql`). Não deixe outros usuários no grupo `mysql`.
- O log **contém texto de query** (dados potencialmente sensíveis, mesmo com
  senhas mascaradas). Restrinja e rotacione:

  ```bash
  chmod 640 /var/lib/mysql/selective_trace.json   # se quiser tirar o grupo
  # logrotate: rotacione com create 0640 mysql mysql
  ```

- Modo TABLE: `mysql.selective_trace_events` herda as permissões do schema
  `mysql` (acesso já restrito a admins). Faça expurgo periódico.

## Boas práticas operacionais

- **Filtro mínimo**: audite só o necessário — reduz volume e exposição.
- **Monitore** `SHOW GLOBAL STATUS LIKE 'selective_trace%'`:
  `write_failures` (path/SELinux/disco), `events_dropped` (fila TABLE cheia),
  `callback_errors` (pressão de memória).
- **Kill-switch**: `SET GLOBAL selective_trace_enabled=OFF` a quente.
- **Maturity EXPERIMENTAL**: o servidor recusa carregar por default; quem
  instala libera conscientemente com `plugin-maturity=experimental`.

## Reportar vulnerabilidades

Abra uma issue no repositório (ou contato privado ao mantenedor para
questões sensíveis) descrevendo versão, plataforma e passos de reprodução.
