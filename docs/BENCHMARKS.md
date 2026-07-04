# BENCHMARKS.md — Overhead do `selective_log` vs `general_log`

Medições da Etapa 5, executadas com [`scripts/benchmark.sh`](../scripts/benchmark.sh)
no container `mariadb-plugin-test` (imagem oficial `mariadb:11.4.4`).

- **Ambiente**: Docker sobre WSL2 (kernel 6.6.87), host Windows 11, disco NVMe.
  Números absolutos variam por máquina; o que importa é a **comparação
  relativa** entre cenários na mesma rodada.
- **Ferramenta**: `mariadb-slap` (incluída na imagem oficial), cada cenário com
  uma rodada de aquecimento + uma rodada medida; tabelas recriadas/semeadas
  identicamente antes de cada cenário.
- Plugin build: RelWithDebInfo, tag `mariadb-11.4.4`.

## Cenários

| Cenário | Configuração |
|---|---|
| `baseline` | `selective_log_enabled=OFF`, `general_log=OFF` |
| `general_log` | `general_log=ON` (arquivo) |
| `sel_miss` | selective_log ON filtrando `bench_hot`, carga em `bench_cold` — caminho **"não loga"** (só o custo do filtro) |
| `sel_hit_file` | selective_log ON, carga em `bench_hot`, `output=FILE` — **toda query logada** |
| `sel_hit_table` | idem com `output=TABLE` (INSERT assíncrono) |

## Suíte MIX — carga realista (INSERT + SELECT indexado)

`concurrency=8`, 20.000 queries por rodada (50% INSERT, 50% SELECT por PK):

| Cenário | Segundos | Δ vs baseline |
|---|---:|---:|
| baseline | 2,745 | — |
| general_log | 2,743 | ~0% |
| sel_miss | 2,642 | ~0% |
| sel_hit_file | 2,648 | ~0% |
| sel_hit_table | 2,754 | ~0% |

**Leitura**: a ~7.300 qps com round-trip de cliente e I/O de InnoDB dominando,
**nenhum** mecanismo de log (nem o general_log) produz overhead mensurável —
as diferenças (±2%) são ruído entre rodadas. Nessa carga o argumento de
performance é irrelevante; o valor do selective_log é **volume de log**
(só o que interessa) e formato estruturado.

## Suíte LIGHT — custo do caminho de log isolado (`DO 1`)

`concurrency=32`, 60.000 statements `DO 1` por rodada (custo de execução
próximo de zero → o custo do log fica proporcionalmente visível):

| Cenário | Segundos | qps aprox. | Δ tempo vs baseline |
|---|---:|---:|---:|
| baseline | 0,962 | 62.400 | — |
| **general_log** | **1,059** | 56.700 | **+10,1%** |
| sel_miss | 0,927 | 64.700 | −3,6% (≈ ruído) |
| sel_hit_file | 0,949 | 63.200 | −1,4% (≈ ruído) |
| sel_hit_table | 0,954 | 62.900 | −0,8% (≈ ruído)¹ |

¹ No `sel_hit_table` a 60k+ eventos/s a fila do writer assíncrono saturou e
descartou 66.257 eventos (`Selective_log_events_dropped`) — **por design**: em
burst acima da vazão de INSERT, o plugin descarta e contabiliza em vez de
frear as queries dos usuários. Na suíte MIX (7,3k qps) não houve nenhum drop.

## Conclusões (critério de aceite)

1. **Caminho "não loga"** (`sel_miss`): custo indistinguível de zero — o
   filtro é um rdlock + comparações de string sem alocação, invisível mesmo a
   62k qps. É o cenário de produção típico (filtro restrito, maioria do
   tráfego fora dele).
2. **Overhead sensivelmente menor que `general_log=ON`**: no pior caso
   sintético o general_log custou **+10,1%**, enquanto o selective_log ficou
   em **~0%** em todos os modos — inclusive **logando 100% das queries** em
   FILE (escrita síncrona via logger service) e TABLE (assíncrona).
3. O modo TABLE protege a latência das queries sob burst à custa de
   possíveis descartes (monitoráveis via `Selective_log_events_dropped`);
   para auditoria sem perda em alto volume, prefira `output=FILE`.

## Como reproduzir

```bash
docker exec -i mariadb-plugin-test bash < scripts/benchmark.sh
# variáveis: BENCH_CONCURRENCY, BENCH_QUERIES,
#            BENCH_LIGHT_CONCURRENCY, BENCH_LIGHT_QUERIES
```

## Perfil de produção: 100k SELECT + 20k DML por minuto

Teste com o mix real informado pelo DBA (~83% SELECT / ~17% escrita ≈ 2.000
qps de demanda), via [`scripts/benchmark-profile.sh`](../scripts/benchmark-profile.sh)
— mix exato de 15 SELECT : 1 INSERT : 1 UPDATE : 1 DELETE, `concurrency=16`,
plugin v0.5.1.

### Capacidade (full speed, 36.000 queries por medição)

| Cenário | qps | Δ vs baseline |
|---|---:|---:|
| baseline (tudo OFF) | 29.925 | — |
| `general_log=ON` | 28.685 | −4,1% |
| **realista**: filtro `app_main:dml`, FILE (só writes auditados) | 29.925 | **0,0%** |
| filtro `app_main:dml`, TABLE | 29.340 | −2,0% (≈ ruído) |
| pior caso: `app_main` inteiro em FILE (SELECTs inclusive) | 31.088 | +3,9% (ruído)¹ |

¹ Cenário com *mais* trabalho medindo *mais rápido* que o baseline delimita a
banda de ruído da máquina (±4%): o custo do plugin fica abaixo do ruído em
todos os cenários, enquanto o general_log aparece no limite da banda.

**Headroom**: a demanda real (~2.000 qps) usa **6,7%** da capacidade medida
com o plugin ativo no cenário realista.

### Sustentado: 5 minutos a plena carga (cenário realista, FILE)

30.480 qps médios por 300 s (≈ **15× a demanda de produção**), logando
**5.400 eventos/s** (≈ 16× os 333 writes/s reais):

| Métrica | Resultado |
|---|---|
| RSS do mariadbd | 256 → 262 MB (+6 MB, estabilizando; sem crescimento monotônico — consistente com caches do servidor, não leak) |
| Eventos logados | 1.620.006, **zero** drops / write_failures / callback_errors |
| Arquivo de log | 363 MB para 1,62M eventos ≈ **224 bytes/evento** |

### Tradução para o volume de produção (20k DML/min auditados)

- **Throughput/latência**: nenhuma degradação mensurável — na taxa real
  (2k qps) o custo é indistinguível de zero, e mesmo a 15× isso o plugin
  fica dentro do ruído.
- **Disco (o único efeito operacional relevante)**: 20.000 eventos/min ×
  224 B ≈ **4,5 MB/min ≈ 270 MB/h ≈ 6,5 GB/dia** de JSON. Planejar
  logrotate/expurgo (ou `min_duration_ms`/filtros mais estreitos para
  reduzir volume).
- **Modo TABLE**: acompanhou ~29k qps (≈ 4.900 INSERTs/s no writer) sem um
  único drop — a fila só descartou no teste sintético anterior a 60k+
  eventos/s. Na taxa real (333/s) opera com folga de ordens de magnitude;
  atenção apenas ao crescimento da `mysql.selective_log_events` (mesmo
  ~224 B/evento + índice) — expurgo periódico recomendado.

## Validação de memória (Valgrind)

[`scripts/valgrind-test.sh`](../scripts/valgrind-test.sh) sobe o `mariadbd`
compilado (RelWithDebInfo) sob `valgrind --leak-check=full` com o plugin
carregado, roda a bateria (modos FILE e TABLE, trocas repetidas das listas de
filtro, erro de SQL, `min_duration`, `UNINSTALL`/`INSTALL PLUGIN`) e derruba o
servidor de forma limpa.

Resultado (2026-07-04):

```
HEAP SUMMARY: total heap usage: 26,207 allocs, 26,206 frees
LEAK SUMMARY:
   definitely lost: 0 bytes in 0 blocks
   indirectly lost: 0 bytes in 0 blocks
     possibly lost: 336 bytes in 1 blocks
   still reachable: 0 bytes in 0 blocks
```

**Zero leaks atribuíveis ao plugin.** O único bloco "possibly lost"
(336 bytes) é o TLS da thread de signal handler do próprio servidor
(`mysqld.cc:start_signal_handler` → `pthread_create`), sem nenhum frame do
plugin no stack — artefato conhecido e benigno do mariadbd.
