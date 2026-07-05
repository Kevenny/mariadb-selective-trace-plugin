/* Copyright (C) 2026 selective_trace plugin authors

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
  log_writer_file — FILE output mode for the selective_trace plugin.

  Thin wrapper around the MariaDB logger service (service_logger.h):
  byte-level thread safety comes from the service's internal mutex, this
  wrapper only guards the handle lifecycle (open/reopen on path change)
  with an rwlock so concurrent writers never serialize on each other.
*/

#ifndef SELECTIVE_TRACE_LOG_WRITER_FILE_H
#define SELECTIVE_TRACE_LOG_WRITER_FILE_H

#include <cstddef>
#include <string>

namespace selective_trace {

/* Called once from plugin init/deinit. */
void file_writer_init();
void file_writer_deinit();

/*
  (Re)open the log file at path. Safe to call while other threads write.
  Returns true on success; on failure the writer stays closed and
  file_writer_write() becomes a no-op returning false.
*/
bool file_writer_reopen(const char *path);

void file_writer_close();

/*
  Append one line (caller includes the trailing '\n'). Lazily opens the
  file on first use with the path given. Returns true if the line hit the
  file.
*/
bool file_writer_write(const char *line, size_t len, const char *path);

/* Total failed writes/opens since load (for diagnostics/status). */
unsigned long file_writer_failures();

/* Append src as a JSON string body (no quotes added) into *out. */
void json_escape_append(std::string *out, const char *src, size_t len);

} /* namespace selective_trace */

#endif /* SELECTIVE_TRACE_LOG_WRITER_FILE_H */
