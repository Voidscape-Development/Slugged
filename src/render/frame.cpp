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

namespace {

// How much room the laid-out lines take, whatever box they were laid out in.
bool fitsIn(const LayoutResult &layout, float width, float height)
{
	if (layout.lines.empty())
		return true;

	float widest = 0.0f;

	for (const LineInfo &line : layout.lines)
		widest = std::max(widest, line.width);

	const float top = layout.lines.front().baselineY - layout.lines.front().ascent;
	const float bottom = layout.lines.back().baselineY + layout.lines.back().descent;

	return widest <= width + 0.5f && bottom - top <= height + 0.5f;
}

// Shrinks and then cuts `doc` until it fits the box, as its fit settings allow.
// Returns the layout of what fits best; `doc` is left as what was laid out.
LayoutResult fitToBox(Document &doc, float wrapWidth, float width, float height)
{
	LayoutResult layout = Layout::run(doc, wrapWidth, height);

	if (fitsIn(layout, width, height))
		return layout;

	if (doc.fit.shrink) {
		const Document original = doc;

		auto attempt = [&](float scale, LayoutResult &out) {
			doc = original;
			doc.scaleSizes(scale);
			out = Layout::run(doc, wrapWidth, height);

			return fitsIn(out, width, height);
		};

		// Text that does not wrap shrinks in proportion, so the ratio of the
		// box to the text is close to the answer; wrapped text reflows as it
		// shrinks, so search from there.
		float widest = 1.0f;

		for (const LineInfo &line : layout.lines)
			widest = std::max(widest, line.width);

		const float tall =
			std::max(1.0f, layout.lines.back().baselineY + layout.lines.back().descent -
					       (layout.lines.front().baselineY - layout.lines.front().ascent));

		float lo = std::clamp(doc.fit.minScale, 0.01f, 1.0f);
		float hi = 1.0f;
		float best = -1.0f;
		LayoutResult bestLayout;
		LayoutResult trial;

		const float guess = std::clamp(std::min(width / widest, height / tall), lo, hi);

		if (attempt(guess, trial)) {
			best = guess;
			bestLayout = std::move(trial);
			lo = guess;
		} else {
			hi = guess;
		}

		for (int i = 0; i < 7 && hi - lo > 0.005f; i++) {
			const float mid = (lo + hi) * 0.5f;

			if (attempt(mid, trial)) {
				best = mid;
				bestLayout = std::move(trial);
				lo = mid;
			} else {
				hi = mid;
			}
		}

		if (best > 0.0f) {
			doc = original;
			doc.scaleSizes(best);
			return bestLayout;
		}

		// Not even the smallest allowed size fits: keep that size, and let
		// the ellipsis deal with the rest.
		attempt(std::clamp(original.fit.minScale, 0.01f, 1.0f), layout);
	}

	if (!doc.fit.ellipsis)
		return layout;

	// Keep as many characters as fit with an ellipsis after them.
	const Document whole = doc;
	const std::string plain = whole.plainText();

	std::vector<size_t> starts;

	for (size_t i = 0; i < plain.size(); i++)
		if ((uint8_t(plain[i]) & 0xC0) != 0x80)
			starts.push_back(i);

	auto cutAt = [&](size_t count, LayoutResult &out) {
		size_t cut = count < starts.size() ? starts[count] : plain.size();

		while (cut > 0 && (plain[cut - 1] == ' ' || plain[cut - 1] == '\n'))
			cut--;

		doc = whole;
		doc.replaceText(cut, plain.size(), "\xE2\x80\xA6");
		out = Layout::run(doc, wrapWidth, height);

		return fitsIn(out, width, height);
	};

	// The most characters that fit: `kept` always fits (or is zero).
	size_t kept = 0;
	size_t lo = 1;
	size_t hi = starts.size();
	LayoutResult trial;

	while (lo <= hi) {
		const size_t mid = lo + (hi - lo) / 2;

		if (cutAt(mid, trial)) {
			kept = mid;
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}

	cutAt(kept, trial);

	return trial;
}

} // namespace

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

	// Text too big for a fixed box shrinks, or is cut short, when asked to.
	// Scrolling text is meant to be bigger than its box.
	Document fitted;
	const bool fit = fixed && !doc.scroll.enabled && (doc.fit.shrink || doc.fit.ellipsis) && innerWidth > 0.0f &&
			 innerHeight > 0.0f;

	if (fit)
		fitted = doc;

	const LayoutResult layout =
		fit ? fitToBox(fitted, innerWidth, innerWidth, innerHeight)
		    : Layout::run(doc, fixed && !horizontalScroll ? innerWidth : 0.0f, fixed ? innerHeight : 0.0f);

	const FrameRequests requests = collectRequests(layout);

	state.atlas.ensure(requests.shapes);
	state.images.ensure(requests.images);

	const GeometryStats stats =
		GeometryBuilder::build(doc, layout, state.atlas, state.images, input.params, out.geometry, &out.halos);

	out.units = stats.units;
	out.changedUnits = stats.changedUnits;

	// The text's extent: its ink, and at least the line boxes, so trailing
	// spaces and empty lines still take up room.
	InkBounds extent = stats.ink;
	extent.add(0.0f, 0.0f, layout.width, layout.height);

	float editX = padding;
	float editY = padding;

	if (fixed) {
		out.geometry.translate(padding, padding);
		out.halos.geometry.translate(padding, padding);

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
		out.halos.geometry.translate(dx, dy);

		editX = dx;
		editY = dy;

		out.width = uint32_t(std::max(1.0f, std::ceil(std::max(extent.x1, 0.0f) + dx + padding)));
		out.height = uint32_t(std::max(1.0f, std::ceil(std::max(extent.y1, 0.0f) + dy + padding)));

		// An auto-sized ticker scrolls through its own box.
		out.clip = doc.scroll.enabled;

		out.contentX0 = extent.x0 + dx;
		out.contentY0 = extent.y0 + dy;
		out.contentX1 = extent.x1 + dx;
		out.contentY1 = extent.y1 + dy;
	}

	if (input.wantEditMap)
		out.editMap = std::make_shared<const EditMap>(
			EditMap::build(layout, input.tokenSpans, editX, editY, input.writtenSize));

	GeometryBuilder::buildBackground(doc.background, float(out.width), float(out.height), out.background);

	if (!out.halos.empty()) {
		// The halos' image covers their silhouettes and the blur's reach,
		// with room for the letters to move: motion slides them, bounces
		// them and spins them, and effects shake them, all in the vertex
		// shader, after this rectangle is fixed.
		InkBounds bounds;
		const auto &positions = out.halos.geometry.positions;

		for (size_t i = 0; i + 1 < positions.size(); i += 4)
			bounds.add(positions[i], positions[i + 1], positions[i], positions[i + 1]);

		float reach = 0.0f;

		for (const HaloGroup &group : out.halos.groups)
			reach = std::max(reach, group.reach);

		const float span = std::max(bounds.x1 - bounds.x0, bounds.y1 - bounds.y0);
		float room = reach + std::max(24.0f, span * 0.1f);

		switch (doc.motion.motion) {
		case Motion::Slide:
		case Motion::Bounce:
		case Motion::Wave:
			room += std::fabs(doc.motion.param);
			break;
		case Motion::Spin:
			room += span * 0.5f;
			break;
		default:
			break;
		}

		out.haloX0 = std::floor(bounds.x0 - room);
		out.haloY0 = std::floor(bounds.y0 - room);
		out.haloX1 = std::ceil(bounds.x1 + room);
		out.haloY1 = std::ceil(bounds.y1 + room);
	}

	out.atlas = state.atlas.shared();
	out.atlasGeneration = state.atlas.generation();
	out.image = state.images.pixels();
	out.imageWidth = state.images.width();
	out.imageHeight = state.images.height();
	out.imageGeneration = state.images.generation();

	return out;
}

} // namespace slugged
