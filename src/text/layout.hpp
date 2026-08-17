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
#include <vector>

namespace slugged {

struct FontFace;

// A glyph placed in the source's own pixel space.
//
// Coordinates are y-down with the origin at the layout box's top-left, matching
// OBS's source space. `y` is the baseline, not the top of the glyph.
struct PositionedGlyph {
	FontFace *face = nullptr;
	uint32_t glyphIndex = 0;

	float x = 0.0f;
	float y = 0.0f;

	// Em size in pixels. Slug shapes are em-normalised, so this is the only
	// scale factor the geometry builder needs.
	float sizePx = 0.0f;

	const Style *style = nullptr;

	uint32_t lineIndex = 0;
	uint32_t wordIndex = 0;

	// Position of this glyph in the document's visible sequence. Drives
	// per-glyph animation staggering.
	uint32_t ordinal = 0;

	bool rtl = false;
};

struct LineInfo {
	float baselineY = 0.0f;
	float width = 0.0f;
	float ascent = 0.0f;
	float descent = 0.0f;

	size_t firstGlyph = 0;
	size_t glyphCount = 0;
};

struct LayoutResult {
	std::vector<PositionedGlyph> glyphs;
	std::vector<LineInfo> lines;

	// Tight bounds of the laid-out text, excluding padding.
	float width = 0.0f;
	float height = 0.0f;

	// Number of distinct words, for MotionOrder::PerWord staggering.
	uint32_t wordCount = 0;
};

class Layout {
public:
	// Lays `doc` out. `availableWidth` is the wrapping limit in pixels; pass 0
	// for unlimited, which is what SizeMode::Auto without an explicit box does.
	static LayoutResult run(const Document &doc, float availableWidth);
};

} // namespace slugged
