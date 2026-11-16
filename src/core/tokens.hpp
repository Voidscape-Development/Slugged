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

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

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
//
// A token may be followed by filters, each introduced by a bar, that reshape
// its value: {followers|number}, {name|upper|truncate:12}, {timer|mm:ss},
// {message|default:Nothing yet}. See applyFilter() for the list. A token with
// no value stays as written unless a `default` filter supplies one.
// Where one replaced token came from and went to: [srcStart, srcEnd) in the
// text as written, [dstStart, dstEnd) in the expanded text. The editor uses
// these to treat a live value on the canvas as the one token it stands for.
struct TokenSpan {
	uint32_t srcStart = 0;
	uint32_t srcEnd = 0;
	uint32_t dstStart = 0;
	uint32_t dstEnd = 0;

	bool operator==(const TokenSpan &) const = default;
};

class TokenContext {
public:
	// The runtime table: what a script pushes in. It is not saved, and it does
	// not fight the settings list, which a settings update replaces wholesale.
	void set(const std::string &name, const std::string &value);
	void erase(const std::string &name);
	void clearRuntime() { _runtime.clear(); }

	// Where a name's value comes from, among the tables a script or the user
	// can write. Live values (host and built-ins) are not reported.
	enum class Layer { None, Runtime, Source, File, Global };
	Layer find(const std::string &name, std::string &value) const;

	// Replace a whole table. Returns true when the contents changed.
	bool setSourceVariables(const VariableMap &values);
	bool setFileVariables(const VariableMap &values);
	bool setHostVariables(const VariableMap &values);

	static bool hasTokens(const std::string &text);

	// Every recognised token replaced. Unknown tokens stay exactly as written so
	// a typo is visible on screen.
	std::string expand(const std::string &text) const;

	// The same, also recording every span that was replaced (a "{{" escape
	// included, since it too changes the length).
	std::string expand(const std::string &text, std::vector<TokenSpan> &spans) const;

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
	std::string expandImpl(const std::string &text, bool liveToo, std::vector<TokenSpan> *spans) const;

	VariableMap _runtime;
	VariableMap _source;
	VariableMap _file;
	VariableMap _host;

	float _uptime = 0.0f;
	float _timer = 0.0f;
	std::time_t _clock = 0;
};

// Applies one filter ("upper", "pad:3") to a token's value. Unknown filters
// leave the value as it was. `default` is handled by the caller, because it
// also applies when there is no value at all.
std::string applyFilter(const std::string &value, const std::string &filter);

// The text `to` with every number that differs from the one at the same place
// in `from` part of the way there: at `t` = 0 it reads as `from`, at 1 as
// `to`. Each number keeps `to`'s formatting (decimals, thousands separators).
// Returns false when the two do not have the same words around the same count
// of numbers, so there is nothing sensible to count through.
bool interpolateNumbers(const std::string &from, const std::string &to, double t, std::string &out);

// A number as plain text: no exponent and no trailing zeros ("12", "0.5").
std::string formatPlainNumber(double value);

// Reads a number written the way an overlay shows one: "1234", "-3.5",
// "1,234,567". Returns false when `text` is anything else.
bool parseNumber(const std::string &text, double &out);

// "1:05", "1:02:03" -- the format every duration token uses.
std::string formatDuration(double seconds);

// Parses "90", "1:30" or "1:02:03" into seconds. Returns a negative value when
// `text` is not a duration.
double parseDuration(const std::string &text);

// Parses "YYYY-MM-DD HH:MM[:SS]" (local time) or "HH:MM[:SS]" (the next time
// that clock time comes around). Returns false when `text` is neither.
bool parseTarget(const std::string &text, std::time_t now, std::time_t &out);

} // namespace slugged
