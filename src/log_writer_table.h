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
  log_writer_table — TABLE output mode for the selective_log plugin.

  Events are enqueued as ready-to-run INSERT statements and executed by a
  dedicated background thread over an internal SQL-service connection
  (mysql_real_connect_local). Running the SQL on a thread with no
  current_thd guarantees a dedicated internal THD (sql_log_bin=0,
  skip_grants — see sql/sql_prepare.cc:mysql_real_connect_local), never
  the THD of the user statement that produced the event. The self-log
  loop is broken by construction: the audit callback ignores every event
  raised from this thread (table_writer_is_self()).

  The log table (mysql.selective_log_events) is created lazily by the
  writer thread and recreated if an INSERT fails because it went missing.
*/

#ifndef SELECTIVE_LOG_LOG_WRITER_TABLE_H
#define SELECTIVE_LOG_LOG_WRITER_TABLE_H

#include <string>

namespace selective_log {

/* Called once from plugin init/deinit. shutdown() joins the thread. */
void table_writer_init();
void table_writer_shutdown();

/*
  Queue one INSERT statement; starts the writer thread on first use.
  Returns false if the queue is full (event dropped) or the thread could
  not be started.
*/
bool table_writer_enqueue(std::string *sql);

/* True when the calling thread is the writer thread (reentrancy guard). */
bool table_writer_is_self();

unsigned long table_writer_failures();
unsigned long table_writer_dropped();

/* Append src as a SQL single-quoted string body (no quotes added). */
void sql_escape_append(std::string *out, const char *src, size_t len);

} /* namespace selective_log */

#endif /* SELECTIVE_LOG_LOG_WRITER_TABLE_H */
