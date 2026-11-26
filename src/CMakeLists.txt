# Copyright (C) 2026 Kevenny Ferraz
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

SET(SELECTIVE_TRACE_SOURCES
    selective_trace.cc
    filter_engine.cc
    filter_engine.h
    log_writer_file.cc
    log_writer_file.h
    log_writer_table.cc
    log_writer_table.h)

# RECOMPILE_FOR_EMBEDDED (mesmo padrão do server_audit): no Linux isso evita
# o -Wl,--no-undefined — o plugin referencia símbolos do servidor (PSI/mysys
# rwlock, thd_*) que só existem em runtime, resolvidos pelo mariadbd.
MYSQL_ADD_PLUGIN(selective_trace ${SELECTIVE_TRACE_SOURCES} MODULE_ONLY
                 RECOMPILE_FOR_EMBEDDED
                 LINK_LIBRARIES ${CMAKE_DL_LIBS})

if(WIN32 AND TARGET selective_trace)
  # None of MariaDB's plugin "services" (libservices/*.c — sql_service,
  # debug_sync_service, etc.) are declared MYSQL_PLUGIN_EXPORT, so on a
  # normal MSVC DLL (nothing exported unless marked) the server's
  # dlsym()-equivalent lookup (sql_plugin.cc, GetProcAddress by symbol
  # name) never finds "sql_service" in our .dll and can't inject the real
  # function table — the plugin keeps the compile-time version-marker
  # value from sql_service.c and crashes on first use (TABLE output's
  # mysql_init()/mysql_real_connect_local(), i.e. every SET GLOBAL
  # selective_trace_output=... on Windows). On Linux this is a non-issue:
  # ELF .so symbols are exported by default, so dlsym() always finds them.
  # WINDOWS_EXPORT_ALL_SYMBOLS reproduces that default for our .dll without
  # touching MariaDB's own source. Reproduced and confirmed on a live
  # Windows MariaDB service before this fix.
  set_target_properties(selective_trace PROPERTIES
                         WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
endif()
