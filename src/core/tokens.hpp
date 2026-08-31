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

#include "variables.hpp"

#include <string>

namespace slugged {

// Template token expansion.
//
// Tokens are written {name} and are replaced at render time, so a source can
// show live values without the text itself being rewritten. A token may carry
// one argument after a colon -- {strftime:%H:%M} -- which is passed through
// verbatim, colons and all, so a format string needs no escaping.
//
// Values are resolved in this order, and the first table holding the name wins:
//
//   1. variables set on the source itself (the properties dialog's list, the
//      editor's Variables tab, and the source's own `set_variable` proc)
//   2. variables read from the source's watched variables file
//   3. the process-wide table, shared by every Slugged source
//   4. host values supplied by the OBS layer ({scene}, {fps}, {stream_time}...)
//   5. the clock and calendar built-ins resolved here
//
// User variables therefore shadow built-ins, so a stream can define its own
// {timer} without fighting the one below.
//
// Expansion is deliberately non-recursive: a value that itself contains braces
// is inserted literally rather than expanded again, so no input can produce an
// infinite loop.
class TokenContext {
public:
	// Sets a single variable on the source's own table. This is the entry point
	// the per-source `set_variable` proc handler uses.
	void set(const std::string &name, const std::string &value);

	void erase(const std::string &name);
	void clear();

	// Replaces a whole table in one step. Returns true when the contents
	// actually changed, so the caller can skip a re-layout.
	bool setSourceVariables(const VariableMap &values);
	bool setFileVariables(const VariableMap &values);

	// Values derived from the running OBS instance ({scene}, {fps},
	// {stream_time}...). They arrive as a plain table rather than as a callback
	// because several of them take locks that should not be taken from inside a
	// rebuild on the graphics thread; obs/host_tokens.cpp refreshes them on a
	// timer instead.
	bool setHostVariables(const VariableMap &values);

	// True when `text` contains at least one token, so callers can skip the
	// per-frame re-expansion entirely for static text.
	static bool hasTokens(const std::string &text);

	// Returns `text` with every recognised token replaced. Unknown tokens are
	// left exactly as written, which makes typos visible on screen rather than
	// silently blanking the text.
	std::string expand(const std::string &text) const;

	// Advances the built-in time tokens. `seconds` is the source's own running
	// time, used by {uptime} and {timer}. It is deliberately *not* the
	// animation clock: that one restarts whenever a motion preset replays,
	// which would leave a timer resetting itself every time it ticked over.
	void tick(float seconds) { _uptime = seconds; }

	const VariableMap &sourceVariables() const { return _source; }
	const VariableMap &fileVariables() const { return _file; }

private:
	bool lookup(const std::string &name, const std::string &arg, std::string &out) const;
	bool builtin(const std::string &name, const std::string &arg, std::string &out) const;

	VariableMap _source;
	VariableMap _file;
	VariableMap _host;

	float _uptime = 0.0f;
};

} // namespace slugged
