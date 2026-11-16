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

#include <algorithm>
#include <cctype>
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

TokenContext::Layer TokenContext::find(const std::string &name, std::string &value) const
{
	const std::string key = normaliseVariableName(name);

	const std::pair<const VariableMap *, Layer> tables[] = {{&_runtime, Layer::Runtime},
								{&_source, Layer::Source},
								{&_file, Layer::File}};

	for (const auto &[table, layer] : tables) {
		const auto it = table->find(key);

		if (it != table->end()) {
			value = it->second;
			return layer;
		}
	}

	if (GlobalVariables::lookup(key, value))
		return Layer::Global;

	return Layer::None;
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

std::vector<std::string> split(const std::string &text, char separator)
{
	std::vector<std::string> out;
	size_t start = 0;

	while (true) {
		const size_t at = text.find(separator, start);

		out.push_back(text.substr(start, at == std::string::npos ? std::string::npos : at - start));

		if (at == std::string::npos)
			break;

		start = at + 1;
	}

	return out;
}

std::string trimmed(const std::string &text)
{
	const size_t first = text.find_first_not_of(" \t\r\n");

	if (first == std::string::npos)
		return {};

	const size_t last = text.find_last_not_of(" \t\r\n");

	return text.substr(first, last - first + 1);
}

// The shortest plain rendering of a number: no exponent, no trailing zeros.
std::string formatPlain(double value, int maxDecimals = 6)
{
	if (!std::isfinite(value))
		return "0";

	char buf[64];
	std::snprintf(buf, sizeof(buf), "%.*f", maxDecimals, value);

	std::string out = buf;

	if (out.find('.') != std::string::npos) {
		while (!out.empty() && out.back() == '0')
			out.pop_back();

		if (!out.empty() && out.back() == '.')
			out.pop_back();
	}

	return out == "-0" ? "0" : out;
}

// Inserts `separator` between every three digits of the integer part.
std::string groupThousands(const std::string &number, const std::string &separator)
{
	size_t start = (!number.empty() && (number[0] == '-' || number[0] == '+')) ? 1 : 0;
	size_t end = number.find('.');

	if (end == std::string::npos)
		end = number.size();

	std::string out = number.substr(0, start);
	const std::string digits = number.substr(start, end - start);

	for (size_t i = 0; i < digits.size(); i++) {
		if (i > 0 && (digits.size() - i) % 3 == 0)
			out += separator;

		out += digits[i];
	}

	return out + number.substr(end);
}

// Number of UTF-8 code points in `text`.
size_t codePoints(const std::string &text)
{
	size_t count = 0;

	for (unsigned char c : text)
		if ((c & 0xC0) != 0x80)
			count++;

	return count;
}

// The first `count` code points of `text`.
std::string firstCodePoints(const std::string &text, size_t count)
{
	size_t seen = 0;

	for (size_t i = 0; i < text.size(); i++) {
		if ((uint8_t(text[i]) & 0xC0) != 0x80) {
			if (seen == count)
				return text.substr(0, i);

			seen++;
		}
	}

	return text;
}

std::string formatClock(double seconds, bool hours, bool padFirst)
{
	if (!(seconds > 0.0))
		seconds = 0.0;

	const long long total = (long long)std::floor(seconds);
	char buf[64];

	if (hours)
		std::snprintf(buf, sizeof(buf), padFirst ? "%02lld:%02lld:%02lld" : "%lld:%02lld:%02lld", total / 3600,
			      (total / 60) % 60, total % 60);
	else
		std::snprintf(buf, sizeof(buf), padFirst ? "%02lld:%02lld" : "%lld:%02lld", total / 60, total % 60);

	return buf;
}

} // namespace

std::string formatPlainNumber(double value)
{
	return formatPlain(value);
}

bool parseNumber(const std::string &text, double &out)
{
	std::string clean;

	for (char c : trimmed(text))
		if (c != ',' && c != '_')
			clean += c;

	if (clean.empty())
		return false;

	char *end = nullptr;
	out = std::strtod(clean.c_str(), &end);

	return end && *end == '\0' && std::isfinite(out);
}

std::string applyFilter(const std::string &value, const std::string &filter)
{
	const size_t colon = filter.find(':');
	const std::string name = trimmed(colon == std::string::npos ? filter : filter.substr(0, colon));
	const std::string arg = colon == std::string::npos ? std::string() : filter.substr(colon + 1);

	double number = 0.0;
	const bool numeric = parseNumber(value, number);

	if (name == "upper" || name == "lower") {
		std::string out = value;

		for (char &c : out)
			c = char(name == "upper" ? std::toupper((unsigned char)c) : std::tolower((unsigned char)c));

		return out;
	}

	if (name == "title") {
		std::string out = value;
		bool start = true;

		for (char &c : out) {
			const unsigned char u = (unsigned char)c;

			if (u < 0x80 && std::isalpha(u)) {
				c = char(start ? std::toupper(u) : std::tolower(u));
				start = false;
			} else if (u < 0x80) {
				start = std::isspace(u) || c == '-';
			}
		}

		return out;
	}

	if (name == "trim")
		return trimmed(value);

	if (name == "number" || name == "num")
		return numeric ? groupThousands(formatPlain(number), arg.empty() ? "," : arg) : value;

	if (name == "compact") {
		if (!numeric)
			return value;

		static const struct {
			double size;
			const char *suffix;
		} kUnits[] = {{1e12, "T"}, {1e9, "B"}, {1e6, "M"}, {1e3, "K"}};

		for (const auto &unit : kUnits) {
			if (std::fabs(number) >= unit.size) {
				const double scaled = number / unit.size;

				// One decimal under 100 (1.2K, 45.3K), none above (120K).
				return formatPlain(std::fabs(scaled) < 100.0 ? std::floor(scaled * 10.0) / 10.0
									     : std::floor(scaled),
						   1) +
				       unit.suffix;
			}
		}

		return formatPlain(number);
	}

	if (name == "round" || name == "fixed") {
		if (!numeric)
			return value;

		const int decimals = std::clamp(arg.empty() ? 0 : std::atoi(arg.c_str()), 0, 12);
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%.*f", decimals, number);

		return buf;
	}

	if (name == "pad") {
		const size_t width = size_t(std::clamp(std::atoi(arg.c_str()), 0, 64));

		if (numeric && value.find_first_not_of("+-0123456789") == std::string::npos) {
			const bool negative = !value.empty() && value[0] == '-';
			std::string digits =
				value.substr(!value.empty() && (value[0] == '-' || value[0] == '+') ? 1 : 0);

			while (digits.size() < width)
				digits.insert(digits.begin(), '0');

			return (negative ? "-" : "") + digits;
		}

		std::string out = value;

		while (codePoints(out) < width)
			out.insert(out.begin(), ' ');

		return out;
	}

	if (name == "add" || name == "sub" || name == "mul" || name == "div") {
		double operand = 0.0;

		if (!numeric || !parseNumber(arg, operand))
			return value;

		if (name == "add")
			number += operand;
		else if (name == "sub")
			number -= operand;
		else if (name == "mul")
			number *= operand;
		else if (operand != 0.0)
			number /= operand;

		return formatPlain(number);
	}

	if (name == "truncate") {
		const size_t limit = size_t(std::max(std::atoi(arg.c_str()), 1));

		if (codePoints(value) <= limit)
			return value;

		return firstCodePoints(value, limit - 1) + "\xE2\x80\xA6";
	}

	// {count} {count|plural:viewer,viewers}
	if (name == "plural") {
		const auto forms = split(arg, ',');
		const std::string one = forms.empty() ? std::string() : forms[0];
		const std::string many = forms.size() > 1 ? forms[1] : one + "s";

		return numeric && std::fabs(number) == 1.0 ? one : many;
	}

	// Durations: the value is seconds, or already a clock ("1:05").
	if (name == "mm" || name == "m" || name == "h" || name == "hh" || name == "seconds" || name == "minutes") {
		double seconds = numeric ? number : parseDuration(trimmed(value));

		if (seconds < 0.0)
			return value;

		if (name == "seconds")
			return formatPlain(std::floor(seconds));

		if (name == "minutes")
			return formatPlain(std::floor(seconds / 60.0));

		// The rest of the filter names the clock's shape: mm:ss, m:ss,
		// h:mm:ss, hh:mm:ss.
		const std::string shape = trimmed(filter);

		if (shape == "mm:ss")
			return formatClock(seconds, false, true);

		if (shape == "m:ss")
			return formatClock(seconds, false, false);

		if (shape == "h:mm:ss")
			return formatClock(seconds, true, false);

		if (shape == "hh:mm:ss")
			return formatClock(seconds, true, true);

		return value;
	}

	return value;
}

namespace {

// A number as it appears in text: where it is and how it is written.
struct NumberSpan {
	size_t start = 0;
	size_t end = 0;
	double value = 0.0;
	int decimals = 0;
	bool grouped = false;
};

bool isDigit(char c)
{
	return c >= '0' && c <= '9';
}

// Splits `text` into numbers and the words between them.
std::vector<NumberSpan> findNumbers(const std::string &text, std::vector<std::string> &between)
{
	std::vector<NumberSpan> out;
	size_t last = 0;
	size_t i = 0;

	while (i < text.size()) {
		const bool sign = text[i] == '-' && i + 1 < text.size() && isDigit(text[i + 1]) &&
				  (i == 0 || !isDigit(text[i - 1]));

		if (!isDigit(text[i]) && !sign) {
			i++;
			continue;
		}

		NumberSpan span;
		span.start = i;

		size_t j = sign ? i + 1 : i;

		// Digits, with a thousands separator only where exactly three digits
		// follow it ("1,234", but not "1,2" or "1,23456").
		while (j < text.size()) {
			if (isDigit(text[j])) {
				j++;
			} else if (text[j] == ',' && j > span.start && isDigit(text[j - 1]) && j + 3 < text.size() &&
				   isDigit(text[j + 1]) && isDigit(text[j + 2]) && isDigit(text[j + 3]) &&
				   (j + 4 >= text.size() || !isDigit(text[j + 4]))) {
				span.grouped = true;
				j += 4;
			} else {
				break;
			}
		}

		// Decimals.
		if (j + 1 < text.size() && text[j] == '.' && isDigit(text[j + 1])) {
			size_t k = j + 1;

			while (k < text.size() && isDigit(text[k]))
				k++;

			span.decimals = int(k - j - 1);
			j = k;
		}

		span.end = j;

		std::string digits;

		for (size_t k = span.start; k < span.end; k++)
			if (text[k] != ',')
				digits += text[k];

		span.value = std::strtod(digits.c_str(), nullptr);

		between.push_back(text.substr(last, span.start - last));
		out.push_back(span);

		last = j;
		i = j;
	}

	between.push_back(text.substr(last));

	return out;
}

} // namespace

bool interpolateNumbers(const std::string &from, const std::string &to, double t, std::string &out)
{
	std::vector<std::string> fromWords;
	std::vector<std::string> toWords;

	const std::vector<NumberSpan> a = findNumbers(from, fromWords);
	const std::vector<NumberSpan> b = findNumbers(to, toWords);

	if (a.empty() || a.size() != b.size() || fromWords != toWords)
		return false;

	t = std::clamp(t, 0.0, 1.0);
	out.clear();

	for (size_t i = 0; i < b.size(); i++) {
		out += toWords[i];

		if (a[i].value == b[i].value || t >= 1.0) {
			out += to.substr(b[i].start, b[i].end - b[i].start);
			continue;
		}

		double value = a[i].value + (b[i].value - a[i].value) * t;

		// Whole numbers count in whole steps, never overshooting.
		if (b[i].decimals == 0)
			value = b[i].value >= a[i].value ? std::floor(value) : std::ceil(value);

		char buf[64];
		std::snprintf(buf, sizeof(buf), "%.*f", b[i].decimals, value);

		std::string text = buf;

		if (text == "-0")
			text = "0";

		out += b[i].grouped ? groupThousands(text, ",") : text;
	}

	out += toWords.back();

	return true;
}

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

std::string TokenContext::expandImpl(const std::string &text, bool liveToo, std::vector<TokenSpan> *spans) const
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
			if (spans)
				spans->push_back(TokenSpan{uint32_t(i), uint32_t(i + 2), uint32_t(out.size()),
							   uint32_t(out.size() + 1)});

			out += '{';
			i += 2;
			continue;
		}

		const size_t close = text.find('}', i + 1);

		if (close == std::string::npos) {
			out += text[i++];
			continue;
		}

		const std::string whole = text.substr(i + 1, close - i - 1);

		// Filters follow the first bar; the token itself is what comes before.
		const size_t bar = whole.find('|');
		const std::string body = bar == std::string::npos ? whole : whole.substr(0, bar);

		// Everything past the first colon is the argument, so a format string
		// full of colons arrives intact.
		const size_t colon = body.find(':');

		const std::string name = colon == std::string::npos ? body : body.substr(0, colon);
		const std::string arg = colon == std::string::npos ? std::string() : body.substr(colon + 1);

		std::string value;
		Origin origin = name.empty() ? Origin::None : lookup(name, arg, value);

		if (bar != std::string::npos) {
			for (const std::string &filter : split(whole.substr(bar + 1), '|')) {
				const std::string trimmedFilter = trimmed(filter);

				// `default` stands in for a value that is missing or empty, and
				// is what the text says until one arrives.
				if (trimmedFilter.rfind("default", 0) == 0 &&
				    (trimmedFilter.size() == 7 || trimmedFilter[7] == ':')) {
					if (origin == Origin::None || value.empty()) {
						value = trimmedFilter.size() > 8 ? filter.substr(filter.find(':') + 1)
										 : std::string();

						if (origin == Origin::None)
							origin = Origin::Content;
					}

					continue;
				}

				if (origin != Origin::None)
					value = applyFilter(value, filter);
			}
		}

		if (origin == Origin::Content || (origin == Origin::Live && liveToo)) {
			if (spans)
				spans->push_back(TokenSpan{uint32_t(i), uint32_t(close + 1), uint32_t(out.size()),
							   uint32_t(out.size() + value.size())});

			out += value;
		} else
			out += text.substr(i, close - i + 1);

		i = close + 1;
	}

	return out;
}

std::string TokenContext::expand(const std::string &text) const
{
	return expandImpl(text, true, nullptr);
}

std::string TokenContext::expand(const std::string &text, std::vector<TokenSpan> &spans) const
{
	return expandImpl(text, true, &spans);
}

std::string TokenContext::expandContent(const std::string &text) const
{
	return expandImpl(text, false, nullptr);
}

} // namespace slugged
