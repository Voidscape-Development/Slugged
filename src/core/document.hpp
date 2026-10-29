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
#include <functional>
#include <string>
#include <vector>

namespace slugged {

// A span of text sharing one set of style overrides. Text is UTF-8 throughout.
struct Run {
	std::string text;
	StyleOverride style;

	bool operator==(const Run &) const = default;
};

// A paragraph. Paragraphs break lines independently and stack vertically.
struct Block {
	std::vector<Run> runs;
	ParagraphOverride paragraph;

	std::string plainText() const;
	size_t size() const;

	bool operator==(const Block &) const = default;
};

// How the source decides its own width and height.
enum class SizeMode {
	// Grow to fit the text, its outline and its shadow (the GDI+ default).
	Auto,
	// Fixed box; text wraps inside it and is clipped to it.
	Fixed,
};
SLUGGED_ENUM_TRAITS(SizeMode, {SizeMode::Auto, "auto"}, {SizeMode::Fixed, "fixed"})

struct Background {
	bool enabled = false;
	Rgba color{0.0f, 0.0f, 0.0f, 0.5f};
	float radius = 0.0f;

	bool operator==(const Background &) const = default;
};

// The full description of what a source draws.
//
// Offsets into a document are UTF-8 byte offsets into plainText(), where each
// paragraph break counts as one byte ('\n').
struct Document {
	// The style every character starts from. Runs only record where they differ.
	Style base;
	ParagraphStyle paragraph;

	std::vector<Block> blocks;

	SizeMode sizeMode = SizeMode::Auto;

	// Only meaningful when sizeMode == Fixed.
	float boxWidth = 640.0f;
	float boxHeight = 200.0f;

	WrapMode wrap = WrapMode::Word;
	VAlign valign = VAlign::Top;

	// Inner padding in px on every side.
	float padding = 0.0f;

	Background background;
	MotionSpec motion;
	ScrollSpec scroll;

	// Whole-source opacity, applied once in the pixel shader.
	float opacity = 1.0f;

	bool operator==(const Document &) const = default;

	// ---- text ------------------------------------------------------------

	// Paragraphs joined by '\n'. This is what the flat `text` property mirrors.
	std::string plainText() const;
	size_t size() const;
	bool empty() const;

	// Replaces [start, end) with `text`, which may contain newlines. Inserted
	// characters take the style of the character before them (or after, at the
	// start of a paragraph), so typing extends whatever the cursor sits in.
	void replaceText(size_t start, size_t end, const std::string &text);

	// Replaces the whole text with the minimal edit that produces `text`, so the
	// styling of every character that did not change survives. This is the path
	// a script or obs-websocket takes when it sets `text`.
	void setPlainText(const std::string &text);

	// Rewrites every run's text through `expand`, leaving the run structure --
	// and therefore every per-character style -- exactly as it was. Returns true
	// when any run changed. A token has to sit inside one run to be recognised.
	bool expandTokens(const std::function<std::string(const std::string &)> &expand);

	// ---- styling ---------------------------------------------------------

	// Sets the fields of `delta` on every character in [start, end).
	void applyStyle(size_t start, size_t end, const StyleOverride &delta);

	// Sets the fields of `delta` on the base style and removes them from every
	// run, so the whole text takes them.
	void applyBaseStyle(const StyleOverride &delta);

	// Removes all of the overrides in [start, end).
	void clearStyle(size_t start, size_t end);

	// The fully resolved style of the character at `offset`, or of the
	// character before it when `offset` is at a run's end ("the style typing
	// would continue").
	Style styleAt(size_t offset) const;

	// Paragraph settings for every paragraph [start, end) touches.
	void applyParagraph(size_t start, size_t end, const ParagraphOverride &delta);
	void applyBaseParagraph(const ParagraphOverride &delta);
	ParagraphStyle paragraphAt(size_t offset) const;

	// Merges adjacent runs with identical overrides and drops empty runs.
	void normalize();

	// The fully resolved style of a run.
	Style resolve(const Run &run) const { return run.style.resolve(base); }
	ParagraphStyle resolve(const Block &block) const { return block.paragraph.resolve(paragraph); }
};

// Splits `text` on '\n' (dropping '\r'), returning at least one piece.
std::vector<std::string> splitLines(const std::string &text);

// Moves `offset` back to the start of the UTF-8 sequence it points into.
size_t utf8Floor(const std::string &text, size_t offset);

} // namespace slugged
