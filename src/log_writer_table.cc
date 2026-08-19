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

#include <my_global.h>
#include <my_pthread.h>            /* portable pthread API (POSIX + Windows) */
#include <mysql/plugin.h>          /* pulls mysql/services.h (sql service) */

#include <deque>
#include <string>
#include <cstdio>

#include "log_writer_table.h"

namespace selective_trace {

#define LOG_TABLE_FQN "mysql.selective_trace_events"

static const char REPAIR_LOG_TABLE_SQL[]=
  "REPAIR TABLE " LOG_TABLE_FQN;

static const char CREATE_LOG_TABLE_SQL[]=
  "CREATE TABLE IF NOT EXISTS " LOG_TABLE_FQN " ("
  " `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
  " `ts` DATETIME(3) NOT NULL,"
  " `conn_id` BIGINT UNSIGNED NOT NULL,"
  " `query_id` BIGINT UNSIGNED NOT NULL,"
  " `user` VARCHAR(384) NOT NULL DEFAULT '',"
  " `db` VARCHAR(192) NOT NULL DEFAULT '',"
  " `tables_involved` TEXT NOT NULL,"
  " `command` VARCHAR(32) NOT NULL DEFAULT '',"
  " `duration_ms` DOUBLE NULL,"
  " `error_code` INT NOT NULL DEFAULT 0,"
  " `query` MEDIUMTEXT NOT NULL,"
  " KEY `idx_selective_trace_ts` (`ts`)"
  ") ENGINE=Aria TRANSACTIONAL=0 DEFAULT CHARSET=utf8mb4";

static const size_t QUEUE_MAX_EVENTS= 10000;

/* Initialized at runtime in table_writer_init(): the PTHREAD_*_INITIALIZER
   static forms do not exist on Windows (CRITICAL_SECTION/CONDITION_VARIABLE
   need runtime init), so we cannot statically initialize these. */
static pthread_mutex_t q_mutex;
static pthread_cond_t q_cond;
static int sync_inited= 0;
static std::deque<std::string> *queue= NULL;
static int thread_running= 0;
static int stop_requested= 0;
static pthread_t writer_tid;

static unsigned long insert_failures= 0;
static unsigned long dropped_events= 0;
static unsigned long reconnect_count= 0;
static unsigned long repair_count= 0;
static unsigned int last_logged_errno= 0;

static MYSQL *conn= NULL;               /* writer thread only */

bool table_writer_is_self()
{
  bool self;
  pthread_mutex_lock(&q_mutex);
  self= thread_running && pthread_equal(pthread_self(), writer_tid);
  pthread_mutex_unlock(&q_mutex);
  return self;
}

static void close_conn()
{
  if (conn != NULL)
  {
    mysql_close(conn);
    conn= NULL;
  }
}

/* writer thread only */
static bool ensure_conn()
{
  if (conn != NULL)
    return true;
  conn= mysql_init(NULL);
  if (conn == NULL)
    return false;
  if (mysql_real_connect_local(conn) == NULL)
  {
    fprintf(stderr, "selective_trace: internal connection failed: %s\n",
            mysql_error(conn));
    mysql_close(conn);
    conn= NULL;
    return false;
  }
  /*
    Pin a known sql_mode on the writer session. sql_escape_append() escapes
    with backslashes, which is only valid when NO_BACKSLASH_ESCAPES is off.
    Without this, a server running with NO_BACKSLASH_ESCAPES would make our
    escaping ineffective — a "'" in the query text could break out of the
    INSERT string literal (SQL injection into the log table). We also drop
    STRICT so a single oversized field never aborts the row insert.
  */
  {
    static const char set_mode[]= "SET SESSION sql_mode=''";
    if (mysql_real_query(conn, set_mode,
                         (unsigned long) (sizeof(set_mode) - 1)))
      fprintf(stderr, "selective_trace: could not set writer sql_mode: %s\n",
              mysql_error(conn));
  }
  if (mysql_real_query(conn, CREATE_LOG_TABLE_SQL,
                       (unsigned long) (sizeof(CREATE_LOG_TABLE_SQL) - 1)))
    fprintf(stderr, "selective_trace: could not create " LOG_TABLE_FQN ": %s\n",
            mysql_error(conn));
  return true;
}

/* writer thread only */
static void run_insert(const std::string &sql)
{
  if (!ensure_conn())
  {
    insert_failures++;
    return;
  }

  if (mysql_real_query(conn, sql.data(), (unsigned long) sql.size()) == 0)
    return;

  unsigned int err= mysql_errno(conn);
  if (err == 1146 || err == 1049)       /* table/schema went missing */
  {
    mysql_real_query(conn, CREATE_LOG_TABLE_SQL,
                     (unsigned long) (sizeof(CREATE_LOG_TABLE_SQL) - 1));
    if (mysql_real_query(conn, sql.data(), (unsigned long) sql.size()) == 0)
      return;
    err= mysql_errno(conn);
  }
  /*
    The log table is Aria/TRANSACTIONAL=0 (fast, not crash-safe), so an
    unclean server shutdown can leave it marked as crashed. Without this,
    every later INSERT failed forever and tracing silently stopped writing
    until a human ran REPAIR by hand — surviving even UNINSTALL/INSTALL,
    since the damage is on disk, not in plugin state.

    Note these are the *raw* handler codes (my_base.h HA_ERR_CRASHED &
    friends), which is what actually reaches the client here — not the
    mapped ER_CRASHED_ON_USAGE/1194 one might expect. Verified against a
    deliberately corrupted Aria index: the writer sees errno 144/145.
  */
  else if (err == 126 || err == 127 || err == 144 || err == 145 ||
           err == 180 || err == 1194 || err == 1195 || err == 1034)
  {
    if (mysql_real_query(conn, REPAIR_LOG_TABLE_SQL,
                         (unsigned long) (sizeof(REPAIR_LOG_TABLE_SQL) - 1)) == 0)
    {
      /* REPAIR returns a result set; drain it or the connection desyncs. */
      MYSQL_RES *res= mysql_store_result(conn);
      if (res != NULL)
        mysql_free_result(res);
      repair_count++;
      if (mysql_real_query(conn, sql.data(), (unsigned long) sql.size()) == 0)
        return;
    }
    err= mysql_errno(conn);
  }
  else if (err == 2006 || err == 2013)  /* connection gone: retry once */
  {
    close_conn();
    if (ensure_conn() &&
        mysql_real_query(conn, sql.data(), (unsigned long) sql.size()) == 0)
      return;
    err= conn ? mysql_errno(conn) : err;
  }

  insert_failures++;
  if (err != last_logged_errno)         /* don't flood the error log */
  {
    last_logged_errno= err;
    fprintf(stderr, "selective_trace: INSERT into " LOG_TABLE_FQN
            " failed (errno %u): %s\n", err,
            conn ? mysql_error(conn) : "no connection");
  }
}

/*
  The writer keeps a single internal connection (and its THD) alive for the
  whole plugin lifetime, reused for every queued INSERT. Under sustained
  high-volume tracing that long-lived THD's memory is not returned to the OS
  at a matching rate (freed but fragmented, not leaked: confirmed with
  Valgrind, which reports 0 lost/reachable across a full run), so the
  server's RSS climbs without bound and can end in an OOM kill. Recycling
  the connection periodically bounds how much any single THD can accumulate.
*/
static const unsigned long RECONNECT_EVERY_N_INSERTS= 20000;

static void *writer_thread_func(void *arg __attribute__((unused)))
{
  my_thread_init();

  std::deque<std::string> batch;
  unsigned long since_reconnect= 0;
  for (;;)
  {
    pthread_mutex_lock(&q_mutex);
    while (queue->empty() && !stop_requested)
      pthread_cond_wait(&q_cond, &q_mutex);
    batch.swap(*queue);
    int stopping= stop_requested;
    pthread_mutex_unlock(&q_mutex);

    for (size_t i= 0; i < batch.size(); i++)
    {
      /* never let an exception kill the writer thread (and the server) */
      try
      {
        run_insert(batch[i]);
      }
      catch (...)
      {
        insert_failures++;
      }

      if (++since_reconnect >= RECONNECT_EVERY_N_INSERTS)
      {
        close_conn();
        since_reconnect= 0;
        reconnect_count++;
      }
    }
    batch.clear();

    if (stopping)
      break;
  }

  close_conn();
  my_thread_end();
  return NULL;
}

/* caller must hold q_mutex */
static bool start_thread_locked()
{
  if (thread_running)
    return true;
  stop_requested= 0;
  if (pthread_create(&writer_tid, NULL, writer_thread_func, NULL) != 0)
  {
    fprintf(stderr, "selective_trace: could not start table writer thread\n");
    return false;
  }
  thread_running= 1;
  return true;
}

void table_writer_init()
{
  /* Runtime init of the sync primitives — safe here because the plugin init
     path is single-threaded (no query events yet). Guarded so a re-INSTALL
     after shutdown re-initializes cleanly. */
  if (!sync_inited)
  {
    pthread_mutex_init(&q_mutex, NULL);
    pthread_cond_init(&q_cond, NULL);
    sync_inited= 1;
  }
  pthread_mutex_lock(&q_mutex);
  if (queue == NULL)
  {
    try
    {
      queue= new (std::nothrow) std::deque<std::string>();
    }
    catch (...)
    {
      queue= NULL;              /* enqueue() will refuse and drop */
    }
  }
  pthread_mutex_unlock(&q_mutex);
}

void table_writer_shutdown()
{
  pthread_t tid;
  int join_it= 0;

  pthread_mutex_lock(&q_mutex);
  if (thread_running)
  {
    stop_requested= 1;
    tid= writer_tid;
    join_it= 1;
    pthread_cond_signal(&q_cond);
  }
  pthread_mutex_unlock(&q_mutex);

  if (join_it)
    pthread_join(tid, NULL);

  pthread_mutex_lock(&q_mutex);
  thread_running= 0;
  delete queue;
  queue= NULL;
  pthread_mutex_unlock(&q_mutex);

  if (sync_inited)
  {
    pthread_mutex_destroy(&q_mutex);
    pthread_cond_destroy(&q_cond);
    sync_inited= 0;
  }
}

bool table_writer_enqueue(std::string *sql)
{
  bool ok= false;
  pthread_mutex_lock(&q_mutex);
  if (queue != NULL && start_thread_locked())
  {
    if (queue->size() >= QUEUE_MAX_EVENTS)
      dropped_events++;
    else
    {
      try
      {
        queue->push_back(std::string());
        queue->back().swap(*sql);        /* swap: noexcept */
        pthread_cond_signal(&q_cond);
        ok= true;
      }
      catch (...)
      {
        dropped_events++;       /* out of memory: drop this event */
      }
    }
  }
  pthread_mutex_unlock(&q_mutex);
  return ok;
}

unsigned long table_writer_failures()
{
  return insert_failures;
}

unsigned long table_writer_dropped()
{
  return dropped_events;
}

unsigned long table_writer_reconnects()
{
  return reconnect_count;
}

unsigned long table_writer_repairs()
{
  return repair_count;
}

void sql_escape_append(std::string *out, const char *src, size_t len)
{
  for (size_t i= 0; i < len; i++)
  {
    char c= src[i];
    switch (c)
    {
    case '\'': out->append("\\'", 2); break;
    case '\\': out->append("\\\\", 2); break;
    case '\0': out->append("\\0", 2); break;
    case '\n': out->append("\\n", 2); break;
    case '\r': out->append("\\r", 2); break;
    case '\032': out->append("\\Z", 2); break;
    default: out->push_back(c); break;
    }
  }
}

} /* namespace selective_trace */
