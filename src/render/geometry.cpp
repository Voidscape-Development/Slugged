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

#include "geometry.hpp"
#include "../text/font_manager.hpp"

#include <algorithm>
#include <cmath>

namespace slugged {

namespace {

// Antialiasing headroom around each quad.
//
// Slug solves coverage analytically from the em coordinate, so a fragment just
// outside the glyph's true bounding box still needs to be rasterised for the
// edge to be smooth. The authored quad is ground truth, so this margin only
// ever grows the rasterisation area outward and never moves the glyph.
//
// The margin is derived from the glyph's pixel size rather than being a fixed
// em constant: one pixel of fringe is a much larger fraction of an em at 12px
// than at 200px, and a fixed constant would clip the fringe on small text.
float aaMarginEm(float sizePx)
{
	constexpr float kFringePixels = 1.5f;

	if (sizePx <= 0.0f)
		return 0.02f;

	return std::max(kFringePixels / sizePx, 0.005f);
}

struct QuadParams {
	const slughorn::Atlas::Shape *shape = nullptr;

	float penX = 0.0f;
	float baselineY = 0.0f;
	float sizePx = 0.0f;

	Rgba color;

	float motionMode = 0.0f;
	float motionParam = 0.0f;
	float startTime = 0.0f;
	float duration = 1.0f;

	float ordinal = 0.0f;
	float glyphCount = 1.0f;
};

void emitQuad(const QuadParams &p, GeometryBuffers &out)
{
	const slughorn::Atlas::Shape &s = *p.shape;

	// Skip shapes with no area (spaces, control glyphs); they carry no curves
	// and would render nothing while still costing four vertices.
	if (float(s.width) <= 0.0f || float(s.height) <= 0.0f)
		return;

	const float margin = aaMarginEm(p.sizePx);

	// Em-space bounds of the quad, expanded by the AA margin.
	const float emX0 = float(s.bearingX) - margin;
	const float emY0 = float(s.bearingY) - float(s.height) - margin;
	const float emX1 = float(s.bearingX) + float(s.width) + margin;
	const float emY1 = float(s.bearingY) + margin;

	// Screen-space corners. Layout is y-down (OBS source space) while glyph
	// em-space is y-up, so y is negated about the baseline. The em coordinates
	// are *not* flipped: each vertex keeps the true em coordinate of the glyph
	// point it sits on, which is what the coverage solve interpolates. The net
	// effect is a vertical mirror of the quad in screen space with a consistent
	// em mapping, so winding flips -- the renderer disables culling for this
	// reason.
	const float x0 = p.penX + emX0 * p.sizePx;
	const float x1 = p.penX + emX1 * p.sizePx;
	const float y0 = p.baselineY - emY0 * p.sizePx;
	const float y1 = p.baselineY - emY1 * p.sizePx;

	const float pivotX = (x0 + x1) * 0.5f;
	const float pivotY = (y0 + y1) * 0.5f;

	const uint32_t base = uint32_t(out.vertexCount());

	// Corner order matches the em/position pairing above: (emX0,emY0),
	// (emX1,emY0), (emX1,emY1), (emX0,emY1).
	const float px[4] = {x0, x1, x1, x0};
	const float py[4] = {y0, y0, y1, y1};
	const float ex[4] = {emX0, emX1, emX1, emX0};
	const float ey[4] = {emY0, emY0, emY1, emY1};
	const float uvx[4] = {0.0f, 1.0f, 1.0f, 0.0f};
	const float uvy[4] = {0.0f, 0.0f, 1.0f, 1.0f};

	for (int i = 0; i < 4; i++) {
		out.positions.push_back(px[i]);
		out.positions.push_back(py[i]);
		out.positions.push_back(0.0f);
		out.positions.push_back(0.0f); // vec3 is 16 bytes wide in libobs

		out.em.push_back(ex[i]);
		out.em.push_back(ey[i]);
		out.em.push_back(uvx[i]);
		out.em.push_back(uvy[i]);

		out.bandXform.push_back(float(s.bandScaleX));
		out.bandXform.push_back(float(s.bandScaleY));
		out.bandXform.push_back(float(s.bandOffsetX));
		out.bandXform.push_back(float(s.bandOffsetY));

		out.shapeData.push_back(float(s.bandTexX));
		out.shapeData.push_back(float(s.bandTexY));
		out.shapeData.push_back(float(s.bandMaxX));
		out.shapeData.push_back(float(s.bandMaxY));

		out.color.push_back(p.color.r);
		out.color.push_back(p.color.g);
		out.color.push_back(p.color.b);
		out.color.push_back(p.color.a);

		out.fx.push_back(p.motionMode);
		out.fx.push_back(p.motionParam);
		out.fx.push_back(p.startTime);
		out.fx.push_back(p.duration);

		out.pivot.push_back(pivotX);
		out.pivot.push_back(pivotY);
		out.pivot.push_back(p.ordinal);
		out.pivot.push_back(p.glyphCount);
	}

	out.indices.push_back(base + 0);
	out.indices.push_back(base + 1);
	out.indices.push_back(base + 2);
	out.indices.push_back(base + 0);
	out.indices.push_back(base + 2);
	out.indices.push_back(base + 3);
}

// Which unit of the text this glyph's animation delay is counted in.
float staggerIndex(const PositionedGlyph &g, MotionOrder order)
{
	switch (order) {
	case MotionOrder::Together:
		return 0.0f;
	case MotionOrder::PerGlyph:
		return float(g.ordinal);
	case MotionOrder::PerWord:
		return float(g.wordIndex);
	case MotionOrder::PerLine:
		return float(g.lineIndex);
	}

	return 0.0f;
}

// Evaluates a gradient at t, so per-glyph gradient fills can be resolved on the
// CPU into a flat colour per quad. A true per-pixel gradient would need
// slughorn's gradient atlas wired through the shader; sampling per glyph covers
// the common "ramp across the text" case at no shader cost.
Rgba sampleFill(const Fill &fill, float t)
{
	if (fill.type == Fill::Type::Solid || fill.stops.size() < 2)
		return fill.color;

	t = std::min(std::max(t, 0.0f), 1.0f);

	const GradientStop *lo = &fill.stops.front();
	const GradientStop *hi = &fill.stops.back();

	for (size_t i = 0; i + 1 < fill.stops.size(); i++) {
		if (t >= fill.stops[i].t && t <= fill.stops[i + 1].t) {
			lo = &fill.stops[i];
			hi = &fill.stops[i + 1];
			break;
		}
	}

	const float span = hi->t - lo->t;
	const float k = span > 1e-6f ? (t - lo->t) / span : 0.0f;

	Rgba out;

	out.r = lo->color.r + (hi->color.r - lo->color.r) * k;
	out.g = lo->color.g + (hi->color.g - lo->color.g) * k;
	out.b = lo->color.b + (hi->color.b - lo->color.b) * k;
	out.a = lo->color.a + (hi->color.a - lo->color.a) * k;

	return out;
}

} // namespace

bool GeometryBuilder::build(const Document &doc, const LayoutResult &layout, const AtlasCache &cache,
			    GeometryBuffers &out)
{
	out.clear();

	const slughorn::Atlas *atlas = cache.atlas();

	if (!atlas || layout.glyphs.empty())
		return false;

	const MotionSpec &motion = doc.motion;
	const float glyphCount = float(std::max<size_t>(layout.glyphs.size(), 1));

	// Gradient parameter runs along the layout's own extent, so a ramp spans
	// the visible text rather than an arbitrary fixed distance.
	const float spanX = std::max(layout.width, 1.0f);
	const float spanY = std::max(layout.height, 1.0f);

	for (const PositionedGlyph &g : layout.glyphs) {
		if (!g.face || !g.style)
			continue;

		const Style &style = *g.style;

		const float startTime = motion.motion == Motion::None ? 0.0f
								      : staggerIndex(g, motion.order) * motion.stagger;

		QuadParams params;

		params.penX = g.x;
		params.baselineY = g.y;
		params.sizePx = g.sizePx;
		params.motionMode = float(int(motion.motion));
		params.motionParam = motion.param;
		params.startTime = startTime;
		params.duration = std::max(motion.duration, 1e-4f);
		params.ordinal = float(g.ordinal);
		params.glyphCount = glyphCount;

		// Gradient position: along the ramp direction across the whole block,
		// or across the glyph's own box when perGlyph is set.
		float t = 0.0f;

		if (style.fill.type != Fill::Type::Solid) {
			if (style.fill.perGlyph) {
				t = glyphCount > 1.0f ? float(g.ordinal) / (glyphCount - 1.0f) : 0.0f;
			} else {
				const float rad = style.fill.angleDeg * 3.14159265f / 180.0f;

				t = (g.x / spanX) * std::cos(rad) + (g.y / spanY) * std::sin(rad);
			}
		}

		const Rgba fillColor = sampleFill(style.fill, t);

		// ---- drop shadow (drawn first, so everything covers it) ----------
		if (style.shadow.enabled) {
			const slughorn::Key key = AtlasCache::fillKey(g.face->slot, g.glyphIndex);
			const auto shape = atlas->getShape(key);

			if (shape) {
				QuadParams shadow = params;

				shadow.shape = &shape.value();
				shadow.penX = g.x + style.shadow.offsetX;
				shadow.baselineY = g.y + style.shadow.offsetY;
				shadow.color = style.shadow.color;

				emitQuad(shadow, out);
			}
		}

		// ---- outline -----------------------------------------------------
		if (style.outline.enabled && style.outline.widthPx > 0.0f && g.sizePx > 0.0f) {
			const float widthEm = AtlasCache::bucketWidth(style.outline.widthPx / g.sizePx);
			const slughorn::Key key = AtlasCache::outlineKey(g.face->slot, g.glyphIndex, widthEm);
			const auto shape = atlas->getShape(key);

			if (shape) {
				QuadParams outline = params;

				outline.shape = &shape.value();
				outline.color = style.outline.color;

				emitQuad(outline, out);
			}
		}

		// ---- fill --------------------------------------------------------
		{
			const slughorn::Key key = AtlasCache::fillKey(g.face->slot, g.glyphIndex);
			const auto shape = atlas->getShape(key);

			if (shape) {
				QuadParams fill = params;

				fill.shape = &shape.value();
				fill.color = fillColor;

				emitQuad(fill, out);
			}
		}
	}

	return !out.empty();
}

} // namespace slugged
