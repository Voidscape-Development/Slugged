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

#include <ctime>
#include <string>

namespace slugged {

// Template token expansion.
//
// Tokens are written {name} and are replaced at render time. A token may carry
// one argument after a colon -- {strftime:%H:%M} -- passed through verbatim,
// colons and all.
//
// Values are resolved in this order, and the first table holding the name wins:
//
//   1. values a script set on the source at runtime (its set_variable proc)
//   2. variables set on the source itself (properties list, editor)
//   3. variables read from the source's watched variables file
//   4. the process-wide table shared by every Slugged source
//   5. host values supplied by the OBS layer ({scene}, {fps}, {stream_time}...)
//   6. the clock, calendar and timer built-ins resolved here
//
// The first four are *content*: a value changing there is a real change to
// what the text says, and replays a text-change animation. The last two are
// *live*: they tick over on their own, and a clock advancing is not a reason to
// replay an intro.
//
// Expansion is non-recursive: a value containing braces is inserted literally,
// so no input can produce an infinite loop.
class TokenContext {
public:
	// The runtime table: what a script pushes in. It is not saved, and it does
	// not fight the settings list, which a settings update replaces wholesale.
	void set(const std::string &name, const std::string &value);
	void erase(const std::string &name);
	void clearRuntime() { _runtime.clear(); }

	// Replace a whole table. Returns true when the contents changed.
	bool setSourceVariables(const VariableMap &values);
	bool setFileVariables(const VariableMap &values);
	bool setHostVariables(const VariableMap &values);

	static bool hasTokens(const std::string &text);

	// Every recognised token replaced. Unknown tokens stay exactly as written so
	// a typo is visible on screen.
	std::string expand(const std::string &text) const;

	// Only content tokens replaced; live ones are left as written. Two texts
	// whose content expansions are equal differ only by clocks ticking.
	std::string expandContent(const std::string &text) const;

	// The source's running time, for {uptime}.
	void setUptime(float seconds) { _uptime = seconds; }

	// The source's stopwatch, for {timer} and duration countdowns.
	void setTimer(float seconds) { _timer = seconds; }

	// Pins "now" for the clock tokens. Zero means the real time; tests use it.
	void setClock(std::time_t now) { _clock = now; }

	const VariableMap &sourceVariables() const { return _source; }
	const VariableMap &fileVariables() const { return _file; }

private:
	enum class Origin { None, Content, Live };

	Origin lookup(const std::string &name, const std::string &arg, std::string &out) const;
	bool builtin(const std::string &name, const std::string &arg, std::string &out) const;
	std::string expandImpl(const std::string &text, bool liveToo) const;

	VariableMap _runtime;
	VariableMap _source;
	VariableMap _file;
	VariableMap _host;

	float _uptime = 0.0f;
	float _timer = 0.0f;
	std::time_t _clock = 0;
};

// "1:05", "1:02:03" -- the format every duration token uses.
std::string formatDuration(double seconds);

// Parses "90", "1:30" or "1:02:03" into seconds. Returns a negative value when
// `text` is not a duration.
double parseDuration(const std::string &text);

// Parses "YYYY-MM-DD HH:MM[:SS]" (local time) or "HH:MM[:SS]" (the next time
// that clock time comes around). Returns false when `text` is neither.
bool parseTarget(const std::string &text, std::time_t now, std::time_t &out);

} // namespace slugged
