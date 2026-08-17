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
#include <string>
#include <vector>

namespace slugged {

struct FontFace;

// One shaped glyph, still in em units (advances divided by unitsPerEm) so the
// caller can scale to any pixel size without reshaping.
struct ShapedGlyph {
	FontFace *face = nullptr;
	uint32_t glyphIndex = 0;

	// Advance and offsets as fractions of the em square.
	float advance = 0.0f;
	float offsetX = 0.0f;
	float offsetY = 0.0f;

	// Byte offset into the original run text that produced this glyph. Several
	// glyphs can share one cluster (and one glyph can span several bytes), which
	// is what makes caret placement and per-character effects correct for
	// ligatures and combining marks.
	uint32_t cluster = 0;

	// Index of the run this glyph came from.
	uint32_t runIndex = 0;

	// True when this glyph is a space or other breakable whitespace, which
	// line breaking and justification both need to know.
	bool whitespace = false;

	// Right-to-left visual direction for this glyph's item.
	bool rtl = false;
};

// A run of text paired with the style it should be shaped with.
struct ShapeRun {
	std::string text;
	const Style *style = nullptr;
	uint32_t runIndex = 0;
};

// Shapes a paragraph's runs into a single visually-ordered glyph sequence.
//
// The pipeline is the standard one: resolve the paragraph's base direction,
// split into directional levels with FriBidi, split those by script and by
// which font actually covers each character, shape each resulting item with
// HarfBuzz, then reorder the items into visual order. Skipping any stage
// produces text that looks right in English and wrong everywhere else, which is
// why this exists rather than a simple cmap-and-advance loop.
class Shaper {
public:
	// `baseDirection` of Auto derives the paragraph direction from its first
	// strong character, per the Unicode bidi algorithm.
	static std::vector<ShapedGlyph> shape(const std::vector<ShapeRun> &runs, Direction baseDirection);
};

// Decodes one UTF-8 codepoint at `pos`, advancing it. Returns U+FFFD and
// advances by one byte on malformed input, so bad input never stalls a loop.
uint32_t utf8Next(const std::string &text, size_t &pos);

// Number of codepoints in a UTF-8 string.
size_t utf8Length(const std::string &text);

} // namespace slugged
