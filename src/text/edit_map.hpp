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

#include "../core/tokens.hpp"
#include "layout.hpp"

#include <cstdint>
#include <vector>

namespace slugged {

// Where the text sits on screen, for editing it there.
//
// A layout knows where glyphs go; an editor needs the reverse as well: which
// character a click landed on, where the caret for an offset belongs, which
// boxes a selection covers. This is that, in the source's own pixel space and
// in offsets into the text *as written* -- so a live value on the canvas is one
// unit standing for its whole "{token}", however many characters it shows.
//
// Built on the rebuild worker alongside the frame it describes; immutable after.
struct EditMap {
	struct Rect {
		float x = 0.0f;
		float y = 0.0f;
		float width = 0.0f;
		float height = 0.0f;

		bool operator==(const Rect &) const = default;
	};

	struct Cell {
		float x0 = 0.0f;
		float x1 = 0.0f;
		uint32_t start = 0;
		uint32_t end = 0;
		bool rtl = false;

		// True when this cell is a replaced token rather than typed text.
		bool token = false;
	};

	struct Line {
		float top = 0.0f;
		float bottom = 0.0f;

		// Where an empty line's caret sits, and the line's ends otherwise.
		float x0 = 0.0f;
		float x1 = 0.0f;

		uint32_t start = 0;
		uint32_t end = 0;
		bool rtl = false;

		// In visual order, left to right.
		std::vector<Cell> cells;
	};

	std::vector<Line> lines;

	// Size of the text as written, so a reader can tell a map built for an
	// older version of the text from a current one.
	uint32_t textSize = 0;

	// `spans` must be sorted by position, as Document::expandTokens produces
	// them. (dx, dy) moves layout space into the source's.
	static EditMap build(const LayoutResult &layout, const std::vector<TokenSpan> &spans, float dx, float dy,
			     uint32_t textSize);

	bool empty() const { return lines.empty(); }

	// A zero-width rectangle where the caret for `offset` belongs.
	Rect caret(uint32_t offset) const;

	// The offset a click at (x, y) means. Points outside the text resolve to
	// the nearest line and the nearest end of it.
	uint32_t hitTest(float x, float y) const;

	// The offset one line above or below `offset`, keeping its x position.
	uint32_t vertical(uint32_t offset, int lines) const;

	// The boxes covering [start, end), one or more per line.
	std::vector<Rect> selection(uint32_t start, uint32_t end) const;

	// The boxes of every replaced token, for marking live values.
	std::vector<Rect> tokens() const;

	// The whole token `offset` falls strictly inside, if any, as [start, end).
	bool tokenAt(uint32_t offset, uint32_t &start, uint32_t &end) const;

private:
	size_t lineOf(uint32_t offset) const;
};

} // namespace slugged
