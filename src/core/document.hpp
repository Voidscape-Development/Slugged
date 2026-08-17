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

#include "types.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace slugged {

// A maximal span of text sharing one Style. Text is UTF-8 throughout.
struct Run {
	std::string text;
	Style style;
};

// A paragraph. Blocks break lines independently and stack vertically.
struct Block {
	std::vector<Run> runs;

	HAlign align = HAlign::Left;
	Direction direction = Direction::Auto;

	// Multiple of the font's own line height; 1.0 uses the font's metrics
	// unchanged.
	float lineHeight = 1.0f;

	float spaceBefore = 0.0f;
	float spaceAfter = 0.0f;

	std::string plainText() const
	{
		std::string out;

		for (const Run &r : runs)
			out += r.text;

		return out;
	}
};

// How the source decides its own width and height.
enum class SizeMode {
	// Grow to fit the text exactly (the GDI+ default).
	Auto,
	// Fixed box; text wraps inside it and may be clipped.
	Fixed,
};

struct Document {
	std::vector<Block> blocks;

	SizeMode sizeMode = SizeMode::Auto;

	// Only meaningful when sizeMode == Fixed.
	float boxWidth = 640.0f;
	float boxHeight = 200.0f;

	WrapMode wrap = WrapMode::Word;
	VAlign valign = VAlign::Top;

	// Inner padding in px, applied on every side. Keeps outlines and shadows
	// from being clipped by the source's own bounds in Auto mode.
	float padding = 0.0f;

	// Background fill behind the whole box, drawn as a plain quad.
	bool backgroundEnabled = false;
	Rgba backgroundColor{0.0f, 0.0f, 0.0f, 0.5f};
	float backgroundRadius = 0.0f;

	MotionSpec motion;
	ScrollSpec scroll;

	// Global opacity multiplier, applied in the pixel shader.
	float opacity = 1.0f;

	// ---- convenience -----------------------------------------------------

	// The document's text with block breaks as newlines. This is what the flat
	// `text` source property mirrors, so obs-websocket and scripts can read and
	// write a Slugged source exactly like a GDI+ one.
	std::string plainText() const
	{
		std::string out;

		for (size_t i = 0; i < blocks.size(); i++) {
			if (i)
				out += '\n';

			out += blocks[i].plainText();
		}

		return out;
	}

	// Replaces all content with `text`, splitting on newlines, while preserving
	// the styling of the first run of each existing block where one exists.
	// This is the path a script or obs-websocket takes when it sets `text`: the
	// look the user built in the editor survives, only the words change.
	void setPlainText(const std::string &text);

	// The style new content inherits when the document has no runs at all.
	Style defaultStyle;

	// True when there is nothing to render.
	bool empty() const
	{
		for (const Block &b : blocks)
			for (const Run &r : b.runs)
				if (!r.text.empty())
					return true;

		return false;
	}

	// Every distinct style in document order; used by the atlas cache to decide
	// which faces to load before layout runs.
	std::vector<const Style *> styles() const
	{
		std::vector<const Style *> out;

		for (const Block &b : blocks)
			for (const Run &r : b.runs)
				out.push_back(&r.style);

		return out;
	}
};

// Splits UTF-8 `text` on '\n', returning at least one (possibly empty) piece.
std::vector<std::string> splitLines(const std::string &text);

} // namespace slugged
