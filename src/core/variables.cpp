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

#include "variables.hpp"

#include <mutex>

namespace slugged {

namespace {

bool isSpace(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

std::string trim(const std::string &s)
{
	size_t begin = 0;
	size_t end = s.size();

	while (begin < end && isSpace(s[begin]))
		begin++;

	while (end > begin && isSpace(s[end - 1]))
		end--;

	return s.substr(begin, end - begin);
}

// Guards `g_values` and `g_generation` together: a reader that saw a new
// generation must see the values that produced it.
std::mutex &globalMutex()
{
	static std::mutex mutex;

	return mutex;
}

VariableMap g_values;
uint64_t g_generation = 0;

} // namespace

bool parseAssignment(const std::string &line, std::string &name, std::string &value)
{
	const std::string trimmed = trim(line);

	if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';')
		return false;

	const size_t sep = trimmed.find('=');

	if (sep == std::string::npos)
		return false;

	// Only the name is trimmed. The value keeps everything after the one space
	// a human naturally types around the '=', because a variable whose value is
	// deliberately " " or "  ..." is a real thing in an overlay.
	const std::string key = normaliseVariableName(trimmed.substr(0, sep));

	if (key.empty())
		return false;

	std::string raw = trimmed.substr(sep + 1);

	if (!raw.empty() && raw.front() == ' ')
		raw.erase(0, 1);

	name = key;
	value = std::move(raw);

	return true;
}

VariableMap parseAssignments(const std::string &text)
{
	VariableMap out;

	size_t start = 0;

	while (start <= text.size()) {
		const size_t end = text.find('\n', start);
		const size_t stop = end == std::string::npos ? text.size() : end;

		std::string name;
		std::string value;

		if (parseAssignment(text.substr(start, stop - start), name, value))
			out[name] = std::move(value);

		if (end == std::string::npos)
			break;

		start = end + 1;
	}

	return out;
}

std::string normaliseVariableName(const std::string &name)
{
	std::string out = trim(name);

	// A brace inside a name could never be written as a token, so a name
	// containing one is a typo rather than a value to store under an
	// unreachable key.
	if (out.find('{') != std::string::npos || out.find('}') != std::string::npos)
		return {};

	return out;
}

void GlobalVariables::set(const std::string &name, const std::string &value)
{
	const std::string key = normaliseVariableName(name);

	if (key.empty())
		return;

	std::lock_guard<std::mutex> lock(globalMutex());

	const auto it = g_values.find(key);

	if (it != g_values.end() && it->second == value)
		return;

	g_values[key] = value;
	g_generation++;
}

void GlobalVariables::erase(const std::string &name)
{
	const std::string key = normaliseVariableName(name);

	std::lock_guard<std::mutex> lock(globalMutex());

	if (g_values.erase(key) > 0)
		g_generation++;
}

void GlobalVariables::clear()
{
	std::lock_guard<std::mutex> lock(globalMutex());

	if (g_values.empty())
		return;

	g_values.clear();
	g_generation++;
}

void GlobalVariables::replace(const VariableMap &values)
{
	std::lock_guard<std::mutex> lock(globalMutex());

	if (g_values == values)
		return;

	g_values = values;
	g_generation++;
}

bool GlobalVariables::lookup(const std::string &name, std::string &out)
{
	std::lock_guard<std::mutex> lock(globalMutex());

	const auto it = g_values.find(name);

	if (it == g_values.end())
		return false;

	out = it->second;

	return true;
}

VariableMap GlobalVariables::snapshot()
{
	std::lock_guard<std::mutex> lock(globalMutex());

	return g_values;
}

uint64_t GlobalVariables::generation()
{
	std::lock_guard<std::mutex> lock(globalMutex());

	return g_generation;
}

} // namespace slugged
