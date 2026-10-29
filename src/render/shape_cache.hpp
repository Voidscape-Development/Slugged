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
#include <tuple>
#include <vector>

#include "slughorn/slughorn.hpp"

namespace slugged {

struct FontInstance;

// How one layer of a colour glyph is painted.
struct LayerPaint {
	enum class Kind {
		Solid,
		// Palette index 0xFFFF: the text's own fill colour.
		Foreground,
		Linear,
		Radial,
	};

	Kind kind = Kind::Solid;

	Rgba color;
	Rgba color2;

	// Em space, y-up. Linear runs from (x0, y0) to (x1, y1); radial is centred
	// on (x0, y0) with radius r.
	float x0 = 0.0f;
	float y0 = 0.0f;
	float x1 = 1.0f;
	float y1 = 0.0f;
	float r = 1.0f;
};

struct ColorLayer {
	slughorn::Atlas::ShapeInfo shape;
	LayerPaint paint;
};

// A colour bitmap glyph (CBDT or sbix), as straight-alpha RGBA.
struct BitmapGlyph {
	int width = 0;
	int height = 0;
	std::vector<uint8_t> rgba;

	// Placement in em space, y-up: left edge and top edge, and size.
	float left = 0.0f;
	float top = 0.0f;
	float emWidth = 0.0f;
	float emHeight = 0.0f;
};

// Everything one glyph can be drawn from.
struct GlyphShapes {
	// The filled outline, in em space. Empty curves for a blank glyph.
	slughorn::Atlas::ShapeInfo fill;

	// COLR layers, bottom first. Empty for a monochrome glyph.
	std::vector<ColorLayer> layers;

	// Set for a glyph that only exists as a colour bitmap.
	std::unique_ptr<BitmapGlyph> bitmap;

	bool hasFill() const { return !fill.curves.empty(); }
	bool isColor() const { return !layers.empty() || bitmap != nullptr; }
};

// Decomposed and stroked glyph geometry, shared by every source.
//
// Decomposing an outline and especially stroking it is the expensive part of a
// rebuild, and the same glyphs appear in many sources and in every rebuild of
// one. Entries are keyed by font instance id, which is never reused, so an entry
// can never describe a different font than the one that asked for it.
//
// Only used from the rebuild worker, with the FontManager lock held.
class ShapeCache {
public:
	static ShapeCache &instance();

	const GlyphShapes &glyph(FontInstance *font, uint32_t glyphIndex);

	// The outline ring for a glyph: a stroke `widthEm` wide (in total) centred
	// on the glyph's edge. Returns nullptr for a blank glyph.
	const slughorn::Atlas::ShapeInfo *stroke(FontInstance *font, uint32_t glyphIndex, float widthEm, LineJoin join);

	// Stroke widths are bucketed so dragging the outline slider does not
	// produce a distinct shape per pixel of travel.
	static float bucketWidth(float widthEm);

	// Drops everything built from the given font instances.
	void evict(const std::vector<uint32_t> &instanceIds);

	void clear();

private:
	using GlyphKey = std::pair<uint32_t, uint32_t>;
	using StrokeKey = std::tuple<uint32_t, uint32_t, int, int>;

	void trim();

	std::map<GlyphKey, GlyphShapes> _glyphs;
	std::map<StrokeKey, std::unique_ptr<slughorn::Atlas::ShapeInfo>> _strokes;
};

} // namespace slugged
