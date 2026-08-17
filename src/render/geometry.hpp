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

#include <cstdint>
#include <vector>

namespace slugged {

// Geometry in the exact shape libobs wants to consume.
//
// Structure-of-arrays rather than interleaved, because gs_vb_data takes one
// pointer per attribute. Positions are 4 floats per vertex, not 3: libobs's
// struct vec3 is SSE-aligned and 16 bytes wide, so a packed 3-float array would
// be misread as garbage.
struct GeometryBuffers {
	std::vector<float> positions; // 4 floats/vertex (x, y, z, unused)
	std::vector<float> em;        // 4 floats/vertex
	std::vector<float> bandXform; // 4
	std::vector<float> shapeData; // 4
	std::vector<float> color;     // 4
	std::vector<float> fx;        // 4: mode, param, startTime, duration
	std::vector<float> pivot;     // 4: pivotX, pivotY, glyphOrdinal, glyphCount

	std::vector<uint32_t> indices;

	size_t vertexCount() const { return positions.size() / 4; }

	bool empty() const { return indices.empty(); }

	void clear()
	{
		positions.clear();
		em.clear();
		bandXform.clear();
		shapeData.clear();
		color.clear();
		fx.clear();
		pivot.clear();
		indices.clear();
	}
};

// Turns positioned glyphs into drawable quads.
//
// Each glyph can contribute up to three quads, emitted back to front so plain
// alpha blending gives the right result without depth or sorting: drop shadow,
// then outline, then fill.
class GeometryBuilder {
public:
	// `atlas` must already contain every shape `glyphs` refers to; AtlasCache
	// guarantees that. Returns false when there is nothing to draw.
	static bool build(const Document &doc, const LayoutResult &layout, const AtlasCache &cache,
			  GeometryBuffers &out);
};

} // namespace slugged
