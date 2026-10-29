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

#include "frame.hpp"
#include "../text/font_manager.hpp"

#include <algorithm>
#include <cmath>

namespace slugged {

FrameOutput buildFrame(const FrameInput &input, FrameState &state)
{
	FrameOutput out;

	const Document &doc = input.document;

	const bool fixed = input.overrideBox || doc.sizeMode == SizeMode::Fixed;
	const float boxWidth = input.overrideBox ? input.boxWidth : doc.boxWidth;
	const float boxHeight = input.overrideBox ? input.boxHeight : doc.boxHeight;
	const float padding = std::max(doc.padding, 0.0f);

	// A horizontal ticker reads as one long line; wrapping it would defeat it.
	const bool horizontalScroll = doc.scroll.enabled && doc.scroll.speedX != 0.0f;

	const float innerWidth = std::max(0.0f, boxWidth - 2.0f * padding);
	const float innerHeight = std::max(0.0f, boxHeight - 2.0f * padding);

	auto guard = FontManager::instance().lock();

	const LayoutResult layout =
		Layout::run(doc, fixed && !horizontalScroll ? innerWidth : 0.0f, fixed ? innerHeight : 0.0f);

	const FrameRequests requests = collectRequests(layout);

	state.atlas.ensure(requests.shapes);
	state.images.ensure(requests.images);

	const GeometryStats stats =
		GeometryBuilder::build(doc, layout, state.atlas, state.images, input.params, out.geometry);

	out.units = stats.units;
	out.changedUnits = stats.changedUnits;

	// The text's extent: its ink, and at least the line boxes, so trailing
	// spaces and empty lines still take up room.
	InkBounds extent = stats.ink;
	extent.add(0.0f, 0.0f, layout.width, layout.height);

	if (fixed) {
		out.geometry.translate(padding, padding);

		out.width = uint32_t(std::max(1.0f, std::round(boxWidth)));
		out.height = uint32_t(std::max(1.0f, std::round(boxHeight)));
		out.clip = true;

		out.contentX0 = extent.x0 + padding;
		out.contentY0 = extent.y0 + padding;
		out.contentX1 = extent.x1 + padding;
		out.contentY1 = extent.y1 + padding;
	} else {
		// Grow to the ink, so an outline, a shadow or an italic overhang is
		// never cut off by the source's own bounds.
		const float dx = padding - std::min(0.0f, extent.x0);
		const float dy = padding - std::min(0.0f, extent.y0);

		out.geometry.translate(dx, dy);

		out.width = uint32_t(std::max(1.0f, std::ceil(std::max(extent.x1, 0.0f) + dx + padding)));
		out.height = uint32_t(std::max(1.0f, std::ceil(std::max(extent.y1, 0.0f) + dy + padding)));

		// An auto-sized ticker scrolls through its own box.
		out.clip = doc.scroll.enabled;

		out.contentX0 = extent.x0 + dx;
		out.contentY0 = extent.y0 + dy;
		out.contentX1 = extent.x1 + dx;
		out.contentY1 = extent.y1 + dy;
	}

	GeometryBuilder::buildBackground(doc.background, float(out.width), float(out.height), out.background);

	out.atlas = state.atlas.shared();
	out.atlasGeneration = state.atlas.generation();
	out.image = state.images.pixels();
	out.imageWidth = state.images.width();
	out.imageHeight = state.images.height();
	out.imageGeneration = state.images.generation();

	return out;
}

} // namespace slugged
