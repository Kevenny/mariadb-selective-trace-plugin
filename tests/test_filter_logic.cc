/* Copyright (C) 2026 Kevenny Ferraz

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/*
  Standalone unit tests for src/filter_engine.{h,cc}.

  No test framework and no MariaDB headers on purpose — build and run with:

      g++ -std=c++11 -Wall -Wextra -Werror \
          -I src tests/test_filter_logic.cc src/filter_engine.cc \
          -o test_filter_logic && ./test_filter_logic
*/

#include <cstdio>
#include <cstring>
#include "filter_engine.h"

using selective_trace::FilterRules;
using selective_trace::parse_filter_lists;
using selective_trace::match_schema;
using selective_trace::match_table;

static int failures= 0;
static int checks= 0;

#define CHECK(cond)                                                     \
  do                                                                    \
  {                                                                     \
    checks++;                                                           \
    if (!(cond))                                                        \
    {                                                                   \
      failures++;                                                       \
      std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                                   \
  } while (0)

/* strlen-based convenience wrappers for the (ptr, len) API */
static bool m_schema(const FilterRules &r, const char *db)
{
  return match_schema(r, db, std::strlen(db));
}
static bool m_table(const FilterRules &r, const char *db, const char *tbl)
{
  return match_table(r, db, std::strlen(db), tbl, std::strlen(tbl));
}

static void test_empty_lists()
{
  FilterRules r;
  std::string err;
  CHECK(parse_filter_lists("", "", &r, &err));
  CHECK(r.empty());
  CHECK(parse_filter_lists(NULL, NULL, &r, &err));
  CHECK(r.empty());

  /* fail-safe: empty rules never match anything */
  CHECK(!m_schema(r, "testdb"));
  CHECK(!m_table(r, "testdb", "t1"));
}

static void test_schema_filter()
{
  FilterRules r;
  std::string err;
  CHECK(parse_filter_lists("testdb, other_db", "", &r, &err));
  CHECK(r.schemas.size() == 2);

  CHECK(m_schema(r, "testdb"));
  CHECK(m_schema(r, "TESTDB"));          /* case-insensitive */
  CHECK(m_schema(r, "Other_Db"));
  CHECK(!m_schema(r, "testdb2"));
  CHECK(!m_schema(r, "testd"));
  CHECK(!m_schema(r, ""));

  /* schema filter must not make table filter match */
  CHECK(!m_table(r, "testdb", "t1"));
}

static void test_token_cleanup()
{
  FilterRules r;
  std::string err;
  CHECK(parse_filter_lists("  a  ,, b ,", " `S1`.`T1` , c.d ", &r, &err));
  CHECK(r.schemas.size() == 2);
  CHECK(r.tables.size() == 2);
  CHECK(m_schema(r, "a"));
  CHECK(m_schema(r, "b"));
  CHECK(m_table(r, "s1", "t1"));         /* backticks stripped, lowered */
  CHECK(m_table(r, "S1", "T1"));
  CHECK(m_table(r, "c", "d"));

  /* duplicates are collapsed */
  CHECK(parse_filter_lists("x,X, x ", "y.z,`Y`.`Z`", &r, &err));
  CHECK(r.schemas.size() == 1);
  CHECK(r.tables.size() == 1);
}

static void test_table_filter_cross_schema()
{
  FilterRules r;
  std::string err;
  CHECK(parse_filter_lists("", "appdb.orders,other.users", &r, &err));

  CHECK(m_table(r, "appdb", "orders"));
  CHECK(m_table(r, "APPDB", "ORDERS"));
  CHECK(m_table(r, "other", "users"));
  CHECK(!m_table(r, "appdb", "users"));  /* pair must match together   */
  CHECK(!m_table(r, "other", "orders"));
  CHECK(!m_table(r, "appdb", "orders2"));
  CHECK(!m_table(r, "app", "dborders")); /* no substring confusion     */
  CHECK(!m_table(r, "appdb.or", "ders")); /* dot boundary is exact     */

  /* table filter must not make schema filter match */
  CHECK(!m_schema(r, "appdb"));
}

static void test_wildcard()
{
  FilterRules r;
  std::string err;
  CHECK(parse_filter_lists("", "logs.*,appdb.orders", &r, &err));
  CHECK(r.wildcard_schemas.size() == 1);
  CHECK(r.tables.size() == 1);

  /* schema.* logs every table of the schema... */
  CHECK(m_table(r, "logs", "anything"));
  CHECK(m_table(r, "LOGS", "Other"));
  /* ...and behaves like a schema filter entry */
  CHECK(m_schema(r, "logs"));
  CHECK(!m_schema(r, "appdb"));
  CHECK(!m_table(r, "logsx", "t"));
}

static void test_invalid_tokens()
{
  FilterRules r;
  std::string err;

  /* table entries must be schema.table */
  CHECK(!parse_filter_lists("", "noDot", &r, &err));
  CHECK(err == "nodot");                 /* offending token reported */
  CHECK(!parse_filter_lists("", ".x", &r, &err));
  CHECK(!parse_filter_lists("", "x.", &r, &err));
  CHECK(!parse_filter_lists("", "a.b.c", &r, &err));

  /* schema entries cannot contain a dot */
  CHECK(!parse_filter_lists("app.orders", "", &r, &err));
  CHECK(err == "app.orders");

  /* on failure previously parsed state is not leaked into *out */
  FilterRules untouched;
  std::string e2;
  CHECK(parse_filter_lists("keepme", "", &untouched, &e2));
  CHECK(!parse_filter_lists("", "bad", &untouched, &e2));
  CHECK(untouched.schemas.size() == 1);  /* still the old contents */
}

static void test_command_qualifiers()
{
  using namespace selective_trace;
  FilterRules r;
  std::string err;

  CHECK(parse_filter_lists("vendas:insert|update, rh",
                           "app.pedidos:delete, logs.*:dml", &r, &err));
  CHECK(match_schema(r, "vendas", 6) == (CMD_INSERT | CMD_UPDATE));
  CHECK(match_schema(r, "VENDAS", 6) == (CMD_INSERT | CMD_UPDATE));
  CHECK(match_schema(r, "rh", 2) == CMD_ALL);      /* sem qualificador */
  CHECK(match_table(r, "app", 3, "pedidos", 7) == CMD_DELETE);
  CHECK(match_table(r, "app", 3, "outra", 5) == 0);
  CHECK(match_table(r, "logs", 4, "qualquer", 8) == CMD_DML);
  CHECK(match_schema(r, "logs", 4) == CMD_DML);    /* schema.* conta */

  /* entradas duplicadas mesclam as máscaras */
  CHECK(parse_filter_lists("a:insert, A:update", "", &r, &err));
  CHECK(r.schemas.size() == 1);
  CHECK(match_schema(r, "a", 1) == (CMD_INSERT | CMD_UPDATE));

  /* grupos ddl/all e espaços em volta do qualificador */
  CHECK(parse_filter_lists("s:ddl, v : insert | delete", "t.x:all", &r, &err));
  CHECK(match_schema(r, "s", 1) == CMD_DDL);
  CHECK(match_schema(r, "v", 1) == (CMD_INSERT | CMD_DELETE));
  CHECK(match_table(r, "t", 1, "x", 1) == CMD_ALL);

  /* qualificadores inválidos rejeitados com o token ofensor */
  CHECK(!parse_filter_lists("s:banana", "", &r, &err));
  CHECK(err == "s:banana");
  CHECK(!parse_filter_lists("", "a.b:", &r, &err));
  CHECK(!parse_filter_lists("s:", "", &r, &err));

  /* mapeamento keyword -> bit */
  CHECK(command_bit("INSERT") == CMD_INSERT);
  CHECK(command_bit("TRUNCATE") == CMD_TRUNCATE);
  CHECK(command_bit("WITH") == CMD_SELECT);        /* CTE ~ SELECT */
  CHECK(command_bit("GRANT") == CMD_OTHER);
  CHECK(command_bit("OTHER") == CMD_OTHER);

  /* transaction-control commands */
  CHECK(command_bit("COMMIT") == CMD_COMMIT);
  CHECK(command_bit("ROLLBACK") == CMD_ROLLBACK);
  CHECK(command_bit("BEGIN") == CMD_BEGIN);
  CHECK(command_bit("SAVEPOINT") == CMD_SAVEPOINT);
  /* bare START must NOT be a transaction begin (START SLAVE/REPLICA/...);
     only START TRANSACTION opens one, and extract_command turns it into
     BEGIN before command_bit ever sees it */
  CHECK(command_bit("START") == CMD_OTHER);

  /* filter only commits of a schema */
  CHECK(parse_filter_lists("app:commit", "", &r, &err));
  CHECK(match_schema(r, "app", 3) == CMD_COMMIT);
  /* tcl group covers all transaction control */
  CHECK(parse_filter_lists("app:tcl", "", &r, &err));
  CHECK(match_schema(r, "app", 3) ==
        (CMD_COMMIT | CMD_ROLLBACK | CMD_BEGIN | CMD_SAVEPOINT));
  /* commit + rollback together */
  CHECK(parse_filter_lists("app:commit|rollback", "", &r, &err));
  CHECK(match_schema(r, "app", 3) == (CMD_COMMIT | CMD_ROLLBACK));
}

static std::string cmd(const char *q)
{
  char buf[24];
  selective_trace::extract_command(q, std::strlen(q), buf, sizeof(buf));
  return std::string(buf);
}

static void test_extract_command()
{
  CHECK(cmd("SELECT 1") == "SELECT");
  CHECK(cmd("  insert into t values (1)") == "INSERT");
  CHECK(cmd("(SELECT 1) UNION (SELECT 2)") == "SELECT");
  CHECK(cmd("WITH cte AS (SELECT 1) SELECT * FROM cte") == "WITH");

  /* START TRANSACTION canonicalizes to BEGIN; other START commands do not */
  CHECK(cmd("START TRANSACTION") == "BEGIN");
  CHECK(cmd("start transaction read only") == "BEGIN");
  CHECK(cmd("BEGIN") == "BEGIN");
  CHECK(cmd("START SLAVE") == "START");
  CHECK(cmd("START REPLICA") == "START");
  CHECK(cmd("START ALL SLAVES") == "START");
  CHECK(cmd("COMMIT") == "COMMIT");
  CHECK(cmd("ROLLBACK TO SAVEPOINT sp1") == "ROLLBACK");

  /* comentários de linha "--" (caso DBeaver: comentário anexado ao stmt) */
  CHECK(cmd("-- 3. Gerar eventos\nINSERT INTO testdb.t1 VALUES (1,'x')")
        == "INSERT");
  CHECK(cmd("--\nUPDATE t SET v=1") == "UPDATE");
  CHECK(cmd("-- so comentario") == "OTHER");
  /* "--x" sem espaço não é comentário em MariaDB: vira token */
  CHECK(cmd("--x") == "OTHER");

  /* comentários "#" */
  CHECK(cmd("# cabecalho\n# outro\ndelete from t") == "DELETE");

  /* comentários de bloco e executáveis */
  CHECK(cmd("/* c1 */ /* c2 */ REPLACE INTO t VALUES (1)") == "REPLACE");
  CHECK(cmd("/*!40000 ALTER TABLE t DISABLE KEYS */") == "ALTER");
  CHECK(cmd("/*M!100400 CREATE TABLE x (a int) */") == "CREATE");

  /* misturas */
  CHECK(cmd(" -- a\n /* b */ # c\n\tCALL proc(1)") == "CALL");
  CHECK(cmd("") == "OTHER");
  CHECK(cmd("   \n\t ") == "OTHER");
}

static std::string mask(const char *q)
{
  std::string out;
  selective_trace::mask_secrets(q, std::strlen(q), &out);
  return out;
}

static void test_mask_secrets()
{
  /* nada a mascarar: query devolvida idêntica */
  CHECK(mask("SELECT * FROM t WHERE v='hello'") ==
        "SELECT * FROM t WHERE v='hello'");
  CHECK(mask("INSERT INTO t VALUES ('data')") ==
        "INSERT INTO t VALUES ('data')");

  /* DCL: senha vira *** preservando aspas */
  CHECK(mask("CREATE USER x@localhost IDENTIFIED BY 'SuperSecret123'") ==
        "CREATE USER x@localhost IDENTIFIED BY '***'");
  CHECK(mask("SET PASSWORD FOR x = PASSWORD('AnotherSecret456')") ==
        "SET PASSWORD FOR x = PASSWORD('***')");
  CHECK(mask("SET PASSWORD = 'plain'") == "SET PASSWORD = '***'");
  CHECK(mask("ALTER USER x IDENTIFIED BY \"double\"") ==
        "ALTER USER x IDENTIFIED BY \"***\"");
  CHECK(mask("GRANT ALL ON *.* TO x IDENTIFIED BY 'p'") ==
        "GRANT ALL ON *.* TO x IDENTIFIED BY '***'");

  /* IDENTIFIED WITH plugin AS/BY '<hash>' */
  CHECK(mask("CREATE USER x IDENTIFIED WITH ed25519 AS 'HASHVAL'") ==
        "CREATE USER x IDENTIFIED WITH ed25519 AS '***'");
  CHECK(mask("CREATE USER x IDENTIFIED BY PASSWORD 'HASHVAL'") ==
        "CREATE USER x IDENTIFIED BY PASSWORD '***'");

  /* case-insensitive e aspas escapadas dentro do segredo */
  CHECK(mask("create user x identified by 'a\\'b'") ==
        "create user x identified by '***'");

  /* não confundir substrings: coluna chamada password_hash não dispara
     mascaramento indevido do valor de outra coluna */
  CHECK(mask("UPDATE t SET note='my password is safe' WHERE id=1") ==
        "UPDATE t SET note='my password is safe' WHERE id=1");

  /* hashes hexadecimais sem aspas (0x...) também são mascarados */
  CHECK(mask("CREATE USER x IDENTIFIED BY PASSWORD 0x1234ABCD") ==
        "CREATE USER x IDENTIFIED BY PASSWORD 0x***");
  CHECK(mask("GRANT ALL TO x IDENTIFIED BY 0xDEADBEEF") ==
        "GRANT ALL TO x IDENTIFIED BY 0x***");
  /* conector VIA (sintaxe MariaDB) + hash hex */
  CHECK(mask("ALTER USER x IDENTIFIED VIA plug USING 0xAAAA") ==
        "ALTER USER x IDENTIFIED VIA plug USING 0x***");
  /* OLD_PASSWORD() function */
  CHECK(mask("SET PASSWORD = OLD_PASSWORD('secret')") ==
        "SET PASSWORD = OLD_PASSWORD('***')");
  /* dois usuários numa CREATE USER: ambos mascarados */
  CHECK(mask("CREATE USER a IDENTIFIED BY 'p1', b IDENTIFIED BY 'p2'") ==
        "CREATE USER a IDENTIFIED BY '***', b IDENTIFIED BY '***'");
  /* 0x fora de contexto de senha NÃO é mascarado (não é falso positivo) */
  CHECK(mask("SELECT * FROM t WHERE id = 0xFF") ==
        "SELECT * FROM t WHERE id = 0xFF");
  CHECK(mask("INSERT INTO t VALUES (0xCAFE, 'x')") ==
        "INSERT INTO t VALUES (0xCAFE, 'x')");
}

static void test_connection_filter()
{
  using namespace selective_trace;
  FilterRules r;
  std::string err;

  /* empty = no connections */
  CHECK(parse_connection_list("", &r, &err));
  CHECK(r.connections.empty());
  CHECK(parse_connection_list(NULL, &r, &err));
  CHECK(r.connections.empty());

  /* parse, sort, dedupe */
  CHECK(parse_connection_list("42, 7, 42, 100 , 7", &r, &err));
  CHECK(r.connections.size() == 3);       /* 7, 42, 100 */
  CHECK(match_connection(r, 7));
  CHECK(match_connection(r, 42));
  CHECK(match_connection(r, 100));
  CHECK(!match_connection(r, 8));
  CHECK(!match_connection(r, 0));

  /* large id (64-bit) */
  CHECK(parse_connection_list("18446744073709551615", &r, &err));
  CHECK(match_connection(r, 18446744073709551615ULL));

  /* empty tokens between commas are ignored */
  CHECK(parse_connection_list("1,,2,", &r, &err));
  CHECK(r.connections.size() == 2);

  /* invalid tokens rejected with the offending token */
  CHECK(!parse_connection_list("42,abc", &r, &err));
  CHECK(err == "abc");
  CHECK(!parse_connection_list("-1", &r, &err));   /* '-' is not a digit */
  CHECK(!parse_connection_list("4 2", &r, &err));  /* embedded space */

  /* connections make FilterRules non-empty (fail-safe interplay) */
  FilterRules r2;
  CHECK(parse_filter_lists("", "", &r2, &err));
  CHECK(r2.empty());
  CHECK(parse_connection_list("5", &r2, &err));
  CHECK(!r2.empty());
}

static void test_match_null_safety()
{
  FilterRules r;
  std::string err;
  CHECK(parse_filter_lists("a", "b.c", &r, &err));
  CHECK(!match_schema(r, NULL, 0));
  CHECK(!match_table(r, NULL, 0, "c", 1));
  CHECK(!match_table(r, "b", 1, NULL, 0));
}

/*
  DDL classification. Every DDL form the server can dispatch has to land on
  the right bit, because a `:ddl` filter is the usual way to watch schema
  changes and a miss there is silent data loss for the operator.
*/
static void test_ddl_commands()
{
  using namespace selective_trace;

  /* each DDL verb resolves from real statement text, not just bare keywords */
  CHECK(cmd("CREATE TABLE app.t (id INT)") == "CREATE");
  CHECK(cmd("CREATE INDEX idx ON app.t (id)") == "CREATE");
  CHECK(cmd("CREATE OR REPLACE VIEW v AS SELECT 1") == "CREATE");
  CHECK(cmd("CREATE DATABASE app") == "CREATE");
  CHECK(cmd("CREATE TEMPORARY TABLE tmp (a INT)") == "CREATE");
  CHECK(cmd("ALTER TABLE app.t ADD COLUMN c INT") == "ALTER");
  CHECK(cmd("alter table app.t drop column c") == "ALTER");
  CHECK(cmd("DROP TABLE IF EXISTS app.t") == "DROP");
  CHECK(cmd("DROP INDEX idx ON app.t") == "DROP");
  CHECK(cmd("TRUNCATE TABLE app.t") == "TRUNCATE");
  CHECK(cmd("truncate app.t") == "TRUNCATE");
  CHECK(cmd("RENAME TABLE app.t TO app.t2") == "RENAME");

  /* ALTER inside a versioned-comment wrapper (mariadb-dump output) */
  CHECK(cmd("/*!40000 ALTER TABLE t ENABLE KEYS */") == "ALTER");

  /* keyword -> bit, and membership in the CMD_DDL group */
  CHECK(command_bit("CREATE") == CMD_CREATE);
  CHECK(command_bit("ALTER") == CMD_ALTER);
  CHECK(command_bit("DROP") == CMD_DROP);
  CHECK(command_bit("TRUNCATE") == CMD_TRUNCATE);
  CHECK(command_bit("RENAME") == CMD_RENAME);
  CHECK((command_bit("CREATE") & CMD_DDL) != 0);
  CHECK((command_bit("ALTER") & CMD_DDL) != 0);
  CHECK((command_bit("DROP") & CMD_DDL) != 0);
  CHECK((command_bit("TRUNCATE") & CMD_DDL) != 0);
  CHECK((command_bit("RENAME") & CMD_DDL) != 0);

  /* DDL and DML groups must not overlap: a :ddl filter must not catch DML */
  CHECK((CMD_DDL & CMD_DML) == 0);
  CHECK((command_bit("INSERT") & CMD_DDL) == 0);
  CHECK((command_bit("SELECT") & CMD_DDL) == 0);

  /* a :ddl filter accepts every DDL verb and rejects the rest */
  FilterRules r;
  std::string err;
  CHECK(parse_filter_lists("app:ddl", "", &r, &err));
  const unsigned mask= match_schema(r, "app", 3);
  CHECK((mask & command_bit("CREATE")) != 0);
  CHECK((mask & command_bit("ALTER")) != 0);
  CHECK((mask & command_bit("DROP")) != 0);
  CHECK((mask & command_bit("TRUNCATE")) != 0);
  CHECK((mask & command_bit("RENAME")) != 0);
  CHECK((mask & command_bit("SELECT")) == 0);
  CHECK((mask & command_bit("INSERT")) == 0);
  CHECK((mask & command_bit("COMMIT")) == 0);

  /* single-verb DDL qualifier: only that verb passes */
  CHECK(parse_filter_lists("app:drop", "", &r, &err));
  CHECK(match_schema(r, "app", 3) == CMD_DROP);
  CHECK((match_schema(r, "app", 3) & command_bit("CREATE")) == 0);

  /* per-table DDL qualifier, cross-schema */
  CHECK(parse_filter_lists("", "app.t:ddl, ops.*:alter", &r, &err));
  CHECK(match_table(r, "app", 3, "t", 1) == CMD_DDL);
  CHECK(match_table(r, "ops", 3, "anything", 8) == CMD_ALTER);
  CHECK(match_table(r, "app", 3, "other", 5) == 0);
}

/*
  SELECT classification. SELECTs are the bulk of a read workload, so both
  the shapes that must match and the ones that must NOT be mistaken for a
  SELECT are worth pinning down.
*/
static void test_select_commands()
{
  using namespace selective_trace;

  /* plain and parenthesised/compound selects */
  CHECK(cmd("SELECT 1") == "SELECT");
  CHECK(cmd("select * from app.t where id=1") == "SELECT");
  CHECK(cmd("  \t\n SELECT * FROM app.t") == "SELECT");
  CHECK(cmd("(SELECT 1) UNION ALL (SELECT 2)") == "SELECT");
  CHECK(cmd("((SELECT 1))") == "SELECT");
  CHECK(cmd("SELECT * FROM a JOIN b ON a.id=b.id") == "SELECT");
  CHECK(cmd("SELECT * FROM (SELECT 1) x") == "SELECT");
  CHECK(cmd("SELECT SLEEP(0.1)") == "SELECT");
  CHECK(cmd("SELECT /*+ MAX_EXECUTION_TIME(1000) */ 1") == "SELECT");

  /* CTEs are treated as selects (documented behaviour) */
  CHECK(cmd("WITH cte AS (SELECT 1) SELECT * FROM cte") == "WITH");
  CHECK(command_bit("WITH") == CMD_SELECT);

  /* a comment block preceding the verb must not hide it */
  CHECK(cmd("/* app:report */ SELECT 1") == "SELECT");
  CHECK(cmd("-- daily report\nSELECT count(*) FROM app.t") == "SELECT");

  /* statements that merely contain the word SELECT are not selects */
  CHECK(cmd("INSERT INTO app.t SELECT * FROM app.src") == "INSERT");
  CHECK(cmd("CREATE TABLE c AS SELECT * FROM app.t") == "CREATE");
  CHECK(cmd("REPLACE INTO t SELECT * FROM s") == "REPLACE");
  CHECK(command_bit("SELECT") == CMD_SELECT);
  CHECK((CMD_SELECT & CMD_DML) == 0);   /* SELECT is not DML here */

  /* a :select filter passes reads and blocks writes/DDL */
  FilterRules r;
  std::string err;
  CHECK(parse_filter_lists("app:select", "", &r, &err));
  const unsigned mask= match_schema(r, "app", 3);
  CHECK((mask & command_bit("SELECT")) != 0);
  CHECK((mask & command_bit("WITH")) != 0);      /* CTE shares the bit */
  CHECK((mask & command_bit("INSERT")) == 0);
  CHECK((mask & command_bit("UPDATE")) == 0);
  CHECK((mask & command_bit("DELETE")) == 0);
  CHECK((mask & command_bit("CREATE")) == 0);

  /* select + ddl combined on one entry */
  CHECK(parse_filter_lists("app:select|ddl", "", &r, &err));
  const unsigned both= match_schema(r, "app", 3);
  CHECK((both & command_bit("SELECT")) != 0);
  CHECK((both & command_bit("ALTER")) != 0);
  CHECK((both & command_bit("INSERT")) == 0);

  /* per-table select filter: only the listed table is read-traced */
  CHECK(parse_filter_lists("", "app.orders:select", &r, &err));
  CHECK(match_table(r, "app", 3, "orders", 6) == CMD_SELECT);
  CHECK(match_table(r, "app", 3, "items", 5) == 0);
}

/*
  Robustness of the pure engine against the odd inputs a real workload
  produces: very long statements, unusual whitespace, truncation into the
  fixed-size command buffer. These run in the audit hot path, so a buffer
  mistake here is a server crash.
*/
static void test_long_and_edge_inputs()
{
  using namespace selective_trace;

  /* a very long statement still classifies from its first keyword */
  std::string big= "SELECT ";
  big.append(60000, 'x');
  char buf[24];
  extract_command(big.data(), big.size(), buf, sizeof(buf));
  CHECK(std::string(buf) == "SELECT");

  /* a single huge keyword must be truncated, never overflow the buffer */
  std::string huge_kw(5000, 'A');
  extract_command(huge_kw.data(), huge_kw.size(), buf, sizeof(buf));
  CHECK(std::strlen(buf) < sizeof(buf));

  /* leading comment longer than the buffer, then the real verb */
  std::string commented= "/* ";
  commented.append(9000, 'c');
  commented+= " */ DELETE FROM app.t";
  extract_command(commented.data(), commented.size(), buf, sizeof(buf));
  CHECK(std::string(buf) == "DELETE");

  /* an unterminated comment yields no command rather than reading past end */
  extract_command("/* never closed", 15, buf, sizeof(buf));
  CHECK(std::strlen(buf) < sizeof(buf));

  /* NUL bytes inside the statement text must not be treated as terminators
     by the (ptr,len) API */
  const char with_nul[]= "SELECT\0 1";
  extract_command(with_nul, sizeof(with_nul) - 1, buf, sizeof(buf));
  CHECK(std::string(buf) == "SELECT");

  /* long identifiers in the filter lists parse and match exactly */
  FilterRules r;
  std::string err;
  std::string long_db(200, 'd');
  std::string long_tbl(200, 't');
  const std::string long_entry= long_db + "." + long_tbl;
  CHECK(parse_filter_lists("", long_entry.c_str(), &r, &err));
  CHECK(match_table(r, long_db.data(), long_db.size(),
                    long_tbl.data(), long_tbl.size()) == CMD_ALL);
  CHECK(!match_table(r, long_db.data(), long_db.size(), "other", 5));

  /* zero-length inputs are safe */
  CHECK(!match_schema(r, "", 0));
  CHECK(!match_table(r, "", 0, "", 0));

  /* masking leaves a long innocuous statement untouched and bounded */
  std::string long_sel= "SELECT * FROM app.t WHERE v='";
  long_sel.append(20000, 'v');
  long_sel+= "'";
  std::string masked;
  const bool changed= mask_secrets(long_sel.data(), long_sel.size(), &masked);
  CHECK(!changed || masked.size() <= long_sel.size());
}

int main()
{
  test_empty_lists();
  test_schema_filter();
  test_token_cleanup();
  test_table_filter_cross_schema();
  test_wildcard();
  test_invalid_tokens();
  test_command_qualifiers();
  test_extract_command();
  test_mask_secrets();
  test_connection_filter();
  test_match_null_safety();
  test_ddl_commands();
  test_select_commands();
  test_long_and_edge_inputs();

  if (failures)
  {
    std::fprintf(stderr, "%d/%d checks FAILED\n", failures, checks);
    return 1;
  }
  std::printf("OK — %d checks passed\n", checks);
  return 0;
}
