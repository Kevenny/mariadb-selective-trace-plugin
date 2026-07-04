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
  filter_engine — pure filtering logic for the selective_log plugin.

  This translation unit must stay free of MariaDB headers so it can be
  unit-tested standalone (tests/test_filter_logic.cc builds it with plain
  g++ and its own main()).

  Matching is ASCII case-insensitive: schema/table identifiers are folded
  to lowercase at parse time and compared with a lowercase fold at match
  time, so filters behave the same regardless of lower_case_table_names.
*/

#ifndef SELECTIVE_LOG_FILTER_ENGINE_H
#define SELECTIVE_LOG_FILTER_ENGINE_H

#include <string>
#include <vector>
#include <cstddef>

namespace selective_log {

struct FilterRules
{
  /* From selective_log_schemas_to_log: lowercase schema names. */
  std::vector<std::string> schemas;
  /* From "schema.*" entries of selective_log_tables_to_log. */
  std::vector<std::string> wildcard_schemas;
  /* From selective_log_tables_to_log: lowercase "schema.table". */
  std::vector<std::string> tables;

  bool empty() const
  {
    return schemas.empty() && wildcard_schemas.empty() && tables.empty();
  }
};

/*
  Parse the two comma separated lists into *out.

  - Tokens are trimmed of ASCII whitespace and lowercased; empty tokens are
    ignored (so "a,,b" and trailing commas are fine).
  - Optional backticks around each identifier part are stripped.
  - Table tokens must be schema.table; "schema.*" is accepted as a
    whole-schema wildcard (equivalent to listing the schema in the schema
    list). Anything else (missing dot, empty part, extra dot) is invalid.
  - NULL pointers are treated as empty lists.

  Returns true on success. On failure returns false and stores the
  offending token in *error (out is left untouched).
*/
bool parse_filter_lists(const char *schemas_csv, const char *tables_csv,
                        FilterRules *out, std::string *error);

/*
  True if db matches the schema filter (schemas list or a schema.*
  wildcard). Empty rules never match — callers get the "both lists empty
  means log nothing" fail-safe for free.
*/
bool match_schema(const FilterRules &rules, const char *db, size_t db_len);

/* True if db.table matches the table filter (exact or schema.* wildcard). */
bool match_table(const FilterRules &rules, const char *db, size_t db_len,
                 const char *table, size_t table_len);

/*
  Extract the first SQL keyword of query, uppercased ("SELECT", "INSERT",
  "WITH", ...), skipping leading whitespace, parentheses and comments of
  all three SQL flavors: block comments, executable comments
  ("slash-star-bang NNNNN" — the content IS the statement), "-- " line
  comments and "#" line comments. Writes "OTHER" when no keyword is found.
  buf receives at most buf_size bytes including the terminating NUL.
*/
void extract_command(const char *query, size_t query_len,
                     char *buf, size_t buf_size);

} /* namespace selective_log */

#endif /* SELECTIVE_LOG_FILTER_ENGINE_H */
