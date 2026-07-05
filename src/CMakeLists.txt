# Copyright (C) 2026 selective_trace plugin authors
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
                 RECOMPILE_FOR_EMBEDDED)
