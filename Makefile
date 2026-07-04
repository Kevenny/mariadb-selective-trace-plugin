.PHONY: setup up down shell build build-plugin package test-up test-down test-shell clean

COMPOSE = docker compose -f docker/docker-compose.yml

## Sobe todo o ambiente do zero (build da imagem + download do fonte MariaDB)
setup:
	./scripts/setup-dev-env.sh

## Sobe apenas o container de desenvolvimento
up:
	$(COMPOSE) up -d dev

## Derruba os containers
down:
	$(COMPOSE) down

## Abre um shell no container de desenvolvimento
shell:
	$(COMPOSE) exec dev bash

## Inicia o Claude Code dentro do container, já no diretório do projeto
claude:
	$(COMPOSE) exec dev bash -lc "cd /workspace && claude"

## Build completo do MariaDB + plugin (primeira vez)
build:
	$(COMPOSE) exec dev bash -lc "./scripts/build.sh full"

## Build incremental, só do plugin
build-plugin:
	$(COMPOSE) exec dev bash -lc "./scripts/build.sh --plugin"

## Empacota o .so compilado para o container de teste
package:
	$(COMPOSE) exec dev bash -lc "./scripts/build.sh --package"

## Sobe o container de teste (MariaDB oficial + plugin instalado)
test-up:
	$(COMPOSE) --profile test up -d mariadb-test

## Derruba o container de teste
test-down:
	$(COMPOSE) --profile test down

## Shell/CLI SQL no container de teste
test-shell:
	$(COMPOSE) exec mariadb-test mariadb -uroot -pdevpassword testdb

## Limpa volumes e containers (CUIDADO: apaga o build acumulado)
clean:
	$(COMPOSE) down -v
