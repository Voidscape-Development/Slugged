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
	_vars[name] = value;
}

void TokenContext::clear()
{
	_vars.clear();
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

	char buf[128] = {0};

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

bool TokenContext::builtin(const std::string &name, std::string &out) const
{
	if (name == "time") {
		out = formatTime("%H:%M");
		return true;
	}

	if (name == "time12") {
		out = formatTime("%I:%M %p");
		return true;
	}

	if (name == "seconds") {
		out = formatTime("%H:%M:%S");
		return true;
	}

	if (name == "date") {
		out = formatTime("%Y-%m-%d");
		return true;
	}

	if (name == "uptime" || name == "timer") {
		out = formatDuration(_elapsed);
		return true;
	}

	return false;
}

bool TokenContext::lookup(const std::string &name, std::string &out) const
{
	// User and script variables shadow built-ins, so a stream can define its own
	// {timer} without fighting the one here.
	const auto it = _vars.find(name);

	if (it != _vars.end()) {
		out = it->second;
		return true;
	}

	return builtin(name, out);
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

		const std::string name = text.substr(i + 1, close - i - 1);

		std::string value;

		if (!name.empty() && lookup(name, value)) {
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
