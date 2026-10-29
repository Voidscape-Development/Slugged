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

struct FontInstance;

// One shaped glyph, in em units so the caller can scale to any size.
struct ShapedGlyph {
	FontInstance *font = nullptr;
	uint32_t glyphIndex = 0;

	// Advance and offsets as fractions of the em square.
	float advance = 0.0f;
	float offsetX = 0.0f;
	float offsetY = 0.0f;

	// Byte offset into the paragraph's text of the cluster this glyph belongs
	// to. Several glyphs can share a cluster (marks, ligature components).
	uint32_t cluster = 0;

	// Index of the run this glyph came from.
	uint32_t runIndex = 0;

	// Resolved bidi embedding level; odd is right-to-left.
	uint8_t level = 0;

	// True when this glyph is breakable whitespace.
	bool whitespace = false;

	// True for the first glyph of its cluster, which is the only place a line
	// may break.
	bool clusterStart = false;
};

// A run of text paired with the style it should be shaped with.
struct ShapeRun {
	std::string text;
	const Style *style = nullptr;
	uint32_t runIndex = 0;
};

struct ShapedParagraph {
	// Glyphs in *logical* order: the order of the text, not of the screen.
	// Lines are broken in this order and each line is reordered for display
	// afterwards, which is what the Unicode bidi algorithm requires.
	std::vector<ShapedGlyph> glyphs;

	// The concatenated text of every run.
	std::string text;

	// Paragraph embedding level: 0 for left-to-right, 1 for right-to-left.
	uint8_t baseLevel = 0;
};

// Shapes a paragraph: resolve bidi levels with SheenBidi, split into items by
// run, level, script and covering font, and shape each item with HarfBuzz.
class Shaper {
public:
	static ShapedParagraph shape(const std::vector<ShapeRun> &runs, Direction baseDirection);
};

// Reorders a line's glyph indices from logical to visual order (UAX #9, rule L2).
void reorderLine(const std::vector<uint8_t> &levels, std::vector<size_t> &order);

// Decodes one UTF-8 codepoint at `pos`, advancing it. Returns U+FFFD and
// advances by one byte on malformed input.
uint32_t utf8Next(const std::string &text, size_t &pos);

size_t utf8Length(const std::string &text);

} // namespace slugged
