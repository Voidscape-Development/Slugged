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

#include <map>
#include <string>

namespace slugged {

// Template token expansion.
//
// Tokens are written {name} and are replaced at render time, so a source can
// show live values without the text itself being rewritten. Values come from
// three places, in priority order: variables the user set on the source,
// variables pushed in by scripts or obs-websocket, and a small set of built-ins
// resolved here (clock, date, uptime).
//
// Expansion is deliberately non-recursive: a value that itself contains braces
// is inserted literally rather than expanded again, so no input can produce an
// infinite loop.
class TokenContext {
public:
	// Sets a user or script-provided variable.
	void set(const std::string &name, const std::string &value);

	void clear();

	// True when `text` contains at least one token, so callers can skip the
	// per-frame re-layout entirely for static text.
	static bool hasTokens(const std::string &text);

	// Returns `text` with every recognised token replaced. Unknown tokens are
	// left exactly as written, which makes typos visible on screen rather than
	// silently blanking the text.
	std::string expand(const std::string &text) const;

	// Advances built-in time-based tokens. `seconds` is the source's own
	// running time, used by {uptime} and {timer}.
	void tick(float seconds) { _elapsed = seconds; }

	const std::map<std::string, std::string> &variables() const { return _vars; }

private:
	bool lookup(const std::string &name, std::string &out) const;
	bool builtin(const std::string &name, std::string &out) const;

	std::map<std::string, std::string> _vars;
	float _elapsed = 0.0f;
};

} // namespace slugged
