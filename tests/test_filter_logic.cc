/* Copyright (C) 2026 selective_log plugin authors

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

using selective_log::FilterRules;
using selective_log::parse_filter_lists;
using selective_log::match_schema;
using selective_log::match_table;

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

static void test_match_null_safety()
{
  FilterRules r;
  std::string err;
  CHECK(parse_filter_lists("a", "b.c", &r, &err));
  CHECK(!match_schema(r, NULL, 0));
  CHECK(!match_table(r, NULL, 0, "c", 1));
  CHECK(!match_table(r, "b", 1, NULL, 0));
}

int main()
{
  test_empty_lists();
  test_schema_filter();
  test_token_cleanup();
  test_table_filter_cross_schema();
  test_wildcard();
  test_invalid_tokens();
  test_match_null_safety();

  if (failures)
  {
    std::fprintf(stderr, "%d/%d checks FAILED\n", failures, checks);
    return 1;
  }
  std::printf("OK — %d checks passed\n", checks);
  return 0;
}
