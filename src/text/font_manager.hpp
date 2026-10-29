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

#include "../core/types.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H

struct hb_face_t;
struct hb_font_t;

namespace slugged {

// One opened font file (or one face of a collection). Shared by every instance
// of it, whatever its variation settings.
struct FontFile {
	FT_Face ft = nullptr;
	hb_face_t *hbFace = nullptr;

	std::string path;
	int faceIndex = 0;

	std::string family;
	std::string style;

	float unitsPerEm = 1000.0f;

	// Ratios of the em square.
	float ascender = 0.8f;
	float descender = 0.2f;
	float lineGap = 0.0f;
	float underlinePos = -0.1f;
	float underlineThickness = 0.05f;
	float strikeoutPos = 0.25f;
	float strikeoutThickness = 0.05f;

	// Colour layers (COLR/CPAL) and colour bitmaps (CBDT/sbix).
	bool hasColorLayers = false;
	bool hasColorBitmaps = false;

	bool variable = false;

	float defaultLineHeight() const { return ascender + descender + lineGap; }

	FontFile() = default;
	FontFile(const FontFile &) = delete;
	FontFile &operator=(const FontFile &) = delete;
	~FontFile();
};

// A font file at one set of variation coordinates. This is what shaping and
// outline extraction work with.
//
// `id` is unique for the life of the process and never reused, so caches keyed
// on it can never confuse a closed instance with a newer one.
struct FontInstance {
	FontFile *file = nullptr;
	hb_font_t *hb = nullptr;

	uint32_t id = 0;

	// Design coordinates in FreeType's 16.16 format, one per axis. Empty for a
	// static font.
	std::vector<FT_Fixed> coords;

	// Puts this instance's coordinates on the shared FT_Face before an outline
	// is loaded. Requires the manager's lock.
	void select() const;

	FontInstance() = default;
	FontInstance(const FontInstance &) = delete;
	FontInstance &operator=(const FontInstance &) = delete;
	~FontInstance();
};

// A variation axis exposed by a variable font, in the font's own units.
struct FontAxis {
	std::string tag; // 4 chars, e.g. "wght"
	std::string name;
	float minValue = 0.0f;
	float maxValue = 0.0f;
	float defaultValue = 0.0f;
};

// Owns the FreeType library and every opened font. One instance is shared by all
// sources.
//
// FreeType faces are not thread-safe, so everything that touches one happens
// under lock(). The source pipeline runs on a single worker thread that holds the
// lock for a whole rebuild; the editor takes it briefly to read axis ranges.
class FontManager {
public:
	static FontManager &instance();

	FontManager();
	~FontManager();

	std::unique_lock<std::recursive_mutex> lock() { return std::unique_lock<std::recursive_mutex>(_mutex); }

	// Resolves a FontSpec to an instance, opening and caching as needed. Returns
	// nullptr only when neither the requested font nor the platform default
	// could be opened.
	FontInstance *acquire(const FontSpec &spec);

	// The instance that should render `codepoint` when `spec` lacks it. The
	// answer depends only on the request, never on what else happens to be
	// open, so a source renders the same whatever other sources loaded.
	// `preferColor` picks a colour font over a monochrome one that also covers
	// the character, which is what an emoji should look like.
	FontInstance *fallbackFor(const FontSpec &spec, uint32_t codepoint, bool preferColor = false);

	// Variation axes of the font `spec` resolves to. Empty for static fonts.
	std::vector<FontAxis> axesOf(const FontSpec &spec);

	// True for characters that are emoji by default rather than only when
	// followed by U+FE0F.
	static bool isEmoji(uint32_t codepoint);

	// Marks the start of a rebuild. Instances unused for a long while are closed
	// here, and their ids returned so caches built on them can drop them too.
	// Only call with no layout in flight.
	std::vector<uint32_t> collect();

	// Forgets every "font not found" answer, so a font installed while OBS is
	// running is picked up.
	void refresh();

	// Closes everything. Only at module unload.
	void reset();

private:
	FontFile *openFile(const std::string &path, int faceIndex);
	FontInstance *instanceFor(FontFile *file, const FontSpec &spec);

	FT_Library _ft = nullptr;

	std::recursive_mutex _mutex;

	std::vector<std::unique_ptr<FontFile>> _files;
	std::vector<std::unique_ptr<FontInstance>> _instances;

	struct SpecEntry {
		FontSpec spec;
		FontInstance *instance = nullptr;
		bool missing = false;
	};

	std::vector<SpecEntry> _specs;

	// (codepoint, family, weight, italic) -> chosen fallback file, or none.
	using FallbackKey = std::tuple<uint32_t, std::string, int, bool, bool>;
	std::map<FallbackKey, std::pair<std::string, int>> _fallbacks;

	// Last rebuild each instance was used in, for collect().
	std::map<uint32_t, uint64_t> _lastUsed;
	uint64_t _rebuild = 0;
	uint32_t _nextId = 1;
};

} // namespace slugged
