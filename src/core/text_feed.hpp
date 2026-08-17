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
#include <string>

namespace slugged {

// Where a source's text comes from.
enum class TextSourceMode {
	// The document authored in the editor.
	Document,
	// A file on disk, re-read when it changes.
	File,
};

// Reads text from a file and reports when it changes.
//
// This is the GDI+ source's "read from file" plus its chatlog mode, which are
// what most existing text sources in the wild actually use. Polling is by
// modification time and size rather than by content hash: it is cheap enough to
// run every frame and catches the append-only writes that chat loggers produce.
class TextFeed {
public:
	struct Config {
		std::string path;

		// Chatlog mode: show only the last `chatlogLines` lines. Matches the
		// GDI+ behaviour streamers rely on for chat overlays.
		bool chatlog = false;
		int chatlogLines = 6;

		// Re-read cadence in seconds. The file is stat()ed this often, not
		// read, so a fast cadence is inexpensive.
		float pollInterval = 0.25f;
	};

	// Advances the poll timer and re-reads if the file changed. Returns true
	// when `text()` differs from the previous call.
	bool tick(const Config &config, float deltaSeconds);

	const std::string &text() const { return _text; }

	// Forces a re-read on the next tick, e.g. after the path changes.
	void invalidate() { _dirty = true; }

private:
	bool reload(const Config &config);

	std::string _text;
	std::string _lastPath;

	int64_t _lastModified = 0;
	int64_t _lastSize = -1;

	float _accumulator = 0.0f;
	bool _dirty = true;
};

// Reads a whole file as UTF-8, converting UTF-16 with a BOM and stripping a
// UTF-8 BOM. Returns false when the file cannot be opened.
bool readTextFile(const std::string &path, std::string &out);

// Keeps only the last `lines` lines of `text`.
std::string tailLines(const std::string &text, int lines);

} // namespace slugged
