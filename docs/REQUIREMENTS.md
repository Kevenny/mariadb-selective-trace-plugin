# Requisitos de Negócio

## Problema
O `general_log` do MariaDB loga **todas** as queries do servidor, gerando:
- Overhead alto de I/O e CPU (~30-50% em cargas intensas).
- Volume de dados difícil de filtrar depois (tudo misturado).
- Sem opção nativa de "logar só o schema X" ou "logar só a tabela Y em
  qualquer schema".

## Solução Desejada
Um plugin de auditoria seletiva que permita:

1. **Logar por schema**: administrador define uma lista de schemas de
   interesse (ex: `producao`, `financeiro`) e só queries executadas nesses
   schemas são registradas.

2. **Logar por tabela, entre todos os schemas**: administrador define uma
   lista de tabelas (ex: `usuarios`, `pagamentos`) e qualquer query que toque
   essas tabelas é registrada, **independente do schema**.

3. **Configuração em runtime**: mudar os filtros sem reiniciar o servidor
   (`SET GLOBAL`).

4. **Baixo overhead**: quando uma query não casa com nenhum filtro, o custo
   adicional deve ser desprezível.

5. **Dois destinos de log**: arquivo (para integração com pipelines de log
   externos) ou tabela (para consulta via SQL direto).

## Não-Objetivos (fora do escopo desta primeira versão)
- Mascaramento/redação de dados sensíveis nas queries logadas.
- Envio direto para sistemas externos (Kafka, Syslog remoto, etc.) — pode
  ser via `FILE` + agente externo (Filebeat, etc.).
- Interface gráfica de configuração — tudo via SQL (`SET GLOBAL`) e
  `my.cnf`.
- Suporte a versões do MariaDB fora da 11.4.x nesta primeira fase.
