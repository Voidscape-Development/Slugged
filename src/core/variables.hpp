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

#include <cstdint>
#include <map>
#include <string>

namespace slugged {

// Named values a token can resolve to.
//
// A Slugged source reads three of these, in priority order: the table the user
// filled in on the source itself, the table read from the source's watched
// variables file, and the process-wide table below. Whichever holds the name
// first wins, so a source can override a global value locally without the two
// having to know about each other.
using VariableMap = std::map<std::string, std::string>;

// Splits one "name=value" line. Whitespace around the name is trimmed and the
// value is taken verbatim after the first '=', so a value may itself contain
// '=' and may have significant leading spaces once past the separator's own.
//
// Returns false for a blank line, a comment ('#' or ';' first), or a line with
// no separator or an empty name -- all of which a hand-edited file will have.
bool parseAssignment(const std::string &line, std::string &name, std::string &value);

// Parses a whole "name=value" document, one assignment per line. Later
// assignments to the same name win, which is what a file appended to by a bot
// wants.
VariableMap parseAssignments(const std::string &text);

// Variable names are matched exactly as they appear between the braces, so this
// only rejects the characters that could never be typed in a token: braces
// themselves, and leading or trailing whitespace that the user cannot see.
std::string normaliseVariableName(const std::string &name);

// The process-wide table, shared by every Slugged source in the running OBS
// instance.
//
// Scripts push into this from whatever thread they happen to run on while the
// graphics thread reads it, so every access is locked. Reads copy rather than
// return a reference for the same reason.
class GlobalVariables {
public:
	static void set(const std::string &name, const std::string &value);
	static void erase(const std::string &name);
	static void clear();

	// Replaces the whole table in one step, so loading the config file cannot
	// leave a half-applied state visible to a source mid-frame.
	static void replace(const VariableMap &values);

	static bool lookup(const std::string &name, std::string &out);
	static VariableMap snapshot();

	// Bumped on every mutation. A source compares this against the value it
	// last expanded with, so it can skip re-expanding text that no global
	// variable has touched.
	static uint64_t generation();
};

} // namespace slugged
