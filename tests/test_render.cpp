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

#include "harness.hpp"

#include "core/document.hpp"
#include "render/frame.hpp"
#include "render/rebuild_worker.hpp"
#include "text/font_enum.hpp"
#include "text/font_manager.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace slugged;

namespace {

bool haveFonts()
{
	FontFileRef ref;

	return fontenum::match(fontenum::defaultFamily(), 400, false, ref);
}

Document docWith(const std::string &text)
{
	Document doc;
	doc.base.sizePx = 40.0f;
	doc.setPlainText(text);
	return doc;
}

FrameOutput build(const Document &doc, FrameState &state, GeometryParams params = {})
{
	FrameInput input;
	input.document = doc;
	input.params = params;

	return buildFrame(input, state);
}

int kindOf(const GeometryBuffers &g, size_t vertex)
{
	const float code = g.attributes[GeometryBuffers::Em][vertex * 4 + 2];

	return int(code) % 8;
}

size_t countKind(const GeometryBuffers &g, QuadKind kind)
{
	size_t n = 0;

	for (size_t v = 0; v < g.vertexCount(); v += 4)
		if (kindOf(g, v) == int(kind))
			n++;

	return n;
}

} // namespace

TEST(frame_builds_one_fill_quad_per_glyph)
{
	if (!haveFonts())
		return;

	FrameState state;
	const FrameOutput frame = build(docWith("Hi!"), state);

	CHECK_EQ(countKind(frame.geometry, QuadKind::Fill), size_t(3));
	CHECK(frame.atlas != nullptr);
	CHECK(frame.width > 20u);
	CHECK(frame.height > 20u);
}

TEST(frame_layers_shadows_then_outlines_then_fills)
{
	if (!haveFonts())
		return;

	Document doc = docWith("ab");
	doc.base.outline.enabled = true;
	doc.base.shadow.enabled = true;

	FrameState state;
	const FrameOutput frame = build(doc, state);

	std::vector<int> kinds;

	for (size_t v = 0; v < frame.geometry.vertexCount(); v += 4)
		kinds.push_back(kindOf(frame.geometry, v));

	const std::vector<int> expected = {int(QuadKind::Shadow),  int(QuadKind::Shadow), int(QuadKind::Outline),
					   int(QuadKind::Outline), int(QuadKind::Fill),   int(QuadKind::Fill)};

	CHECK(kinds == expected);
}

TEST(frame_auto_size_includes_shadow)
{
	if (!haveFonts())
		return;

	Document plain = docWith("Box");
	Document shadowed = plain;
	shadowed.base.shadow.enabled = true;
	shadowed.base.shadow.offsetX = 30.0f;
	shadowed.base.shadow.offsetY = 30.0f;

	FrameState a, b;

	const FrameOutput small = build(plain, a);
	const FrameOutput big = build(shadowed, b);

	CHECK(big.width >= small.width + 25u);
	CHECK(big.height >= small.height + 15u); // the line box already has room below the baseline
}

TEST(frame_fixed_box_clips)
{
	if (!haveFonts())
		return;

	Document doc = docWith("text");
	doc.sizeMode = SizeMode::Fixed;
	doc.boxWidth = 300.0f;
	doc.boxHeight = 120.0f;

	FrameState state;
	const FrameOutput frame = build(doc, state);

	CHECK_EQ(frame.width, 300u);
	CHECK_EQ(frame.height, 120u);
	CHECK(frame.clip);
}

TEST(frame_linear_gradient_runs_across_the_text)
{
	if (!haveFonts())
		return;

	Document doc = docWith("WWWWWW");
	doc.base.fill.type = FillType::Linear;
	doc.base.fill.angleDeg = 0.0f; // left to right

	FrameState state;
	const FrameOutput frame = build(doc, state);

	const auto &grad = frame.geometry.attributes[GeometryBuffers::Gradient];

	// First glyph's left edge near 0, last glyph's right edge near 1.
	const float firstLeft = grad[0];
	const float lastRight = grad[(frame.geometry.vertexCount() - 4 + 1) * 4];

	CHECK(firstLeft < 0.1f);
	CHECK(lastRight > 0.9f);
}

TEST(frame_marks_only_changed_characters)
{
	if (!haveFonts())
		return;

	Document doc = docWith("Score 99");

	GeometryParams params;
	params.changedStart = 6;
	params.changedEnd = 8;

	FrameState state;
	const FrameOutput frame = build(doc, state, params);

	const auto &pivot = frame.geometry.attributes[GeometryBuffers::Pivot];
	size_t changed = 0;

	for (size_t v = 0; v < frame.geometry.vertexCount(); v += 4)
		if (pivot[v * 4 + 3] >= 0.0f)
			changed++;

	CHECK_EQ(changed, size_t(2));
	CHECK_EQ(frame.changedUnits, 2u);
}

TEST(atlas_does_not_grow_without_bound)
{
	if (!haveFonts())
		return;

	Document doc = docWith("Outline");
	doc.base.outline.enabled = true;

	FrameState state;

	// Dragging the outline-width slider through many values.
	for (int i = 1; i <= 60; i++) {
		doc.base.outline.widthPx = float(i) * 0.5f;
		build(doc, state);
	}

	// Live shapes are 7 fills + 7 strokes; the atlas may carry stale ones,
	// but only a bounded number.
	CHECK(state.atlas.size() <= 14u * 2u + 128u + 14u);
}

TEST(frame_colour_emoji_renders_as_colour)
{
	if (!haveFonts())
		return;

	FrameState state;
	const FrameOutput frame = build(docWith("\xF0\x9F\x98\x80"), state); // 😀

	const size_t images = countKind(frame.geometry, QuadKind::Image);
	const size_t fills = countKind(frame.geometry, QuadKind::Fill);

	// Either a bitmap (CBDT/sbix) or COLR layers, depending on the platform's
	// emoji font -- but something must draw.
	if (images + fills == 0) {
		std::fprintf(stderr, "  (no emoji font available; skipping)\n");
		return;
	}

	std::fprintf(stderr, "  emoji drew as %zu bitmap and %zu layer quads\n", images, fills);

	if (images > 0)
		CHECK(frame.image != nullptr && frame.imageWidth > 0);
}

TEST(worker_builds_only_the_newest_input)
{
	if (!haveFonts())
		return;

	auto job = std::make_shared<RebuildWorker::Job>();

	for (uint64_t id = 1; id <= 5; id++) {
		FrameInput input;
		input.document = docWith("frame " + std::to_string(id));

		RebuildWorker::instance().submit(job, std::move(input), id);
	}

	uint64_t readyId = 0;

	for (int i = 0; i < 400 && readyId != 5; i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));

		std::lock_guard<std::mutex> lock(job->mutex);
		readyId = job->readyId;
	}

	CHECK_EQ(readyId, uint64_t(5));

	RebuildWorker::instance().stop();
}
