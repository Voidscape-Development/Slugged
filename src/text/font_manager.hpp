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
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H

struct hb_font_t;

namespace slugged {

// One resolved, opened font face.
//
// `slot` is the piece that matters to the renderer: slughorn's Key packs a
// 21-bit value plus an 8-bit caller-defined mask, so a face's slot becomes that
// mask and glyph indices become the value. Two faces can then contribute the
// same glyph index to one atlas without aliasing. 8 bits caps a single atlas at
// 256 distinct faces, which is far past what a text source realistically mixes;
// past that, allocation fails loudly rather than silently colliding.
struct FontFace {
	FT_Face ft = nullptr;
	hb_font_t *hb = nullptr;

	uint8_t slot = 0;

	std::string path;
	int faceIndex = 0;

	std::string family;
	std::string style;

	// HarfBuzz's positional scale for this face: the number of shaping units
	// that make up one em. Shaped advances are divided by this to reach em
	// space.
	float hbScaleX = 1000.0f;
	float hbScaleY = 1000.0f;

	// Ratios of the em square, from the face's own metrics.
	float unitsPerEm = 1000.0f;
	float ascender = 0.8f;
	float descender = 0.2f;
	float lineGap = 0.0f;
	float underlinePos = -0.1f;
	float underlineThickness = 0.05f;

	// True when the face carries COLR/CPAL colour layers.
	bool hasColor = false;

	// True when the face exposes variation axes.
	bool variable = false;

	float defaultLineHeight() const { return ascender + descender + lineGap; }

	~FontFace();

	FontFace() = default;
	FontFace(const FontFace &) = delete;
	FontFace &operator=(const FontFace &) = delete;
};

// A variation axis exposed by a variable font, in the font's own units.
struct FontAxis {
	std::string tag; // 4 chars, e.g. "wght"
	std::string name;
	float minValue = 0.0f;
	float maxValue = 0.0f;
	float defaultValue = 0.0f;
};

// Owns the FreeType library and every opened face. One instance is shared by
// all sources so two sources using the same font share one FT_Face and one
// atlas slot.
class FontManager {
public:
	static FontManager &instance();

	FontManager();
	~FontManager();

	// Resolves a FontSpec to an open face, opening and caching as needed.
	// Returns nullptr only when neither the requested font nor any fallback
	// could be opened at all.
	FontFace *acquire(const FontSpec &spec);

	// The face used when `spec` has no glyph for `codepoint`. Walks the
	// platform's fallback list, then any face already open, and returns nullptr
	// when nothing covers the codepoint.
	FontFace *fallbackFor(const FontSpec &spec, uint32_t codepoint);

	// Variation axes of the face `spec` resolves to. Empty for static fonts.
	std::vector<FontAxis> axesOf(const FontSpec &spec);

	// Clears every cached face. Only safe when no layout is in flight; the
	// editor calls it after the user installs a font mid-session.
	void reset();

private:
	FontFace *openFace(const std::string &path, int faceIndex, const FontSpec &spec);
	void applyVariations(FontFace &face, const FontSpec &spec);

	FT_Library _ft = nullptr;

	std::mutex _mutex;
	std::vector<std::unique_ptr<FontFace>> _faces;

	// Cache key is the full spec including axis settings: two different weight
	// axis values on one variable file are genuinely different faces to
	// FreeType, since the variation is applied to the FT_Face itself.
	struct Entry {
		FontSpec spec;
		FontFace *face;
	};

	std::vector<Entry> _cache;

	uint8_t _nextSlot = 0;
};

} // namespace slugged
