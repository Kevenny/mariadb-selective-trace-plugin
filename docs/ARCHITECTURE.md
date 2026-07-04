# Arquitetura do Plugin `selective_log`

Este documento descreve a arquitetura **proposta inicialmente** (ponto de
partida). O Claude Code deve validar/ajustar após a Etapa 0 de pesquisa
descrita no `CLAUDE.md`, e atualizar este arquivo se algo mudar.

## Visão Geral

```
┌─────────────────────────────────────────────────────────────┐
│                        Cliente SQL                            │
└───────────────────────────┬───────────────────────────────────┘
                             │ query
                             ▼
┌─────────────────────────────────────────────────────────────┐
│                     MariaDB Server (mysqld)                   │
│  ┌───────────────────────────────────────────────────────┐   │
│  │  Parser / Executor                                     │   │
│  └───────────────────────────┬────────────────────────────┘   │
│                               │ dispara evento de audit         │
│                               ▼                                │
│  ┌───────────────────────────────────────────────────────┐   │
│  │           Audit Plugin API (plugin_audit.h)             │   │
│  └───────────────────────────┬────────────────────────────┘   │
└───────────────────────────────┼────────────────────────────────┘
                                 │
                                 ▼
┌─────────────────────────────────────────────────────────────┐
│                  Plugin selective_log (.so)                   │
│                                                                 │
│  ┌───────────────┐   ┌────────────────┐   ┌────────────────┐  │
│  │ filter_engine │──▶│ decide logar?  │──▶│  log_writer_*  │  │
│  │ (schemas/     │   │ (sim/não)      │   │  (file/table)  │  │
│  │  tabelas)     │   └────────────────┘   └────────────────┘  │
│  └───────────────┘                                             │
│         ▲                                                      │
│         │ atualizado via                                       │
│         │ SET GLOBAL selective_log_*                           │
│  ┌──────┴────────┐                                              │
│  │  Sysvars       │                                             │
│  └────────────────┘                                             │
└─────────────────────────────────────────────────────────────┘
```

## Componentes

### 1. `filter_engine` (lógica pura, sem dependência do MariaDB)
Responsável por:
- Parsear as strings `schemas_to_log` e `tables_to_log` em estruturas de
  dados eficientes para lookup (ex: `std::unordered_set<std::string>`).
- Expor `should_log(schema, table_list) -> bool`.
- Ser testável de forma isolada (unit test standalone, sem precisar
  compilar/rodar o `mysqld`).

### 2. Camada de integração com Audit API
Responsável por:
- Registrar o plugin como `MYSQL_AUDIT_PLUGIN`.
- Assinar os eventos relevantes (a confirmar exatamente quais na Etapa 0 —
  candidatos: `MYSQL_AUDIT_GENERAL_CLASS`, `MYSQL_AUDIT_TABLE_ACCESS_CLASS`).
- Extrair do evento: schema, tabela(s), usuário, texto da query, timestamp,
  duração (se disponível), status.
- Chamar `filter_engine::should_log(...)`.
- Se `true`, delegar para o `log_writer` configurado.

### 3. `log_writer_file`
- Escreve uma linha JSON por evento em arquivo configurável.
- Usa lock (`mysql_mutex_t` ou similar) para escrita concorrente segura.
- Considerar rotação de log (ou documentar que a rotação é responsabilidade
  de ferramentas externas como `logrotate`, se não implementarmos rotação
  própria).

### 4. `log_writer_table`
- Cria a tabela de destino na inicialização, se não existir.
- Insere eventos via API interna do servidor.
- **Cuidado com reentrância**: o próprio INSERT do log pode dispar um novo
  evento de audit — precisa haver um mecanismo de flag "estou processando
  meu próprio log, ignorar" (thread-local, por exemplo).

### 5. Sysvars (variáveis de sistema)
Todas `GLOBAL` e dinâmicas (`PLUGIN_VAR_RQCMDARG`), conforme listado no
`CLAUDE.md` seção 4.4. As variáveis de string (`schemas_to_log`,
`tables_to_log`) devem ter uma função `update` que re-parseia e atualiza o
`filter_engine` de forma thread-safe (lock de escrita curto, só durante a
troca do ponteiro/estrutura).

## Decisões em Aberto (a resolver na Etapa 0 do CLAUDE.md)

1. Qual(is) evento(s) exato(s) da Audit API dá acesso confiável à lista de
   tabelas de uma query (não só o schema atual da sessão)? Verificar se
   `MYSQL_AUDIT_TABLE_ACCESS_CLASS` está disponível e estável na 11.4.4.
2. Como capturar duração da query com a Audit API (existe evento de
   "antes" e "depois", ou precisamos medir nós mesmos com timestamp em dois
   pontos)?
3. Qual o padrão usado por `server_audit` para evitar loop de auto-log ao
   escrever em tabela.
