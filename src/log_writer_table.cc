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

#ifdef _WIN32
#  include <windows.h>             /* GetModuleHandleEx (pin the DLL) */
#else
#  include <dlfcn.h>               /* dladdr/dlopen (pin the .so) */
#endif

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

/*
  The writer's internal connection is invisible to SHOW PROCESSLIST and
  cannot be KILLed. Left at the server default (lock_wait_timeout=86400) an
  INSERT blocked behind FLUSH TABLES WITH READ LOCK or LOCK TABLES on the
  log table would hang for a day, and an UNINSTALL PLUGIN meanwhile would
  hang in pthread_join for as long. Bound every lock wait instead: a
  blocked INSERT fails after this many seconds (counted as a write failure)
  and the writer moves on. lock_wait_timeout covers both metadata locks and
  table-level (thr_lock) waits — see sql/lock.cc.
*/
static const char WRITER_SESSION_SQL[]=
  "SET SESSION lock_wait_timeout=5";

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
static int module_pinned= 0;            /* protected by q_mutex */

static unsigned long insert_failures= 0;
static unsigned long dropped_events= 0;
static unsigned long reconnect_count= 0;
static unsigned long repair_count= 0;
static unsigned int last_logged_errno= 0;

/*
  Set by table_writer_shutdown() when it finds itself running ON the writer
  thread (see there). Thread-local on purpose: it is written and read by the
  same thread, so it needs no lock, and a writer thread started by a later
  INSTALL PLUGIN never sees the flag of the one that is still unwinding.
*/
static thread_local int shutdown_on_self= 0;

bool table_writer_is_self()
{
  bool self;
  pthread_mutex_lock(&q_mutex);
  self= thread_running && pthread_equal(pthread_self(), writer_tid);
  pthread_mutex_unlock(&q_mutex);
  return self;
}

/*
  Keep this shared object mapped for the lifetime of the process.

  Every statement the writer runs on its internal connection goes through
  the audit interface like any other, so the writer's THD holds a reference
  on this very plugin while its INSERT executes and releases it right after
  (sql/sql_prepare.cc:loc_advanced_command -> mysql_audit_release). If an
  UNINSTALL PLUGIN lands while the writer is mid-INSERT and the writer ends
  up being the *last* holder, the server reaps the plugin on the writer
  thread itself: deinit runs inside the writer's own INSERT, and right after
  deinit the server dlclose()s the .so — while the writer's stack still
  points into it. The return from that INSERT would land in unmapped code.

  Pinning is the documented remedy for "library with a thread that may
  outlive dlclose": RTLD_NODELETE (plus the extra handle reference we never
  release) makes the server's dlclose a no-op on the mapping, and the writer
  unwinds through valid code. The server explicitly supports a DSO that is
  not unloaded (sql/sql_plugin.cc:restore_ptr_backup), so a later INSTALL
  PLUGIN reuses the mapping cleanly. Cost: the .so stays mapped after
  UNINSTALL. Done lazily, only when TABLE output actually starts a writer —
  FILE-only deployments keep the normal unload.
*/
static bool pin_module_in_memory()
{
#ifdef _WIN32
  HMODULE h= NULL;
  return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (LPCSTR) &module_pinned, &h) != 0;
#else
  Dl_info info;
  if (dladdr((void *) &module_pinned, &info) == 0 || info.dli_fname == NULL)
    return false;
  return dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE)
         != NULL;
#endif
}

static void close_conn(MYSQL **conn)
{
  if (*conn != NULL)
  {
    mysql_close(*conn);
    *conn= NULL;
  }
}

/* writer thread only */
static bool ensure_conn(MYSQL **conn)
{
  if (*conn != NULL)
    return true;
  MYSQL *c= mysql_init(NULL);
  if (c == NULL)
    return false;
  if (mysql_real_connect_local(c) == NULL)
  {
    fprintf(stderr, "selective_trace: internal connection failed: %s\n",
            mysql_error(c));
    mysql_close(c);
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
    if (mysql_real_query(c, set_mode,
                         (unsigned long) (sizeof(set_mode) - 1)))
      fprintf(stderr, "selective_trace: could not set writer sql_mode: %s\n",
              mysql_error(c));
  }
  if (mysql_real_query(c, WRITER_SESSION_SQL,
                       (unsigned long) (sizeof(WRITER_SESSION_SQL) - 1)))
    fprintf(stderr, "selective_trace: could not set writer lock timeout:"
            " %s\n", mysql_error(c));
  if (mysql_real_query(c, CREATE_LOG_TABLE_SQL,
                       (unsigned long) (sizeof(CREATE_LOG_TABLE_SQL) - 1)))
    fprintf(stderr, "selective_trace: could not create " LOG_TABLE_FQN ": %s\n",
            mysql_error(c));
  *conn= c;
  return true;
}

/*
  writer thread only. Returns true if the row was inserted. A false return
  has already been accounted (insert_failures or dropped_events).
*/
static bool run_insert(MYSQL **conn, const std::string &sql)
{
  if (!ensure_conn(conn))
  {
    insert_failures++;
    return false;
  }
  if (shutdown_on_self)                 /* plugin deinitialized inside the
                                           session setup above: don't run
                                           anything else on its behalf */
  {
    dropped_events++;
    return false;
  }

  if (mysql_real_query(*conn, sql.data(), (unsigned long) sql.size()) == 0)
    return true;

  unsigned int err= mysql_errno(*conn);

  /* The plugin was deinitialized inside that statement (UNINSTALL reaped on
     this thread): the failure is real, but no retries on a dead plugin. */
  if (shutdown_on_self)
  {
    insert_failures++;
    return false;
  }

  if (err == 1146 || err == 1049)       /* table/schema went missing */
  {
    mysql_real_query(*conn, CREATE_LOG_TABLE_SQL,
                     (unsigned long) (sizeof(CREATE_LOG_TABLE_SQL) - 1));
    if (mysql_real_query(*conn, sql.data(), (unsigned long) sql.size()) == 0)
      return true;
    err= mysql_errno(*conn);
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
    if (mysql_real_query(*conn, REPAIR_LOG_TABLE_SQL,
                         (unsigned long) (sizeof(REPAIR_LOG_TABLE_SQL) - 1)) == 0)
    {
      /* REPAIR returns a result set; drain it or the connection desyncs. */
      MYSQL_RES *res= mysql_store_result(*conn);
      if (res != NULL)
        mysql_free_result(res);
      repair_count++;
      if (mysql_real_query(*conn, sql.data(), (unsigned long) sql.size()) == 0)
        return true;
    }
    err= mysql_errno(*conn);
  }
  else if (err == 2006 || err == 2013)  /* connection gone: retry once */
  {
    close_conn(conn);
    if (ensure_conn(conn) &&
        mysql_real_query(*conn, sql.data(), (unsigned long) sql.size()) == 0)
      return true;
    err= *conn ? mysql_errno(*conn) : err;
  }

  insert_failures++;
  if (err != last_logged_errno)         /* don't flood the error log */
  {
    last_logged_errno= err;
    fprintf(stderr, "selective_trace: INSERT into " LOG_TABLE_FQN
            " failed (errno %u): %s\n", err,
            *conn ? mysql_error(*conn) : "no connection");
  }
  return false;
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

static int stop_requested_now()
{
  int s;
  pthread_mutex_lock(&q_mutex);
  s= stop_requested;
  pthread_mutex_unlock(&q_mutex);
  return s;
}

static void *writer_thread_func(void *arg __attribute__((unused)))
{
  my_thread_init();

  /* The connection is private to this thread (not a global): a writer that
     is still unwinding after a self-shutdown (see table_writer_shutdown)
     must never race with the writer of a subsequent INSTALL PLUGIN. */
  MYSQL *conn= NULL;
  std::deque<std::string> batch;
  unsigned long since_reconnect= 0;
  int exit_now= 0;
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
      bool ok;
      /* never let an exception kill the writer thread (and the server) */
      try
      {
        ok= run_insert(&conn, batch[i]);
      }
      catch (...)
      {
        insert_failures++;
        ok= false;
      }

      if (shutdown_on_self)
      {
        /* deinit ran on this very thread inside the statement above: the
           plugin is gone and the queue was freed by table_writer_shutdown.
           Drop what is left of the batch and leave without touching q_mutex
           or q_cond again. */
        dropped_events+= (unsigned long) (batch.size() - i - 1);
        exit_now= 1;
        break;
      }

      if (!ok && stop_requested_now())
      {
        /* Shutting down and INSERTs are failing (lock wait timeout, table
           gone...): draining the rest would make deinit wait one timeout
           per queued event. Drop them instead. */
        dropped_events+= (unsigned long) (batch.size() - i - 1);
        stopping= 1;
        break;
      }

      if (++since_reconnect >= RECONNECT_EVERY_N_INSERTS)
      {
        close_conn(&conn);
        since_reconnect= 0;
        reconnect_count++;
      }
    }
    batch.clear();

    if (exit_now || stopping)
      break;
  }

  close_conn(&conn);
  my_thread_end();
  return NULL;
}

/* caller must hold q_mutex */
static bool start_thread_locked()
{
  if (thread_running)
    return true;
  if (!module_pinned)
  {
    if (pin_module_in_memory())
      module_pinned= 1;
    else
      fprintf(stderr, "selective_trace: could not pin the plugin module in"
              " memory; an UNINSTALL PLUGIN racing with the table writer"
              " may crash the server\n");
  }
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

  if (!sync_inited)
    return;

  pthread_mutex_lock(&q_mutex);
  if (thread_running && pthread_equal(pthread_self(), writer_tid))
  {
    /*
      deinit is running ON the writer thread: the server reaped the plugin
      from the audit release at the end of one of our own INSERTs (UNINSTALL
      raced with the writer, see pin_module_in_memory). We cannot join
      ourselves — pthread_join(self) returns EDEADLK on POSIX and hangs on
      Windows. Tell the loop to exit as soon as the statement unwinds
      (shutdown_on_self is checked right after run_insert), free the queue
      here, and leave q_mutex/q_cond initialized: the thread must not touch
      them again and a later table_writer_init() reuses them (sync_inited).
      The .so stays mapped (pinned), so unwinding through it is safe.
    */
    stop_requested= 1;
    thread_running= 0;
    shutdown_on_self= 1;
    if (queue != NULL)
    {
      dropped_events+= (unsigned long) queue->size();
      delete queue;
      queue= NULL;
    }
    pthread_mutex_unlock(&q_mutex);
#ifndef _WIN32
    pthread_detach(pthread_self());     /* nobody will join us */
#endif
    return;
  }
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
  if (queue != NULL)
  {
    dropped_events+= (unsigned long) queue->size();   /* enqueued after the
                                                          writer's last swap */
    delete queue;
    queue= NULL;
  }
  pthread_mutex_unlock(&q_mutex);

  pthread_mutex_destroy(&q_mutex);
  pthread_cond_destroy(&q_cond);
  sync_inited= 0;
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
