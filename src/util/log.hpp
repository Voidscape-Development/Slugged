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

#pragma once

#include <cstdarg>

// Logging seam.
//
// core/, text/ and most of render/ are deliberately free of libobs so the text
// pipeline can be built and tested without an OBS instance. They still need to
// report problems, so they log through this indirection and plugin-main.cpp
// points it at obs_log() once the module loads. The test harness points it at
// stderr instead.
//
// Levels match libobs's values so the sink can forward them unchanged.

namespace slugged {

constexpr int LOG_ERROR = 100;
constexpr int LOG_WARNING = 200;
constexpr int LOG_INFO = 300;
constexpr int LOG_DEBUG = 400;

using LogSink = void (*)(int level, const char *format, va_list args);

// Installs the sink. Passing nullptr silences logging entirely.
void setLogSink(LogSink sink);

void slugged_log(int level, const char *format, ...);

} // namespace slugged

using slugged::slugged_log;
