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

#include "tokens.hpp"

#include <cstdio>
#include <ctime>

namespace slugged {

void TokenContext::set(const std::string &name, const std::string &value)
{
	const std::string key = normaliseVariableName(name);

	if (!key.empty())
		_source[key] = value;
}

void TokenContext::erase(const std::string &name)
{
	_source.erase(normaliseVariableName(name));
}

void TokenContext::clear()
{
	_source.clear();
	_file.clear();
}

bool TokenContext::setSourceVariables(const VariableMap &values)
{
	if (_source == values)
		return false;

	_source = values;

	return true;
}

bool TokenContext::setFileVariables(const VariableMap &values)
{
	if (_file == values)
		return false;

	_file = values;

	return true;
}

bool TokenContext::setHostVariables(const VariableMap &values)
{
	if (_host == values)
		return false;

	_host = values;

	return true;
}

bool TokenContext::hasTokens(const std::string &text)
{
	return text.find('{') != std::string::npos;
}

namespace {

std::string formatTime(const char *format)
{
	const std::time_t now = std::time(nullptr);
	std::tm local{};

#if defined(_WIN32)
	localtime_s(&local, &now);
#else
	localtime_r(&now, &local);
#endif

	char buf[256] = {0};

	if (!std::strftime(buf, sizeof(buf), format, &local))
		return {};

	return buf;
}

std::string formatDuration(float seconds)
{
	if (seconds < 0.0f)
		seconds = 0.0f;

	const int total = int(seconds);
	const int h = total / 3600;
	const int m = (total / 60) % 60;
	const int s = total % 60;

	char buf[32];

	if (h > 0)
		std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
	else
		std::snprintf(buf, sizeof(buf), "%d:%02d", m, s);

	return buf;
}

} // namespace

bool TokenContext::builtin(const std::string &name, const std::string &arg, std::string &out) const
{
	// An arbitrary format string, so a layout that wants something the named
	// tokens do not cover does not need a new token to be added here.
	if (name == "strftime") {
		if (arg.empty())
			return false;

		out = formatTime(arg.c_str());

		return true;
	}

	struct Named {
		const char *name;
		const char *format;
	};

	static const Named kFormats[] = {
		{"time", "%H:%M"},         {"time12", "%I:%M %p"}, {"seconds", "%H:%M:%S"}, {"date", "%Y-%m-%d"},
		{"date_long", "%d %B %Y"}, {"weekday", "%A"},      {"month", "%B"},         {"year", "%Y"},
	};

	for (const Named &format : kFormats) {
		if (name == format.name) {
			out = formatTime(format.format);
			return true;
		}
	}

	if (name == "uptime" || name == "timer") {
		out = formatDuration(_uptime);
		return true;
	}

	return false;
}

bool TokenContext::lookup(const std::string &name, const std::string &arg, std::string &out) const
{
	// A variable is matched on the whole token text, argument included, so
	// setting a variable literally named "strftime:%H" is possible even though
	// nothing sensible would.
	const std::string full = arg.empty() ? name : name + ":" + arg;

	for (const VariableMap *table : {&_source, &_file}) {
		const auto it = table->find(full);

		if (it != table->end()) {
			out = it->second;
			return true;
		}
	}

	if (GlobalVariables::lookup(full, out))
		return true;

	const auto host = _host.find(full);

	if (host != _host.end()) {
		out = host->second;
		return true;
	}

	return builtin(name, arg, out);
}

std::string TokenContext::expand(const std::string &text) const
{
	std::string out;
	out.reserve(text.size());

	size_t i = 0;

	while (i < text.size()) {
		if (text[i] != '{') {
			out += text[i++];
			continue;
		}

		// "{{" is an escape for a literal brace.
		if (i + 1 < text.size() && text[i + 1] == '{') {
			out += '{';
			i += 2;
			continue;
		}

		const size_t close = text.find('}', i + 1);

		if (close == std::string::npos) {
			out += text[i++];
			continue;
		}

		const std::string body = text.substr(i + 1, close - i - 1);

		// Everything past the *first* colon is the argument, so a format
		// string full of colons arrives intact.
		const size_t colon = body.find(':');

		const std::string name = colon == std::string::npos ? body : body.substr(0, colon);
		const std::string arg = colon == std::string::npos ? std::string() : body.substr(colon + 1);

		std::string value;

		if (!name.empty() && lookup(name, arg, value)) {
			// Inserted literally: a value containing braces is never
			// re-scanned, so expansion always terminates.
			out += value;
		} else {
			// Unknown token stays visible so the mistake is obvious on
			// screen instead of silently blanking the text.
			out += text.substr(i, close - i + 1);
		}

		i = close + 1;
	}

	return out;
}

} // namespace slugged
