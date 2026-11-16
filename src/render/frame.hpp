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

#include "atlas_cache.hpp"
#include "geometry.hpp"
#include "../text/edit_map.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace slugged {

// Everything a source needs to draw one version of its text, built on the CPU
// with no graphics context: layout, atlas, geometry and the source's size. The
// graphics thread only uploads it.
struct FrameInput {
	// The document with tokens already expanded.
	Document document;

	// Lay out in this box instead of the document's own size settings. The
	// overlay filter uses it to lay out against the source it sits on.
	bool overrideBox = false;
	float boxWidth = 0.0f;
	float boxHeight = 0.0f;

	GeometryParams params;

	// Where tokens were replaced, and how long the text was before they were,
	// so the frame can describe itself in the text as written.
	std::vector<TokenSpan> tokenSpans;
	uint32_t writtenSize = 0;

	// Only an open editor needs the edit map.
	bool wantEditMap = false;
};

struct FrameOutput {
	GeometryBuffers geometry;
	GeometryBuffers background;

	std::shared_ptr<const slughorn::Atlas> atlas;
	uint64_t atlasGeneration = 0;

	std::shared_ptr<const std::vector<uint8_t>> image;
	uint32_t imageWidth = 0;
	uint32_t imageHeight = 0;
	uint64_t imageGeneration = 0;

	// The source's size.
	uint32_t width = 1;
	uint32_t height = 1;

	// Extent of the drawn text in source space, for scrolling.
	float contentX0 = 0.0f;
	float contentY0 = 0.0f;
	float contentX1 = 0.0f;
	float contentY1 = 0.0f;

	// Whether drawing is clipped to (0, 0)-(width, height).
	bool clip = false;

	// Present when the input asked for it.
	std::shared_ptr<const EditMap> editMap;

	// Stagger units, for timing a pass.
	uint32_t units = 0;
	uint32_t changedUnits = 0;
};

// Per-source state that persists between frames.
struct FrameState {
	AtlasCache atlas;
	ImageAtlas images;
};

// Builds a frame. Takes the FontManager lock for its duration.
FrameOutput buildFrame(const FrameInput &input, FrameState &state);

} // namespace slugged
