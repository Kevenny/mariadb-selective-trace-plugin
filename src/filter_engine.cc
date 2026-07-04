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

#include "filter_engine.h"

namespace selective_log {

static inline char ascii_lower(char c)
{
  return (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c;
}

static inline bool is_space(char c)
{
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* entry (already lowercase) == s[0..len) folded to lowercase? */
static bool ci_eq(const std::string &entry, const char *s, size_t len)
{
  if (entry.size() != len)
    return false;
  for (size_t i= 0; i < len; i++)
    if (entry[i] != ascii_lower(s[i]))
      return false;
  return true;
}

/* entry (lowercase "schema.table") == db + '.' + table, folded? */
static bool ci_eq_qualified(const std::string &entry,
                            const char *db, size_t db_len,
                            const char *table, size_t table_len)
{
  if (entry.size() != db_len + 1 + table_len)
    return false;
  size_t i= 0;
  for (size_t j= 0; j < db_len; j++, i++)
    if (entry[i] != ascii_lower(db[j]))
      return false;
  if (entry[i++] != '.')
    return false;
  for (size_t j= 0; j < table_len; j++, i++)
    if (entry[i] != ascii_lower(table[j]))
      return false;
  return true;
}

static bool contains(const std::vector<std::string> &list,
                     const char *s, size_t len)
{
  for (size_t i= 0; i < list.size(); i++)
    if (ci_eq(list[i], s, len))
      return true;
  return false;
}

/* Trim whitespace and lowercase. Backticks are handled per identifier
   part (after splitting schema.table at the dot), not here. */
static std::string clean_token(const std::string &raw)
{
  size_t b= 0, e= raw.size();
  while (b < e && is_space(raw[b]))
    b++;
  while (e > b && is_space(raw[e - 1]))
    e--;
  std::string out;
  out.reserve(e - b);
  for (size_t i= b; i < e; i++)
    out.push_back(ascii_lower(raw[i]));
  return out;
}

/* Strip one optional pair of surrounding backticks from an identifier. */
static std::string strip_ticks(const std::string &part)
{
  if (part.size() >= 2 && part[0] == '`' && part[part.size() - 1] == '`')
    return part.substr(1, part.size() - 2);
  return part;
}

static void split_csv(const char *csv, std::vector<std::string> *tokens)
{
  if (csv == NULL)
    return;
  std::string cur;
  for (const char *p= csv; ; p++)
  {
    if (*p == ',' || *p == '\0')
    {
      /* trim before deciding the token is empty */
      std::string t= clean_token(cur);
      if (!t.empty())
        tokens->push_back(t);
      cur.clear();
      if (*p == '\0')
        break;
    }
    else
      cur.push_back(*p);
  }
}

bool parse_filter_lists(const char *schemas_csv, const char *tables_csv,
                        FilterRules *out, std::string *error)
{
  FilterRules rules;

  std::vector<std::string> raw_schemas;
  split_csv(schemas_csv, &raw_schemas);
  for (size_t i= 0; i < raw_schemas.size(); i++)
  {
    std::string schema= strip_ticks(raw_schemas[i]);
    /* schema names cannot contain a dot; catch table entries put in the
       wrong variable early */
    if (schema.empty() || schema.find('.') != std::string::npos)
    {
      if (error)
        *error= raw_schemas[i];
      return false;
    }
    if (!contains(rules.schemas, schema.data(), schema.size()))
      rules.schemas.push_back(schema);
  }

  std::vector<std::string> raw_tables;
  split_csv(tables_csv, &raw_tables);
  for (size_t i= 0; i < raw_tables.size(); i++)
  {
    const std::string &tok= raw_tables[i];
    size_t dot= tok.find('.');
    if (dot == std::string::npos ||           /* no dot at all        */
        dot == 0 ||                           /* empty schema part    */
        dot == tok.size() - 1 ||              /* empty table part     */
        tok.find('.', dot + 1) != std::string::npos) /* more than one dot */
    {
      if (error)
        *error= tok;
      return false;
    }
    std::string schema= strip_ticks(tok.substr(0, dot));
    std::string table= strip_ticks(tok.substr(dot + 1));
    if (schema.empty() || table.empty())
    {
      if (error)
        *error= tok;
      return false;
    }
    if (table == "*")
    {
      if (!contains(rules.wildcard_schemas, schema.data(), schema.size()))
        rules.wildcard_schemas.push_back(schema);
    }
    else
    {
      std::string qualified= schema + "." + table;
      if (!contains(rules.tables, qualified.data(), qualified.size()))
        rules.tables.push_back(qualified);
    }
  }

  out->schemas.swap(rules.schemas);
  out->wildcard_schemas.swap(rules.wildcard_schemas);
  out->tables.swap(rules.tables);
  return true;
}

bool match_schema(const FilterRules &rules, const char *db, size_t db_len)
{
  if (db == NULL || db_len == 0)
    return false;
  return contains(rules.schemas, db, db_len) ||
         contains(rules.wildcard_schemas, db, db_len);
}

bool match_table(const FilterRules &rules, const char *db, size_t db_len,
                 const char *table, size_t table_len)
{
  if (db == NULL || table == NULL || db_len == 0 || table_len == 0)
    return false;
  if (contains(rules.wildcard_schemas, db, db_len))
    return true;
  for (size_t i= 0; i < rules.tables.size(); i++)
    if (ci_eq_qualified(rules.tables[i], db, db_len, table, table_len))
      return true;
  return false;
}

} /* namespace selective_log */
