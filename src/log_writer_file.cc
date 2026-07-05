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

#include <my_global.h>
#include <my_pthread.h>
#include <mysql_version.h>            /* MYSQL_VERSION_ID */
#include <mysql/psi/mysql_thread.h>
#include <mysql/service_logger.h>

#include <cstdio>

#include "log_writer_file.h"

/*
  Logger service ABI change in MariaDB 12.x: logger_open() gained a
  buffer_size argument (the service now buffers internally). 0 keeps the
  legacy unbuffered behavior. Wrap it so the call site stays version-clean.
*/
#if MYSQL_VERSION_ID >= 120000
#  define SELECTIVE_TRACEGER_OPEN(path, size, rot) \
     logger_open((path), (size), (rot), 0)
#else
#  define SELECTIVE_TRACEGER_OPEN(path, size, rot) \
     logger_open((path), (size), (rot))
#endif

namespace selective_trace {

/*
  No size-based rotation: selective_trace delegates retention to external
  tooling (logrotate & friends), so the limit is effectively infinite.
*/
static const unsigned long long NO_ROTATION_SIZE= 0x7FFFFFFFFFFFFFFFULL;

static mysql_rwlock_t log_lock;
static LOGGER_HANDLE *log_handle= NULL;
static unsigned long write_failures= 0;
static int open_failed_logged= 0;   /* avoid flooding the error log */

#ifdef HAVE_PSI_INTERFACE
static PSI_rwlock_key key_rwlock_logfile;
static PSI_rwlock_info log_rwlock_list[]=
{
  { &key_rwlock_logfile, "SELECTIVE_TRACE::log_file_lock", PSI_FLAG_GLOBAL }
};
#else
#define key_rwlock_logfile 0
#endif

void file_writer_init()
{
  logger_init_mutexes();
#ifdef HAVE_PSI_INTERFACE
  if (PSI_server)
    PSI_server->register_rwlock("selective_trace", log_rwlock_list, 1);
#endif
  mysql_rwlock_init(key_rwlock_logfile, &log_lock);
}

void file_writer_deinit()
{
  file_writer_close();
  mysql_rwlock_destroy(&log_lock);
}

static bool open_locked(const char *path)
{
  if (path == NULL || path[0] == '\0')
    return false;
  log_handle= SELECTIVE_TRACEGER_OPEN(path, NO_ROTATION_SIZE, 0);
  if (log_handle == NULL)
  {
    if (!open_failed_logged)
    {
      open_failed_logged= 1;
      fprintf(stderr, "selective_trace: could not open log file '%s'\n", path);
    }
    write_failures++;
    return false;
  }
  open_failed_logged= 0;
  return true;
}

bool file_writer_reopen(const char *path)
{
  bool ok;
  mysql_rwlock_wrlock(&log_lock);
  if (log_handle != NULL)
  {
    logger_close(log_handle);
    log_handle= NULL;
  }
  ok= open_locked(path);
  mysql_rwlock_unlock(&log_lock);
  return ok;
}

void file_writer_close()
{
  mysql_rwlock_wrlock(&log_lock);
  if (log_handle != NULL)
  {
    logger_close(log_handle);
    log_handle= NULL;
  }
  mysql_rwlock_unlock(&log_lock);
}

bool file_writer_write(const char *line, size_t len, const char *path)
{
  bool ok= false;

  mysql_rwlock_rdlock(&log_lock);
  if (log_handle == NULL)
  {
    /* lazy open: upgrade to the write lock */
    mysql_rwlock_unlock(&log_lock);
    mysql_rwlock_wrlock(&log_lock);
    if (log_handle == NULL && !open_locked(path))
    {
      mysql_rwlock_unlock(&log_lock);
      return false;
    }
  }

  if (logger_write(log_handle, line, len) == (int) len)
    ok= true;
  else
    write_failures++;
  mysql_rwlock_unlock(&log_lock);
  return ok;
}

unsigned long file_writer_failures()
{
  return write_failures;
}

void json_escape_append(std::string *out, const char *src, size_t len)
{
  static const char hexdig[]= "0123456789abcdef";
  for (size_t i= 0; i < len; i++)
  {
    unsigned char c= (unsigned char) src[i];
    switch (c)
    {
    case '"':  out->append("\\\"", 2); break;
    case '\\': out->append("\\\\", 2); break;
    case '\b': out->append("\\b", 2); break;
    case '\f': out->append("\\f", 2); break;
    case '\n': out->append("\\n", 2); break;
    case '\r': out->append("\\r", 2); break;
    case '\t': out->append("\\t", 2); break;
    default:
      if (c < 0x20)
      {
        char buf[7]= { '\\', 'u', '0', '0',
                       hexdig[(c >> 4) & 0xF], hexdig[c & 0xF], 0 };
        out->append(buf, 6);
      }
      else
        out->push_back((char) c);
      break;
    }
  }
}

} /* namespace selective_trace */
