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
  selective_log — selective query logging for MariaDB 11.4

  Audit-class plugin that logs queries touching a configurable set of
  schemas and/or tables, as a low-overhead alternative to general_log.

  Event flow per statement (see docs/RESEARCH_NOTES.md):

    MYSQL_AUDIT_GENERAL_LOG      statement dispatch starts: stamp the
                                 per-connection state (query_id, start clock).
    MYSQL_AUDIT_TABLE_*          one event per table touched: match against
                                 the filter, accumulate table names.
    MYSQL_AUDIT_GENERAL_STATUS   statement finished: decide (table match OR
                                 session-schema match), apply min_duration,
                                 assemble the JSON line and write it.

  Filters are immutable FilterRules snapshots swapped under a write lock;
  the hot path only takes read locks, so queries never serialize.
*/

#define PLUGIN_VERSION      0x0007
#define PLUGIN_STR_VERSION  "0.6.0"

#include <my_global.h>
#include <my_pthread.h>
#include <typelib.h>
#include <mysqld_error.h>
#include <mysql/plugin.h>
#include <mysql/plugin_audit.h>
#include <mysql/psi/mysql_thread.h>

#include <new>
#include <string>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/time.h>

#include "filter_engine.h"
#include "log_writer_file.h"
#include "log_writer_table.h"

using selective_log::FilterRules;

/* ------------------------------------------------------------------------
   Filter state, shared by all connections
   ------------------------------------------------------------------------ */

static mysql_rwlock_t filter_lock;
static FilterRules *active_rules= NULL;
static std::string *schemas_storage= NULL;
static std::string *tables_storage= NULL;
static std::string *file_path_storage= NULL;
static int plugin_ready= 0;
/* exceptions swallowed at the C boundaries (SHOW STATUS) */
static ulong status_callback_errors= 0;

#ifdef HAVE_PSI_INTERFACE
static PSI_rwlock_key key_rwlock_filter;
static PSI_rwlock_info rwlock_key_list[]=
{
  { &key_rwlock_filter, "SELECTIVE_LOG::filter_lock", PSI_FLAG_GLOBAL }
};
#else
#define key_rwlock_filter 0
#endif

/* ------------------------------------------------------------------------
   Per-connection statement state

   Same technique as server_audit's loc_info THDVAR: a hidden thread-local
   string variable whose default value is a NUL-free blob the server
   strdup()s once per connection, giving us a per-THD POD buffer that is
   freed automatically with the THD (PLUGIN_VAR_MEMALLOC). The magic field
   detects the pristine 'O'-filled copy and triggers initialization.
   ------------------------------------------------------------------------ */

#define STATE_MAGIC 0x53454C33          /* "SEL3" */
#define STATE_TABLES_BUF 3968

struct StatementState
{
  unsigned int magic;
  unsigned long long query_id;
  unsigned long long start_ns;          /* CLOCK_MONOTONIC at dispatch     */
  int have_start;                       /* start_ns valid for this stmt    */
  int in_statement;                     /* between GENERAL_LOG and STATUS  */
  unsigned int cmd_mask;                /* allowed CommandBits from table
                                           matches accumulated so far      */
  int tables_truncated;                 /* tables[] overflowed             */
  unsigned int tables_len;
  char tables[STATE_TABLES_BUF];        /* "db.tbl,db.tbl,..."             */
};

/*
  The default blob must be NUL-free BEFORE the server registers the
  sysvars and strdup()s it (that happens before init() runs), hence the
  shared-library constructor — same trick as server_audit's so_init().
*/
static char state_init_value[sizeof(struct StatementState) + 1];

static void __attribute__((constructor)) selective_log_so_init(void)
{
  memset(state_init_value, 'O', sizeof(state_init_value) - 1);
  state_init_value[sizeof(state_init_value) - 1]= 0;
}

static MYSQL_THDVAR_STR(state,
    PLUGIN_VAR_NOSYSVAR | PLUGIN_VAR_NOCMDOPT | PLUGIN_VAR_MEMALLOC,
    "selective_log internal per-connection state", NULL, NULL,
    state_init_value);

static StatementState *get_state(MYSQL_THD thd)
{
  StatementState *st= (StatementState *) THDVAR(thd, state);
  if (st->magic != STATE_MAGIC)
  {
    memset(st, 0, sizeof(*st));
    st->magic= STATE_MAGIC;
  }
  return st;
}

static unsigned long long now_ns()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long long) ts.tv_sec * 1000000000ULL +
         (unsigned long long) ts.tv_nsec;
}

/* ------------------------------------------------------------------------
   System variables (spec section 4.4)
   ------------------------------------------------------------------------ */

static my_bool opt_enabled= FALSE;
static char *opt_schemas_to_log= NULL;
static char *opt_tables_to_log= NULL;
static ulong opt_output= 0;
static char *opt_log_file_path= NULL;
static uint opt_min_duration_ms= 0;
static my_bool opt_mask_passwords= TRUE;

#define SELECTIVE_LOG_OUTPUT_FILE  0
#define SELECTIVE_LOG_OUTPUT_TABLE 1

static const char *output_names[]= { "FILE", "TABLE", NullS };
static TYPELIB output_typelib=
{
  array_elements(output_names) - 1, "", output_names, NULL
};

static int check_filter_list(MYSQL_THD thd __attribute__((unused)),
                             struct st_mysql_sys_var *var,
                             void *save, struct st_mysql_value *value,
                             int is_table_list)
try
{
  int len= 0;
  const char *str= value->val_str(value, NULL, &len);
  FilterRules ignored;
  std::string bad_token;

  if (str != NULL)
  {
    bool ok= is_table_list
      ? selective_log::parse_filter_lists(NULL, str, &ignored, &bad_token)
      : selective_log::parse_filter_lists(str, NULL, &ignored, &bad_token);
    if (!ok)
    {
      my_printf_error(ER_WRONG_VALUE_FOR_VAR,
                      "selective_log: invalid entry '%s' in %s",
                      MYF(0), bad_token.c_str(),
                      is_table_list ? "tables_to_log (expected schema.table"
                                      " or schema.*)"
                                    : "schemas_to_log");
      return 1;
    }
  }
  *(const char **) save= str;
  (void) var;
  return 0;
}
catch (...)
{
  /* C boundary: fail the SET instead of letting the exception escape */
  status_callback_errors++;
  return 1;
}

static int check_schemas_to_log(MYSQL_THD thd, struct st_mysql_sys_var *var,
                                void *save, struct st_mysql_value *value)
{
  return check_filter_list(thd, var, save, value, 0);
}

static int check_tables_to_log(MYSQL_THD thd, struct st_mysql_sys_var *var,
                               void *save, struct st_mysql_value *value)
{
  return check_filter_list(thd, var, save, value, 1);
}

static int rebuild_rules_locked(const char *schemas_csv,
                                const char *tables_csv)
{
  FilterRules *fresh= new (std::nothrow) FilterRules();
  if (fresh == NULL)
    return 1;

  std::string bad_token;
  if (!selective_log::parse_filter_lists(schemas_csv, tables_csv,
                                         fresh, &bad_token))
  {
    /* Can't happen after check callbacks, but never swap in bad rules. */
    delete fresh;
    return 1;
  }

  FilterRules *old= active_rules;
  active_rules= fresh;
  delete old;
  return 0;
}

static void update_filter_list(void *var_ptr, const void *save,
                               int is_table_list)
{
  const char *new_val= *(const char *const *) save;
  if (new_val == NULL)
    new_val= "";

  mysql_rwlock_wrlock(&filter_lock);

  /* the lock is C state: release it even if an allocation throws */
  try
  {
    std::string *storage= is_table_list ? tables_storage : schemas_storage;
    storage->assign(new_val);
    *(char **) var_ptr= const_cast<char *>(storage->c_str());

    (void) rebuild_rules_locked(schemas_storage->c_str(),
                                tables_storage->c_str());
  }
  catch (...)
  {
    status_callback_errors++;   /* old rules stay active */
  }

  mysql_rwlock_unlock(&filter_lock);
}

static void update_schemas_to_log(MYSQL_THD thd __attribute__((unused)),
                                  struct st_mysql_sys_var *var
                                    __attribute__((unused)),
                                  void *var_ptr, const void *save)
{
  update_filter_list(var_ptr, save, 0);
}

static void update_tables_to_log(MYSQL_THD thd __attribute__((unused)),
                                 struct st_mysql_sys_var *var
                                   __attribute__((unused)),
                                 void *var_ptr, const void *save)
{
  update_filter_list(var_ptr, save, 1);
}

static void update_log_file_path(MYSQL_THD thd __attribute__((unused)),
                                 struct st_mysql_sys_var *var
                                   __attribute__((unused)),
                                 void *var_ptr, const void *save)
{
  const char *new_val= *(const char *const *) save;
  if (new_val == NULL)
    new_val= "";

  try
  {
    file_path_storage->assign(new_val);
    *(char **) var_ptr= const_cast<char *>(file_path_storage->c_str());
  }
  catch (...)
  {
    status_callback_errors++;
    return;                     /* keep the previous path */
  }

  /* Reopen only if the writer is in use; otherwise it opens lazily. */
  if (opt_enabled && opt_output == SELECTIVE_LOG_OUTPUT_FILE)
    selective_log::file_writer_reopen(file_path_storage->c_str());
  else
    selective_log::file_writer_close();
}

static MYSQL_SYSVAR_BOOL(enabled, opt_enabled, PLUGIN_VAR_OPCMDARG,
  "Enable/disable selective query logging.",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_STR(schemas_to_log, opt_schemas_to_log,
  PLUGIN_VAR_RQCMDARG,
  "Comma separated list of schemas whose queries are logged."
  " Empty means no schema filter.",
  check_schemas_to_log, update_schemas_to_log, "");

static MYSQL_SYSVAR_STR(tables_to_log, opt_tables_to_log,
  PLUGIN_VAR_RQCMDARG,
  "Comma separated list of schema.table entries logged regardless of the"
  " schema filter (schema.* matches the whole schema)."
  " Empty means no table filter.",
  check_tables_to_log, update_tables_to_log, "");

static MYSQL_SYSVAR_ENUM(output, opt_output, PLUGIN_VAR_RQCMDARG,
  "Log destination. FILE writes one JSON object per line to"
  " selective_log_log_file_path; TABLE inserts into the plugin log table.",
  NULL, NULL, SELECTIVE_LOG_OUTPUT_FILE, &output_typelib);

static MYSQL_SYSVAR_STR(log_file_path, opt_log_file_path,
  PLUGIN_VAR_RQCMDARG,
  "Path of the log file used when selective_log_output=FILE."
  " Relative paths are resolved from the server data directory.",
  NULL, update_log_file_path, "selective_log.json");

static MYSQL_SYSVAR_UINT(min_duration_ms, opt_min_duration_ms,
  PLUGIN_VAR_RQCMDARG,
  "Only log queries slower than this many milliseconds. 0 logs all queries.",
  NULL, NULL, 0, 0, 0x7FFFFFFF, 1);

static MYSQL_SYSVAR_BOOL(mask_passwords, opt_mask_passwords,
  PLUGIN_VAR_OPCMDARG,
  "Replace credential literals (IDENTIFIED BY, PASSWORD(), SET PASSWORD)"
  " with *** before logging. On by default.",
  NULL, NULL, TRUE);

static struct st_mysql_sys_var *selective_log_sysvars[]=
{
  MYSQL_SYSVAR(enabled),
  MYSQL_SYSVAR(schemas_to_log),
  MYSQL_SYSVAR(tables_to_log),
  MYSQL_SYSVAR(output),
  MYSQL_SYSVAR(log_file_path),
  MYSQL_SYSVAR(min_duration_ms),
  MYSQL_SYSVAR(mask_passwords),
  MYSQL_SYSVAR(state),
  NULL
};

/* Status variables (SHOW STATUS LIKE 'selective_log%') */
static ulong status_events_logged= 0;
static ulong status_write_failures= 0;
static ulong status_events_dropped= 0;

static int show_write_failures(MYSQL_THD thd __attribute__((unused)),
                               struct st_mysql_show_var *var,
                               void *buff,
                               struct system_status_var *status
                                 __attribute__((unused)),
                               enum enum_var_type scope
                                 __attribute__((unused)))
{
  status_write_failures= selective_log::file_writer_failures() +
                         selective_log::table_writer_failures();
  var->type= SHOW_ULONG;
  var->value= (char *) &status_write_failures;
  (void) buff;
  return 0;
}

static int show_events_dropped(MYSQL_THD thd __attribute__((unused)),
                               struct st_mysql_show_var *var,
                               void *buff,
                               struct system_status_var *status
                                 __attribute__((unused)),
                               enum enum_var_type scope
                                 __attribute__((unused)))
{
  status_events_dropped= selective_log::table_writer_dropped();
  var->type= SHOW_ULONG;
  var->value= (char *) &status_events_dropped;
  (void) buff;
  return 0;
}

static struct st_mysql_show_var selective_log_status[]=
{
  { "selective_log_events_logged", (char *) &status_events_logged,
    SHOW_ULONG },
  SHOW_FUNC_ENTRY("selective_log_write_failures", show_write_failures),
  SHOW_FUNC_ENTRY("selective_log_events_dropped", show_events_dropped),
  { "selective_log_callback_errors", (char *) &status_callback_errors,
    SHOW_ULONG },
  { 0, 0, SHOW_UNDEF }
};

/* ------------------------------------------------------------------------
   Event capture
   ------------------------------------------------------------------------ */

static bool is_query_command(const struct mysql_event_general *event)
{
  return (event->general_command_length == 5 &&
          strncmp(event->general_command, "Query", 5) == 0) ||
         (event->general_command_length == 7 &&
          strncmp(event->general_command, "Execute", 7) == 0);
}

/*
  general_user arrives as "user[priv_user] @ host [ip]" (make_user_name).
  Reduce it to a plain "user@host" — callers escape it for their format.
*/
static void parse_user_host(std::string *out,
                            const char *uh, unsigned int uh_len)
{
  const char *end= uh + uh_len;
  const char *user_end= uh;
  while (user_end < end && *user_end != '[' && *user_end != ' ')
    user_end++;

  const char *at= user_end;
  while (at < end && *at != '@')
    at++;
  if (at >= end)
  {
    out->assign(uh, uh_len);           /* unexpected format: keep raw */
    return;
  }
  const char *host_begin= at + 1;
  while (host_begin < end && *host_begin == ' ')
    host_begin++;
  const char *host_end= host_begin;
  while (host_end < end && *host_end != ' ' && *host_end != '[')
    host_end++;

  out->assign(uh, (size_t)(user_end - uh));
  out->push_back('@');
  out->append(host_begin, (size_t)(host_end - host_begin));
}

/*
  Reset per-statement fields. Runs at every GENERAL_LOG (dispatch start),
  so a statement accumulates ALL its table events until GENERAL_STATUS —
  including sub-statements of stored routines, whose query_id advances
  past the id of the CALL itself.
*/
static void state_begin_statement(StatementState *st,
                                  unsigned long long query_id,
                                  int with_start)
{
  st->query_id= query_id;
  st->cmd_mask= 0;
  st->tables_len= 0;
  st->tables_truncated= 0;
  st->in_statement= 1;
  st->have_start= with_start;
  st->start_ns= with_start ? now_ns() : 0;
}

/*
  Server-internal bookkeeping tables (engine-independent statistics) get
  locked as a side effect of ordinary DML and are not part of the user's
  query. They are recorded/matched only when explicitly listed in
  selective_log_tables_to_log.
*/
static bool is_internal_stats_table(const char *db, size_t db_len,
                                    const char *tbl, size_t tbl_len)
{
  static const struct { const char *name; size_t len; } stats_tables[]=
  {
    { "table_stats", 11 }, { "column_stats", 12 }, { "index_stats", 11 },
    { "innodb_table_stats", 18 }, { "innodb_index_stats", 18 }
  };

  if (db_len != 5 || strncasecmp(db, "mysql", 5) != 0)
    return false;
  for (size_t i= 0; i < array_elements(stats_tables); i++)
    if (tbl_len == stats_tables[i].len &&
        strncasecmp(tbl, stats_tables[i].name, tbl_len) == 0)
      return true;
  return false;
}

static void state_add_table(StatementState *st,
                            const char *db, size_t db_len,
                            const char *tbl, size_t tbl_len)
{
  size_t need= db_len + 1 + tbl_len;
  if (need == 0 || need >= sizeof(st->tables))
  {
    st->tables_truncated= 1;
    return;
  }

  /* dedupe: same table locked more than once in a statement */
  if (st->tables_len > 0)
  {
    const char *hay= st->tables;
    size_t hay_len= st->tables_len;
    for (size_t pos= 0; pos + need <= hay_len; )
    {
      const char *entry_end= (const char *) memchr(hay + pos, ',',
                                                   hay_len - pos);
      size_t entry_len= entry_end ? (size_t)(entry_end - (hay + pos))
                                  : hay_len - pos;
      if (entry_len == need &&
          memcmp(hay + pos, db, db_len) == 0 &&
          hay[pos + db_len] == '.' &&
          memcmp(hay + pos + db_len + 1, tbl, tbl_len) == 0)
        return;
      if (!entry_end)
        break;
      pos+= entry_len + 1;
    }
  }

  if (st->tables_len + (st->tables_len ? 1 : 0) + need >= sizeof(st->tables))
  {
    st->tables_truncated= 1;                  /* full: drop extra tables */
    return;
  }

  if (st->tables_len)
    st->tables[st->tables_len++]= ',';
  memcpy(st->tables + st->tables_len, db, db_len);
  st->tables_len+= (unsigned int) db_len;
  st->tables[st->tables_len++]= '.';
  memcpy(st->tables + st->tables_len, tbl, tbl_len);
  st->tables_len+= (unsigned int) tbl_len;
}

static void handle_table_event(MYSQL_THD thd,
                               const struct mysql_event_table *event)
{
  StatementState *st= get_state(thd);

  /* Plugin enabled mid-statement (no GENERAL_LOG seen): start ad-hoc,
     without a start clock. Inside a statement we accumulate everything —
     sub-statements of stored routines advance query_id, but their tables
     still belong to the dispatched statement. */
  if (!st->in_statement)
    state_begin_statement(st, event->query_id, 0);

  const char *db= event->database.str;
  size_t db_len= event->database.length;
  const char *tbl= event->table.str;
  size_t tbl_len= event->table.length;

  unsigned explicit_cmds= 0;
  unsigned schema_cmds= 0;
  mysql_rwlock_rdlock(&filter_lock);
  const FilterRules *rules= active_rules;
  if (rules)
  {
    explicit_cmds= selective_log::match_table(*rules, db, db_len,
                                              tbl, tbl_len);
    schema_cmds= selective_log::match_schema(*rules, db, db_len);
  }
  mysql_rwlock_unlock(&filter_lock);

  /* bookkeeping side-effect tables only count when explicitly filtered */
  if (explicit_cmds == 0 && is_internal_stats_table(db, db_len,
                                                    tbl, tbl_len))
    return;

  state_add_table(st, db, db_len, tbl, tbl_len);
  if (event->event_subclass == MYSQL_AUDIT_TABLE_RENAME &&
      event->new_table.length)
    state_add_table(st, event->new_database.str, event->new_database.length,
                    event->new_table.str, event->new_table.length);

  st->cmd_mask|= explicit_cmds | schema_cmds;
}

static void handle_status_event(MYSQL_THD thd,
                                const struct mysql_event_general *event)
{
  if (!is_query_command(event))
    return;

  StatementState *st= get_state(thd);

  /* Which command is this statement? Needed for the per-entry command
     qualifiers, and reused verbatim in the output. */
  char cmdbuf[24];
  selective_log::extract_command(event->general_query,
                                 event->general_query_length,
                                 cmdbuf, sizeof(cmdbuf));
  const unsigned cmd_bit= selective_log::command_bit(cmdbuf);

  unsigned allowed= st->in_statement ? st->cmd_mask : 0;

  if (!(allowed & cmd_bit))
  {
    /* fall back to the session-schema filter */
    mysql_rwlock_rdlock(&filter_lock);
    const FilterRules *rules= active_rules;
    if (rules)
      allowed|= selective_log::match_schema(*rules, event->database.str,
                                            event->database.length);
    mysql_rwlock_unlock(&filter_lock);
  }

  if (!(allowed & cmd_bit))
    goto reset;

  {
    double duration_ms= -1;
    if (st->in_statement && st->have_start)
      duration_ms= (double) (now_ns() - st->start_ns) / 1e6;

    if (opt_min_duration_ms > 0 &&
        (duration_ms < 0 || duration_ms < (double) opt_min_duration_ms))
      goto reset;

    /* timestamp with milliseconds (wall clock) */
    struct timeval tv;
    struct tm tm_time;
    gettimeofday(&tv, NULL);
    time_t secs= (time_t) tv.tv_sec;
    localtime_r(&secs, &tm_time);

    char ts[32];
    snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             tm_time.tm_year + 1900, tm_time.tm_mon + 1, tm_time.tm_mday,
             tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec,
             (int) (tv.tv_usec / 1000));

    const int have_tables= (st->in_statement && st->tables_len > 0);
    char numbuf[64];
    std::string user_host;
    parse_user_host(&user_host, event->general_user,
                    event->general_user_length);

    /* Credential masking: log the sanitized query text in both modes. */
    const char *qtext= event->general_query;
    size_t qlen= event->general_query_length;
    std::string masked;
    if (opt_mask_passwords &&
        selective_log::mask_secrets(qtext, qlen, &masked))
    {
      qtext= masked.data();
      qlen= masked.size();
    }

    if (opt_output == SELECTIVE_LOG_OUTPUT_FILE)
    {
      std::string line;
      line.reserve(320 + event->general_query_length + st->tables_len + 32);

      line.append("{\"ts\":\"").append(ts);
      snprintf(numbuf, sizeof(numbuf), "\",\"conn_id\":%lu,\"query_id\":%llu",
               event->general_thread_id, event->query_id);
      line.append(numbuf);

      line.append(",\"user\":\"");
      selective_log::json_escape_append(&line, user_host.data(),
                                        user_host.size());

      line.append("\",\"db\":\"");
      selective_log::json_escape_append(&line, event->database.str,
                                        event->database.length);

      line.append("\",\"tables\":[");
      if (have_tables)
      {
        const char *p= st->tables;
        const char *tend= st->tables + st->tables_len;
        int first= 1;
        while (p < tend)
        {
          const char *comma= (const char *) memchr(p, ',',
                                                   (size_t)(tend - p));
          size_t elen= comma ? (size_t)(comma - p) : (size_t)(tend - p);
          if (!first)
            line.push_back(',');
          first= 0;
          line.push_back('"');
          selective_log::json_escape_append(&line, p, elen);
          line.push_back('"');
          p+= elen + 1;
        }
      }

      line.push_back(']');
      if (have_tables && st->tables_truncated)
        line.append(",\"tables_truncated\":true");
      line.append(",\"command\":\"");
      line.append(cmdbuf);

      if (duration_ms >= 0)
        snprintf(numbuf, sizeof(numbuf), "\",\"duration_ms\":%.3f",
                 duration_ms);
      else
        snprintf(numbuf, sizeof(numbuf), "\",\"duration_ms\":null");
      line.append(numbuf);

      snprintf(numbuf, sizeof(numbuf), ",\"error_code\":%d,\"query\":\"",
               event->general_error_code);
      line.append(numbuf);
      selective_log::json_escape_append(&line, qtext, qlen);
      line.append("\"}\n");

      if (selective_log::file_writer_write(line.data(), line.size(),
                                           opt_log_file_path))
        status_events_logged++;
    }
    else                                /* SELECTIVE_LOG_OUTPUT_TABLE */
    {
      std::string sql;
      sql.reserve(400 + event->general_query_length + st->tables_len + 32);

      sql.append("INSERT INTO mysql.selective_log_events"
                 " (`ts`,`conn_id`,`query_id`,`user`,`db`,`tables_involved`,"
                 "`command`,`duration_ms`,`error_code`,`query`) VALUES ('");
      sql.append(ts);
      snprintf(numbuf, sizeof(numbuf), "',%lu,%llu,'",
               event->general_thread_id, event->query_id);
      sql.append(numbuf);

      selective_log::sql_escape_append(&sql, user_host.data(),
                                       user_host.size());

      sql.append("','");
      selective_log::sql_escape_append(&sql, event->database.str,
                                       event->database.length);
      sql.append("','");
      if (have_tables)
      {
        selective_log::sql_escape_append(&sql, st->tables, st->tables_len);
        if (st->tables_truncated)
          sql.append(",...");
      }
      sql.append("','");
      sql.append(cmdbuf);
      sql.append("',");

      if (duration_ms >= 0)
        snprintf(numbuf, sizeof(numbuf), "%.3f", duration_ms);
      else
        snprintf(numbuf, sizeof(numbuf), "NULL");
      sql.append(numbuf);

      snprintf(numbuf, sizeof(numbuf), ",%d,'", event->general_error_code);
      sql.append(numbuf);
      selective_log::sql_escape_append(&sql, qtext, qlen);
      sql.append("')");

      if (selective_log::table_writer_enqueue(&sql))
        status_events_logged++;
    }
  }

reset:
  /* STATUS closes the statement: never log it twice. */
  st->cmd_mask= 0;
  st->tables_len= 0;
  st->tables_truncated= 0;
  st->have_start= 0;
  st->in_statement= 0;
}

static void notify_impl(MYSQL_THD thd, unsigned int event_class,
                        const void *event)
{
  if (event_class == MYSQL_AUDIT_GENERAL_CLASS)
  {
    const struct mysql_event_general *ev=
      (const struct mysql_event_general *) event;
    if (ev->event_subclass == MYSQL_AUDIT_GENERAL_LOG)
    {
      /* statement dispatch starts: stamp the state */
      StatementState *st= get_state(thd);
      state_begin_statement(st, ev->query_id, 1);
    }
    else if (ev->event_subclass == MYSQL_AUDIT_GENERAL_STATUS)
      handle_status_event(thd, ev);
  }
  else if (event_class == MYSQL_AUDIT_TABLE_CLASS)
    handle_table_event(thd, (const struct mysql_event_table *) event);
}

static void selective_log_notify(MYSQL_THD thd,
                                 unsigned int event_class,
                                 const void *event)
{
  if (!opt_enabled || !plugin_ready || thd == NULL)
    return;

  /* Never log the internal writer's own INSERTs (self-log loop). */
  if (selective_log::table_writer_is_self())
    return;

  /* C boundary: no C++ exception (e.g. bad_alloc while assembling the
     output under memory pressure) may reach the server — that would
     abort mariadbd. Drop the event and count it instead. */
  try
  {
    notify_impl(thd, event_class, event);
  }
  catch (...)
  {
    status_callback_errors++;
  }
}

/* ------------------------------------------------------------------------
   init / deinit
   ------------------------------------------------------------------------ */

static int selective_log_init(void *arg __attribute__((unused)))
{
#ifdef HAVE_PSI_INTERFACE
  if (PSI_server)
    PSI_server->register_rwlock("selective_log", rwlock_key_list, 1);
#endif
  mysql_rwlock_init(key_rwlock_filter, &filter_lock);
  selective_log::file_writer_init();
  selective_log::table_writer_init();

  schemas_storage= new (std::nothrow) std::string(
      opt_schemas_to_log ? opt_schemas_to_log : "");
  tables_storage= new (std::nothrow) std::string(
      opt_tables_to_log ? opt_tables_to_log : "");
  file_path_storage= new (std::nothrow) std::string(
      opt_log_file_path ? opt_log_file_path : "");
  if (schemas_storage == NULL || tables_storage == NULL ||
      file_path_storage == NULL)
    goto fail;

  /* Point the sysvars at our storage from the start, so the update
     callbacks and SHOW VARIABLES always deal with the same memory. */
  opt_schemas_to_log= const_cast<char *>(schemas_storage->c_str());
  opt_tables_to_log= const_cast<char *>(tables_storage->c_str());
  opt_log_file_path= const_cast<char *>(file_path_storage->c_str());

  if (rebuild_rules_locked(schemas_storage->c_str(),
                           tables_storage->c_str()))
    goto fail;

  plugin_ready= 1;
  fprintf(stderr, "selective_log: plugin %s started\n", PLUGIN_STR_VERSION);
  return 0;

fail:
  delete schemas_storage;
  delete tables_storage;
  delete file_path_storage;
  schemas_storage= tables_storage= file_path_storage= NULL;
  selective_log::table_writer_shutdown();
  selective_log::file_writer_deinit();
  mysql_rwlock_destroy(&filter_lock);
  return 1;
}

static int selective_log_deinit(void *arg __attribute__((unused)))
{
  if (!plugin_ready)
    return 0;
  plugin_ready= 0;

  selective_log::table_writer_shutdown();
  selective_log::file_writer_deinit();

  delete active_rules;
  active_rules= NULL;
  delete schemas_storage;
  delete tables_storage;
  delete file_path_storage;
  schemas_storage= tables_storage= file_path_storage= NULL;

  mysql_rwlock_destroy(&filter_lock);
  fprintf(stderr, "selective_log: plugin stopped\n");
  return 0;
}

/* ------------------------------------------------------------------------
   Plugin declaration
   ------------------------------------------------------------------------ */

static struct st_mysql_audit selective_log_descriptor=
{
  MYSQL_AUDIT_INTERFACE_VERSION,
  NULL,
  selective_log_notify,
  { MYSQL_AUDIT_GENERAL_CLASSMASK | MYSQL_AUDIT_TABLE_CLASSMASK }
};

maria_declare_plugin(selective_log)
{
  MYSQL_AUDIT_PLUGIN,
  &selective_log_descriptor,
  "selective_log",
  "selective_log plugin authors",
  "Selective query logging by schema/table",
  PLUGIN_LICENSE_GPL,
  selective_log_init,
  selective_log_deinit,
  PLUGIN_VERSION,
  selective_log_status,
  selective_log_sysvars,
  PLUGIN_STR_VERSION,
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
}
maria_declare_plugin_end;
