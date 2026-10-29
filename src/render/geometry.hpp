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
#include "../text/layout.hpp"
#include "atlas_cache.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace slugged {

// Vertex data in the shape libobs consumes: one array per attribute, because
// gs_vb_data takes one pointer per attribute. Positions are 4 floats per vertex
// because libobs's struct vec3 is 16 bytes wide.
//
// The attribute layout is documented in data/effects/slugged.effect; the order
// below is the TEXCOORD index of each attribute.
struct GeometryBuffers {
	enum Attribute {
		Em,        // em.xy, code (kind + 8 * gradient type), shape A's knock-out alpha
		ABand,     // shape A: band transform
		AShape,    // shape A: band location and limits
		BBand,     // shape B (the outline ring): band transform
		BShape,    // shape B: band location and limits
		Color,     // colour, or gradient start
		Color2,    // gradient end
		Motion,    // preset, amount, start (all), duration
		Pivot,     // pivot.xy, stagger index, start (changed characters only)
		Direction, // direction.xy, radians per second, shape B's knock-out alpha
		Gradient,  // gradient u, v; shadow-to-glyph offset in em
		AttributeCount,
	};

	std::vector<float> positions;
	std::array<std::vector<float>, AttributeCount> attributes;
	std::vector<uint32_t> indices;

	size_t vertexCount() const { return positions.size() / 4; }
	bool empty() const { return indices.empty(); }

	void clear();

	// Moves every vertex (and its motion pivot) by (dx, dy).
	void translate(float dx, float dy);
};

// What a quad's pixels are made of. Must match the KIND_* defines in the effect.
enum class QuadKind {
	Fill = 0,      // coverage of shape A
	Outline = 1,   // coverage of shape B, minus shape A in front of it
	Shadow = 2,    // coverage of A and B, minus A and B at the glyph's position
	Solid = 3,     // filled rectangle (underline, strikeout)
	Image = 4,     // colour bitmap
	ImageTint = 5, // colour bitmap as a flat-colour silhouette (its shadow)
	RoundRect = 6, // background with a corner radius
};

// Which atlas shapes and bitmaps a layout needs, gathered before the atlas is
// built.
struct FrameRequests {
	std::vector<ShapeRequest> shapes;
	std::vector<ImageAtlas::Request> images;
};

FrameRequests collectRequests(const LayoutResult &layout);

struct GeometryParams {
	// Text offsets [changedStart, changedEnd) changed in this frame's text, so a
	// text-change replay can animate just them. Empty when start >= end.
	uint32_t changedStart = 0;
	uint32_t changedEnd = 0;
};

// Ink extent in layout space, excluding antialiasing headroom.
struct InkBounds {
	float x0 = std::numeric_limits<float>::max();
	float y0 = std::numeric_limits<float>::max();
	float x1 = std::numeric_limits<float>::lowest();
	float y1 = std::numeric_limits<float>::lowest();

	bool valid() const { return x1 >= x0 && y1 >= y0; }

	void add(float ax0, float ay0, float ax1, float ay1);
};

struct GeometryStats {
	InkBounds ink;

	// How many characters, words or lines the stagger spreads over, for the
	// whole text and for just the changed part.
	uint32_t units = 0;
	uint32_t changedUnits = 0;
};

// Turns positioned glyphs into drawable quads, layered so plain alpha blending
// is right without depth or sorting: every shadow, then every outline, then
// every fill, then underline and strikeout bars on top. Outlines and shadows
// knock out what is in front of them (see the effect), so a fade or a
// translucent fill never shows the outline or shadow through a letter.
class GeometryBuilder {
public:
	static GeometryStats build(const Document &doc, const LayoutResult &layout, const AtlasCache &atlas,
				   const ImageAtlas &images, const GeometryParams &params, GeometryBuffers &out);

	// A rounded rectangle covering (0, 0)-(width, height).
	static void buildBackground(const Background &background, float width, float height, GeometryBuffers &out);
};

} // namespace slugged
