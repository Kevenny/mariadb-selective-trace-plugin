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

#include <my_global.h>
#include <my_pthread.h>
#include <mysql/plugin.h>          /* pulls mysql/services.h (sql service) */

#include <pthread.h>
#include <deque>
#include <string>
#include <cstdio>

#include "log_writer_table.h"

namespace selective_log {

#define LOG_TABLE_FQN "mysql.selective_log_events"

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
  " KEY `idx_selective_log_ts` (`ts`)"
  ") ENGINE=Aria TRANSACTIONAL=0 DEFAULT CHARSET=utf8mb4";

static const size_t QUEUE_MAX_EVENTS= 10000;

static pthread_mutex_t q_mutex= PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cond= PTHREAD_COND_INITIALIZER;
static std::deque<std::string> *queue= NULL;
static int thread_running= 0;
static int stop_requested= 0;
static pthread_t writer_tid;

static unsigned long insert_failures= 0;
static unsigned long dropped_events= 0;
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
    fprintf(stderr, "selective_log: internal connection failed: %s\n",
            mysql_error(conn));
    mysql_close(conn);
    conn= NULL;
    return false;
  }
  if (mysql_real_query(conn, CREATE_LOG_TABLE_SQL,
                       (unsigned long) (sizeof(CREATE_LOG_TABLE_SQL) - 1)))
    fprintf(stderr, "selective_log: could not create " LOG_TABLE_FQN ": %s\n",
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
    fprintf(stderr, "selective_log: INSERT into " LOG_TABLE_FQN
            " failed (errno %u): %s\n", err,
            conn ? mysql_error(conn) : "no connection");
  }
}

static void *writer_thread_func(void *arg __attribute__((unused)))
{
  my_thread_init();

  std::deque<std::string> batch;
  for (;;)
  {
    pthread_mutex_lock(&q_mutex);
    while (queue->empty() && !stop_requested)
      pthread_cond_wait(&q_cond, &q_mutex);
    batch.swap(*queue);
    int stopping= stop_requested;
    pthread_mutex_unlock(&q_mutex);

    for (size_t i= 0; i < batch.size(); i++)
      run_insert(batch[i]);
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
    fprintf(stderr, "selective_log: could not start table writer thread\n");
    return false;
  }
  thread_running= 1;
  return true;
}

void table_writer_init()
{
  pthread_mutex_lock(&q_mutex);
  if (queue == NULL)
    queue= new (std::nothrow) std::deque<std::string>();
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
      queue->push_back(std::string());
      queue->back().swap(*sql);
      pthread_cond_signal(&q_cond);
      ok= true;
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

} /* namespace selective_log */
