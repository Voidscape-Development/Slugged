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

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace slugged {

void TokenContext::set(const std::string &name, const std::string &value)
{
	const std::string key = normaliseVariableName(name);

	if (!key.empty())
		_runtime[key] = value;
}

void TokenContext::erase(const std::string &name)
{
	_runtime.erase(normaliseVariableName(name));
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

std::string formatDuration(double seconds)
{
	if (!(seconds > 0.0))
		seconds = 0.0;

	const long long total = (long long)std::floor(seconds);
	const long long h = total / 3600;
	const long long m = (total / 60) % 60;
	const long long s = total % 60;

	char buf[48];

	if (h > 0)
		std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", h, m, s);
	else
		std::snprintf(buf, sizeof(buf), "%lld:%02lld", m, s);

	return buf;
}

double parseDuration(const std::string &text)
{
	if (text.empty())
		return -1.0;

	double total = 0.0;
	size_t start = 0;
	int parts = 0;

	while (start <= text.size()) {
		const size_t colon = text.find(':', start);
		const std::string part =
			text.substr(start, colon == std::string::npos ? std::string::npos : colon - start);

		if (part.empty() || part.find_first_not_of("0123456789.") != std::string::npos)
			return -1.0;

		total = total * 60.0 + std::strtod(part.c_str(), nullptr);
		parts++;

		if (colon == std::string::npos)
			break;

		start = colon + 1;
	}

	return parts <= 3 ? total : -1.0;
}

bool parseTarget(const std::string &text, std::time_t now, std::time_t &out)
{
	std::tm local{};

#if defined(_WIN32)
	localtime_s(&local, &now);
#else
	localtime_r(&now, &local);
#endif

	int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
	char tail = 0;

	if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d:%d%c", &year, &month, &day, &hour, &minute, &second, &tail) >=
		    5 &&
	    !tail) {
		std::tm target{};

		target.tm_year = year - 1900;
		target.tm_mon = month - 1;
		target.tm_mday = day;
		target.tm_hour = hour;
		target.tm_min = minute;
		target.tm_sec = second;
		target.tm_isdst = -1;

		out = std::mktime(&target);

		return out != std::time_t(-1);
	}

	hour = minute = second = 0;
	tail = 0;

	// A bare clock time means its next occurrence, so "20:00" works whether the
	// stream started before or after midnight.
	if (std::sscanf(text.c_str(), "%d:%d:%d%c", &hour, &minute, &second, &tail) >= 2 && !tail &&
	    text.find('-') == std::string::npos && hour >= 0 && hour < 24 && minute >= 0 && minute < 60 &&
	    second >= 0 && second < 60) {
		std::tm target = local;

		target.tm_hour = hour;
		target.tm_min = minute;
		target.tm_sec = second;
		target.tm_isdst = -1;

		std::time_t t = std::mktime(&target);

		if (t <= now)
			t += 24 * 3600;

		out = t;

		return true;
	}

	return false;
}

namespace {

std::string formatTime(const char *format, std::time_t now)
{
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

} // namespace

bool TokenContext::builtin(const std::string &name, const std::string &arg, std::string &out) const
{
	const std::time_t now = _clock ? _clock : std::time(nullptr);

	if (name == "strftime") {
		if (arg.empty())
			return false;

		out = formatTime(arg.c_str(), now);

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
			out = formatTime(format.format, now);
			return true;
		}
	}

	if (name == "uptime") {
		out = formatDuration(_uptime);
		return true;
	}

	if (name == "timer") {
		out = formatDuration(_timer);
		return true;
	}

	// {countdown:5:00} counts down from five minutes on the source's stopwatch,
	// so it starts, pauses and resets along with {timer}.
	if (name == "countdown") {
		const double duration = parseDuration(arg);

		if (duration < 0.0)
			return false;

		out = formatDuration(std::ceil(duration - double(_timer)));
		return true;
	}

	// {until:20:00} counts down to the next 8pm; {until:2026-12-31 20:00} to a
	// moment on the calendar.
	if (name == "until") {
		std::time_t target = 0;

		if (!parseTarget(arg, now, target))
			return false;

		out = formatDuration(std::difftime(target, now));
		return true;
	}

	return false;
}

TokenContext::Origin TokenContext::lookup(const std::string &name, const std::string &arg, std::string &out) const
{
	// A variable is matched on the whole token text, argument included.
	const std::string full = arg.empty() ? name : name + ":" + arg;

	for (const VariableMap *table : {&_runtime, &_source, &_file}) {
		const auto it = table->find(full);

		if (it != table->end()) {
			out = it->second;
			return Origin::Content;
		}
	}

	if (GlobalVariables::lookup(full, out))
		return Origin::Content;

	const auto host = _host.find(full);

	if (host != _host.end()) {
		out = host->second;
		return Origin::Live;
	}

	return builtin(name, arg, out) ? Origin::Live : Origin::None;
}

std::string TokenContext::expandImpl(const std::string &text, bool liveToo) const
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

		// Everything past the first colon is the argument, so a format string
		// full of colons arrives intact.
		const size_t colon = body.find(':');

		const std::string name = colon == std::string::npos ? body : body.substr(0, colon);
		const std::string arg = colon == std::string::npos ? std::string() : body.substr(colon + 1);

		std::string value;
		const Origin origin = name.empty() ? Origin::None : lookup(name, arg, value);

		if (origin == Origin::Content || (origin == Origin::Live && liveToo))
			out += value;
		else
			out += text.substr(i, close - i + 1);

		i = close + 1;
	}

	return out;
}

std::string TokenContext::expand(const std::string &text) const
{
	return expandImpl(text, true);
}

std::string TokenContext::expandContent(const std::string &text) const
{
	return expandImpl(text, false);
}

} // namespace slugged
