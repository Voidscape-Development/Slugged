/*
Slugged - GPU vector text for OBS Studio
Copyright (C) 2026 Voidscape Development

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "log.hpp"

namespace slugged {

namespace {
LogSink g_sink = nullptr;
}

void setLogSink(LogSink sink)
{
	g_sink = sink;
}

void slugged_log(int level, const char *format, ...)
{
	if (!g_sink)
		return;

	va_list args;
	va_start(args, format);

	g_sink(level, format, args);

	va_end(args);
}

} // namespace slugged
