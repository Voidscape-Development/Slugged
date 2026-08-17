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

#include "atlas_cache.hpp"
#include "../text/font_manager.hpp"
#include "../util/log.hpp"

#include <cmath>

#include "slughorn/canvas.hpp"
#include "slughorn/freetype.hpp"

namespace slugged {

AtlasCache::AtlasCache() = default;
AtlasCache::~AtlasCache() = default;

slughorn::Key AtlasCache::fillKey(uint8_t slot, uint32_t glyphIndex)
{
	// Glyph indices comfortably fit slughorn's 21-bit key field; a face with
	// more than 2M glyphs does not exist.
	return slughorn::Key(glyphIndex & slughorn::Key::CODEPOINT_MASK, slot);
}

float AtlasCache::bucketWidth(float widthEm)
{
	// 1/512 em buckets: finer than any visible difference at realistic sizes,
	// coarse enough that a slider drag reuses shapes instead of rebuilding.
	return std::round(widthEm * 512.0f) / 512.0f;
}

slughorn::Key AtlasCache::outlineKey(uint8_t slot, uint32_t glyphIndex, float widthEm)
{
	std::string name = "stroke:";

	name += std::to_string(unsigned(slot));
	name += ':';
	name += std::to_string(glyphIndex);
	name += ':';
	name += std::to_string(int(std::round(bucketWidth(widthEm) * 512.0f)));

	return slughorn::Key(name);
}

namespace {

slughorn::canvas::LineJoin toSlughornJoin(LineJoin join)
{
	switch (join) {
	case LineJoin::Miter:
		return slughorn::canvas::LineJoin::Miter;
	case LineJoin::Bevel:
		return slughorn::canvas::LineJoin::Bevel;
	case LineJoin::Round:
		break;
	}

	return slughorn::canvas::LineJoin::Round;
}

slughorn::canvas::LineCap toSlughornCap(LineCap cap)
{
	switch (cap) {
	case LineCap::Butt:
		return slughorn::canvas::LineCap::Butt;
	case LineCap::Square:
		return slughorn::canvas::LineCap::Square;
	case LineCap::Round:
		break;
	}

	return slughorn::canvas::LineCap::Round;
}

// Loads a glyph outline in font units and decomposes it into slughorn curves.
// Returns false when the glyph is blank (a space), which is not an error.
bool decompose(FontFace *face, uint32_t glyphIndex, slughorn::Atlas::ShapeInfo &out)
{
	if (!face || !face->ft)
		return false;

	// NO_SCALE keeps the outline in font units so the result is
	// resolution-independent; em normalisation happens via emScale below.
	if (FT_Load_Glyph(face->ft, glyphIndex, FT_LOAD_NO_SCALE))
		return false;

	const float emScale = 1.0f / (face->ft->units_per_EM ? float(face->ft->units_per_EM) : 1000.0f);
	const float advance = float(face->ft->glyph->metrics.horiAdvance) * emScale;

	return slughorn::freetype::decomposeGlyph(face->ft, emScale, advance, nullptr, out);
}

} // namespace

bool AtlasCache::addFill(const Request &req)
{
	slughorn::Atlas::ShapeInfo info;

	if (!decompose(req.face, req.glyphIndex, info))
		return false;

	_atlas->addShape(fillKey(req.slot, req.glyphIndex), info);

	return true;
}

bool AtlasCache::addOutline(const Request &req)
{
	slughorn::Atlas::ShapeInfo info;

	if (!decompose(req.face, req.glyphIndex, info))
		return false;

	// Stroke the glyph's own contours into a new closed shape. Seeding a Path
	// straight from the decomposed curves means the outline follows the true
	// Bezier outline rather than a polygonal approximation of it.
	//
	// One caveat worth knowing: a Path built from raw curves carries no
	// explicit subpath boundaries, so strokePath() infers them from coordinate
	// gaps. That is exactly the situation glyph contours produce (each contour
	// ends where the next begins, at a discontinuity), so it resolves
	// correctly here, but it is a heuristic rather than a guarantee.
	slughorn::canvas::Path path(info.curves);

	if (!path.strokePath(req.outlineWidthEm, false, toSlughornJoin(req.join), toSlughornCap(req.cap),
			     req.miterLimit))
		return false;

	slughorn::canvas::Canvas painter(*_atlas);

	// The colour passed here is irrelevant: geometry.cpp assigns the real
	// per-glyph colour on the vertices at draw time. Only the shape matters.
	painter.fill(path, slughorn::Color{1.0f, 1.0f, 1.0f, 1.0f}, 1.0f,
		     outlineKey(req.slot, req.glyphIndex, req.outlineWidthEm));

	return true;
}

bool AtlasCache::rebuild(const std::unordered_set<Request, RequestHash> &requests)
{
	auto atlas = std::make_unique<slughorn::Atlas>();

	_atlas = std::move(atlas);

	size_t added = 0;

	for (const Request &req : requests) {
		const bool ok = req.outlineWidthEm > 0.0f ? addOutline(req) : addFill(req);

		if (ok)
			added++;
	}

	try {
		_atlas->build();
	} catch (const std::exception &e) {
		slugged_log(LOG_ERROR, "atlas build failed: %s", e.what());
		_atlas.reset();
		_registered.clear();

		return false;
	}

	_registered = requests;
	_generation++;

	slugged_log(LOG_DEBUG, "atlas rebuilt: %zu of %zu shapes, generation %llu", added, requests.size(),
		    (unsigned long long)_generation);

	return true;
}

bool AtlasCache::ensure(const std::vector<PositionedGlyph> &glyphs)
{
	std::unordered_set<Request, RequestHash> required;

	for (const PositionedGlyph &g : glyphs) {
		if (!g.face || !g.style)
			continue;

		Request fill;

		fill.slot = g.face->slot;
		fill.glyphIndex = g.glyphIndex;
		fill.face = g.face;

		required.insert(fill);

		const Outline &outline = g.style->outline;

		if (outline.enabled && outline.widthPx > 0.0f && g.sizePx > 0.0f) {
			Request stroke = fill;

			// Outline width is authored in pixels but shapes are em-space,
			// so the same shape can be shared by every glyph at that ratio.
			stroke.outlineWidthEm = bucketWidth(outline.widthPx / g.sizePx);
			stroke.join = outline.join;
			stroke.cap = outline.cap;
			stroke.miterLimit = outline.miterLimit;

			if (stroke.outlineWidthEm > 0.0f)
				required.insert(stroke);
		}
	}

	if (required.empty()) {
		if (_atlas) {
			_atlas.reset();
			_registered.clear();
			_generation++;

			return true;
		}

		return false;
	}

	// Rebuild only when something is genuinely missing. Shapes that are no
	// longer needed are deliberately kept until the next real rebuild, so
	// deleting a character and retyping it does not churn the atlas.
	bool complete = _atlas != nullptr;

	if (complete) {
		for (const Request &req : required) {
			if (!_registered.count(req)) {
				complete = false;
				break;
			}
		}
	}

	if (complete)
		return false;

	// Carry forward what was already registered so the atlas grows
	// monotonically within a single edit session.
	std::unordered_set<Request, RequestHash> merged = _registered;

	for (const Request &req : required)
		merged.insert(req);

	return rebuild(merged);
}

} // namespace slugged
