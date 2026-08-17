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
#include <map>
#include <string>
#include <vector>

// Shared value types for the document model and the text pipeline.
//
// Nothing in core/ or text/ includes libobs or Qt: the whole pipeline from
// document to positioned glyphs is plain C++ so it can be unit-tested off-GPU
// and without an OBS instance. Conversion to and from obs_data_t lives in
// obs/settings.cpp, and GPU resources live in render/.

namespace slugged {

// Straight (non-premultiplied) RGBA in 0..1.
struct Rgba {
	float r = 1.0f;
	float g = 1.0f;
	float b = 1.0f;
	float a = 1.0f;

	// OBS stores colours as 0xAABBGGRR (obs_property_add_color).
	static Rgba fromObsColor(uint32_t abgr, float alphaScale = 1.0f)
	{
		return Rgba{
			float(abgr & 0xFF) / 255.0f,
			float((abgr >> 8) & 0xFF) / 255.0f,
			float((abgr >> 16) & 0xFF) / 255.0f,
			(float((abgr >> 24) & 0xFF) / 255.0f) * alphaScale,
		};
	}

	uint32_t toObsColor() const
	{
		auto q = [](float v) -> uint32_t {
			const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
			return uint32_t(c * 255.0f + 0.5f);
		};

		return q(r) | (q(g) << 8) | (q(b) << 16) | (q(a) << 24);
	}
};

struct GradientStop {
	float t = 0.0f;
	Rgba color;
};

struct Fill {
	enum class Type { Solid, Linear, Radial };

	Type type = Type::Solid;
	Rgba color;

	// Ramp for the gradient types, sorted by t. A gradient with fewer than two
	// stops degrades to Solid at render time rather than failing.
	std::vector<GradientStop> stops;

	// Linear: direction in degrees clockwise from +x. Radial: unused.
	float angleDeg = 90.0f;

	// When true the gradient spans the whole text block; when false it spans
	// each glyph's own em box, which is what makes per-letter ramps possible.
	bool perGlyph = false;
};

enum class LineJoin { Miter, Bevel, Round };
enum class LineCap { Butt, Round, Square };

struct Outline {
	bool enabled = false;

	// Stroke width in source pixels, measured at the run's own font size.
	float widthPx = 2.0f;

	Rgba color{0.0f, 0.0f, 0.0f, 1.0f};
	LineJoin join = LineJoin::Round;
	LineCap cap = LineCap::Round;
	float miterLimit = 4.0f;
};

struct Shadow {
	bool enabled = false;
	float offsetX = 2.0f;
	float offsetY = 2.0f;
	Rgba color{0.0f, 0.0f, 0.0f, 0.5f};
};

// A font request. `file` wins over `family` when both are set, so a scene
// collection can ship a .ttf next to itself and render identically on a machine
// where that font was never installed.
struct FontSpec {
	std::string family = "Sans Serif";
	std::string file;
	int faceIndex = 0;

	int weight = 400; // CSS-style 100..900
	bool italic = false;

	// Variable-font axis settings keyed by 4-character tag ("wght", "wdth",
	// "slnt", "opsz", ...). Applied via FT_Set_Var_Design_Coordinates, so these
	// also drive named instances of a variable family.
	std::map<std::string, float> axes;

	bool operator==(const FontSpec &o) const
	{
		return family == o.family && file == o.file && faceIndex == o.faceIndex && weight == o.weight &&
		       italic == o.italic && axes == o.axes;
	}
};

// Everything that can vary between two adjacent characters.
struct Style {
	FontSpec font;
	float sizePx = 64.0f;

	Fill fill;
	Outline outline;
	Shadow shadow;

	// Extra advance in source pixels, added after shaping so it never disturbs
	// kerning or ligature formation.
	float letterSpacing = 0.0f;
	float wordSpacing = 0.0f;

	// BCP-47 tag handed to HarfBuzz. Empty means "guess from the script".
	std::string language;

	bool underline = false;
	bool strikeout = false;
};

enum class HAlign { Left, Center, Right, Justify };
enum class VAlign { Top, Middle, Bottom };
enum class WrapMode { None, Word, Character };

enum class Direction { Auto, LTR, RTL };

// Must stay in sync with the MOTION_* defines in data/effects/slugged.effect.
enum class Motion { None = 0, Fade = 1, Slide = 2, Pop = 3, Typewriter = 4, Wave = 5 };

// How a motion preset is distributed across the text.
enum class MotionOrder { Together, PerGlyph, PerWord, PerLine };

struct MotionSpec {
	Motion motion = Motion::None;
	MotionOrder order = MotionOrder::PerGlyph;

	// Seconds for one glyph's transition, and the delay added per unit of
	// `order` so the effect sweeps through the text.
	float duration = 0.35f;
	float stagger = 0.03f;

	// Preset-specific: slide distance in px, pop start scale, wave amplitude.
	float param = 24.0f;

	// Restart the animation whenever the text content changes, rather than
	// letting it play once when the source is created.
	bool replayOnChange = true;
};

// Continuous whole-block movement, applied as a matrix translation rather than
// as per-glyph data, so it costs nothing to keep running.
struct ScrollSpec {
	bool enabled = false;
	float speedX = 0.0f; // px/second
	float speedY = 0.0f;

	// Wrap the block back around once it has fully left the layout box.
	bool loop = true;

	// Gap in px between the tail and the repeated head when looping.
	float gap = 64.0f;
};

} // namespace slugged
