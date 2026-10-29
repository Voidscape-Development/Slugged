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

#include "../core/document.hpp"
#include "shaper.hpp"

#include <cstdint>
#include <deque>
#include <vector>

namespace slugged {

struct FontInstance;

// A glyph placed in the layout box's pixel space: y-down, origin at the box's
// top-left, `y` on the baseline.
struct PositionedGlyph {
	FontInstance *font = nullptr;
	uint32_t glyphIndex = 0;

	float x = 0.0f;
	float y = 0.0f;

	// Em size in pixels; the only scale the geometry builder needs.
	float sizePx = 0.0f;

	const Style *style = nullptr;

	uint32_t lineIndex = 0;
	uint32_t wordIndex = 0;

	// Position in reading order among visible glyphs. Drives per-character
	// animation staggering.
	uint32_t ordinal = 0;

	// Byte offset of this glyph's cluster in the document's plain text, which
	// is how a text change finds the characters that actually changed.
	uint32_t textOffset = 0;
};

// An underline or strikeout bar, in the same space as PositionedGlyph. A run of
// decorated text produces one continuous bar, spaces included.
struct DecorationRect {
	float x = 0.0f;
	float y = 0.0f; // top edge
	float width = 0.0f;
	float height = 0.0f;

	const Style *style = nullptr;

	// Copied from the first glyph the bar covers, so it animates with its text.
	uint32_t lineIndex = 0;
	uint32_t wordIndex = 0;
	uint32_t ordinal = 0;
	uint32_t textOffset = 0;
};

struct LineInfo {
	float baselineY = 0.0f;
	float x = 0.0f;
	float width = 0.0f;
	float ascent = 0.0f;
	float descent = 0.0f;
};

struct LayoutResult {
	// Resolved styles that glyphs point into. A deque, so pointers stay valid as
	// it grows.
	std::deque<Style> styles;

	std::vector<PositionedGlyph> glyphs;
	std::vector<LineInfo> lines;
	std::vector<DecorationRect> decorations;

	// The box the text was laid out in: the fixed box's inner size, or the
	// text's own extent for an auto-sized source.
	float width = 0.0f;
	float height = 0.0f;

	uint32_t wordCount = 0;
};

class Layout {
public:
	// Lays `doc` out. `availableWidth` is the wrapping limit in pixels, or 0 for
	// none. `availableHeight` positions text vertically within a fixed box, or 0
	// for an auto-sized one. Must be called with the FontManager lock held.
	static LayoutResult run(const Document &doc, float availableWidth, float availableHeight);
};

} // namespace slugged
