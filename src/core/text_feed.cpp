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

#include "text_feed.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace slugged {

namespace {

// Converts UTF-16 to UTF-8. Text sources on Windows are frequently written as
// UTF-16 by Notepad and by chat loggers, and rendering those bytes as UTF-8
// would produce a column of tofu.
std::string utf16ToUtf8(const std::string &bytes, bool bigEndian)
{
	std::string out;
	out.reserve(bytes.size());

	auto emit = [&out](uint32_t cp) {
		if (cp < 0x80) {
			out += char(cp);
		} else if (cp < 0x800) {
			out += char(0xC0 | (cp >> 6));
			out += char(0x80 | (cp & 0x3F));
		} else if (cp < 0x10000) {
			out += char(0xE0 | (cp >> 12));
			out += char(0x80 | ((cp >> 6) & 0x3F));
			out += char(0x80 | (cp & 0x3F));
		} else {
			out += char(0xF0 | (cp >> 18));
			out += char(0x80 | ((cp >> 12) & 0x3F));
			out += char(0x80 | ((cp >> 6) & 0x3F));
			out += char(0x80 | (cp & 0x3F));
		}
	};

	for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
		const uint8_t a = uint8_t(bytes[i]);
		const uint8_t b = uint8_t(bytes[i + 1]);
		uint32_t unit = bigEndian ? (uint32_t(a) << 8 | b) : (uint32_t(b) << 8 | a);

		// Surrogate pair.
		if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < bytes.size()) {
			const uint8_t c = uint8_t(bytes[i + 2]);
			const uint8_t d = uint8_t(bytes[i + 3]);
			const uint32_t low = bigEndian ? (uint32_t(c) << 8 | d) : (uint32_t(d) << 8 | c);

			if (low >= 0xDC00 && low <= 0xDFFF) {
				emit(0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
				i += 2;
				continue;
			}
		}

		emit(unit);
	}

	return out;
}

} // namespace

bool readTextFile(const std::string &path, std::string &out)
{
	std::ifstream file(path, std::ios::binary);

	if (!file.is_open())
		return false;

	std::ostringstream buffer;
	buffer << file.rdbuf();

	std::string bytes = buffer.str();

	if (bytes.size() >= 3 && uint8_t(bytes[0]) == 0xEF && uint8_t(bytes[1]) == 0xBB && uint8_t(bytes[2]) == 0xBF) {
		out = bytes.substr(3);
		return true;
	}

	if (bytes.size() >= 2 && uint8_t(bytes[0]) == 0xFF && uint8_t(bytes[1]) == 0xFE) {
		out = utf16ToUtf8(bytes.substr(2), false);
		return true;
	}

	if (bytes.size() >= 2 && uint8_t(bytes[0]) == 0xFE && uint8_t(bytes[1]) == 0xFF) {
		out = utf16ToUtf8(bytes.substr(2), true);
		return true;
	}

	out = std::move(bytes);

	return true;
}

std::string tailLines(const std::string &text, int lines)
{
	if (lines <= 0)
		return text;

	std::vector<std::string> all;
	std::string cur;

	for (char c : text) {
		if (c == '\n') {
			all.push_back(cur);
			cur.clear();
		} else if (c != '\r') {
			cur += c;
		}
	}

	if (!cur.empty())
		all.push_back(cur);

	// Trailing blank lines are dropped first, otherwise a log file that ends
	// with a newline pushes real content out of view.
	while (!all.empty() && all.back().empty())
		all.pop_back();

	const size_t keep = size_t(lines);
	const size_t start = all.size() > keep ? all.size() - keep : 0;

	std::string out;

	for (size_t i = start; i < all.size(); i++) {
		if (i > start)
			out += '\n';

		out += all[i];
	}

	return out;
}

bool TextFeed::reload(const Config &config)
{
	std::string raw;

	if (!readTextFile(config.path, raw)) {
		// A missing file blanks the source rather than keeping stale text:
		// showing a message that is no longer in the file would be worse.
		const bool changed = !_text.empty();

		_text.clear();

		return changed;
	}

	if (config.chatlog)
		raw = tailLines(raw, config.chatlogLines);

	if (raw == _text)
		return false;

	_text = std::move(raw);

	return true;
}

bool TextFeed::tick(const Config &config, float deltaSeconds)
{
	if (config.path.empty()) {
		const bool changed = !_text.empty();

		_text.clear();
		_lastPath.clear();
		_lastSize = -1;

		return changed;
	}

	if (config.path != _lastPath) {
		_lastPath = config.path;
		_dirty = true;
	}

	_accumulator += deltaSeconds;

	if (!_dirty && _accumulator < config.pollInterval)
		return false;

	_accumulator = 0.0f;

	// stat() rather than read(): a chat log being appended to every second
	// should not be fully re-read and re-laid-out unless it actually grew.
	std::error_code ec;
	const std::filesystem::path fsPath(config.path);

	const auto size = std::filesystem::file_size(fsPath, ec);
	const int64_t currentSize = ec ? -1 : int64_t(size);

	const auto mtime = std::filesystem::last_write_time(fsPath, ec);
	const int64_t currentModified = ec ? 0 : int64_t(mtime.time_since_epoch().count());

	if (!_dirty && currentSize == _lastSize && currentModified == _lastModified)
		return false;

	_lastSize = currentSize;
	_lastModified = currentModified;
	_dirty = false;

	return reload(config);
}

} // namespace slugged
