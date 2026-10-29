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
#include <functional>
#include <set>
#include <string>

namespace slugged {

namespace {

constexpr float kPi = 3.14159265f;

using slughorn::Atlas;

std::string fillKey(const FontInstance *font, uint32_t glyph)
{
	return "f" + std::to_string(font->id) + ":" + std::to_string(glyph);
}

std::string strokeKey(const FontInstance *font, uint32_t glyph, float widthEm, LineJoin join)
{
	return "s" + std::to_string(font->id) + ":" + std::to_string(glyph) + ":" +
	       std::to_string(int(std::lround(ShapeCache::bucketWidth(widthEm) * 512.0f))) + ":" +
	       std::to_string(int(join));
}

std::string layerKey(const FontInstance *font, uint32_t glyph, size_t layer)
{
	return "c" + std::to_string(font->id) + ":" + std::to_string(glyph) + ":" + std::to_string(layer);
}

std::string imageKey(const FontInstance *font, uint32_t glyph)
{
	return "b" + std::to_string(font->id) + ":" + std::to_string(glyph);
}

bool wantsOutline(const Style &style)
{
	return style.outline.enabled && style.outline.widthPx > 0.0f;
}

// The visible outline width is outside the glyph's edge; the stroke is centred
// on the edge and its inner half is covered by the fill, so it is twice as wide.
float strokeWidthEm(const Style &style, float sizePx)
{
	return sizePx > 0.0f ? 2.0f * style.outline.widthPx / sizePx : 0.0f;
}

// Antialiasing headroom in em, so fragments just outside the shape's box are
// rasterised and the edge stays smooth. Derived from pixels, since one pixel is a
// much larger fraction of an em at 12 px than at 200 px. `extraPixels` widens it
// for blur, which spreads coverage past the outline on purpose.
float aaMarginEm(float sizePx, float extraPixels)
{
	if (sizePx <= 0.0f)
		return 0.02f;

	return std::max((1.5f + std::max(extraPixels, 0.0f)) / sizePx, 0.005f);
}

struct Corner {
	float x;
	float y;
	float ex;
	float ey;
};

// Everything about a quad that is the same at all four corners.
struct QuadAttrs {
	QuadKind kind = QuadKind::Fill;
	int gradientType = 0; // 0 none, 1 linear, 2 radial

	std::array<float, 4> aBand{};
	std::array<float, 4> aShape{};
	std::array<float, 4> bBand{};
	std::array<float, 4> bShape{};

	float knockA = 0.0f;
	float knockB = 0.0f;
	float knockOffsetX = 0.0f;
	float knockOffsetY = 0.0f;

	Rgba color;
	Rgba color2;

	float motionMode = 0.0f;
	float motionParam = 0.0f;
	float startAll = 0.0f;
	float duration = 1.0f;
	float staggerIndex = 0.0f;
	float startChanged = -1.0f;
	float dirX = 0.0f;
	float dirY = -1.0f;
	float omega = 0.0f;
};

using GradientFn = std::function<std::pair<float, float>(const Corner &)>;

void push4(std::vector<float> &v, float a, float b, float c, float d)
{
	v.push_back(a);
	v.push_back(b);
	v.push_back(c);
	v.push_back(d);
}

void push4(std::vector<float> &v, const std::array<float, 4> &a)
{
	v.insert(v.end(), a.begin(), a.end());
}

void emit(GeometryBuffers &out, const std::array<Corner, 4> &corners, const QuadAttrs &a, const GradientFn &gradient)
{
	const uint32_t base = uint32_t(out.vertexCount());

	float pivotX = 0.0f;
	float pivotY = 0.0f;

	for (const Corner &c : corners) {
		pivotX += c.x * 0.25f;
		pivotY += c.y * 0.25f;
	}

	auto &attr = out.attributes;

	for (const Corner &c : corners) {
		push4(out.positions, c.x, c.y, 0.0f, 1.0f);

		push4(attr[GeometryBuffers::Em], c.ex, c.ey, float(int(a.kind) + 8 * a.gradientType), a.knockA);
		push4(attr[GeometryBuffers::ABand], a.aBand);
		push4(attr[GeometryBuffers::AShape], a.aShape);
		push4(attr[GeometryBuffers::BBand], a.bBand);
		push4(attr[GeometryBuffers::BShape], a.bShape);
		push4(attr[GeometryBuffers::Color], a.color.r, a.color.g, a.color.b, a.color.a);
		push4(attr[GeometryBuffers::Color2], a.color2.r, a.color2.g, a.color2.b, a.color2.a);
		push4(attr[GeometryBuffers::Motion], a.motionMode, a.motionParam, a.startAll, a.duration);
		push4(attr[GeometryBuffers::Pivot], pivotX, pivotY, a.staggerIndex, a.startChanged);
		push4(attr[GeometryBuffers::Direction], a.dirX, a.dirY, a.omega, a.knockB);

		const auto uv = gradient ? gradient(c) : std::pair<float, float>{0.0f, 0.0f};

		push4(attr[GeometryBuffers::Gradient], uv.first, uv.second, a.knockOffsetX, a.knockOffsetY);
	}

	for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u})
		out.indices.push_back(base + i);
}

void shapeAttrs(const Atlas::Shape &s, std::array<float, 4> &band, std::array<float, 4> &shape)
{
	band = {float(s.bandScaleX), float(s.bandScaleY), float(s.bandOffsetX), float(s.bandOffsetY)};
	shape = {float(s.bandTexX), float(s.bandTexY), float(s.bandMaxX), float(s.bandMaxY)};
}

struct EmBox {
	float x0, y0, x1, y1; // y-up
};

EmBox boxOf(const Atlas::Shape &s)
{
	return {float(s.bearingX), float(s.bearingY) - float(s.height), float(s.bearingX) + float(s.width),
		float(s.bearingY)};
}

EmBox unite(const EmBox &a, const EmBox &b)
{
	return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

// The four corners of an em-space box placed at a pen position, grown by
// `margin` em. Layout is y-down while em space is y-up, hence the negation; the
// em coordinates themselves are left unflipped, so each corner carries the true
// em point it covers.
std::array<Corner, 4> glyphCorners(const EmBox &box, float margin, float penX, float baselineY, float size)
{
	const float ex0 = box.x0 - margin;
	const float ey0 = box.y0 - margin;
	const float ex1 = box.x1 + margin;
	const float ey1 = box.y1 + margin;

	auto at = [&](float ex, float ey) { return Corner{penX + ex * size, baselineY - ey * size, ex, ey}; };

	return {at(ex0, ey0), at(ex1, ey0), at(ex1, ey1), at(ex0, ey1)};
}

std::array<Corner, 4> rectCorners(float x0, float y0, float x1, float y1, float u0 = 0.0f, float v0 = 0.0f,
				  float u1 = 0.0f, float v1 = 0.0f)
{
	return {Corner{x0, y0, u0, v0}, Corner{x1, y0, u1, v0}, Corner{x1, y1, u1, v1}, Corner{x0, y1, u0, v1}};
}

// A two-stop fill as per-vertex gradient coordinates. The rasteriser
// interpolates them linearly, which is exact for a linear ramp and gives the
// pixel shader everything it needs for a radial one.
GradientFn screenGradient(const Fill &fill, float bx0, float by0, float bx1, float by1, int &type)
{
	type = 0;

	if (fill.type == FillType::Solid)
		return nullptr;

	if (fill.type == FillType::Radial) {
		type = 2;

		const float cx = (bx0 + bx1) * 0.5f;
		const float cy = (by0 + by1) * 0.5f;
		const float rx = std::max((bx1 - bx0) * 0.5f, 1e-3f);
		const float ry = std::max((by1 - by0) * 0.5f, 1e-3f);

		return [=](const Corner &c) { return std::pair<float, float>{(c.x - cx) / rx, (c.y - cy) / ry}; };
	}

	type = 1;

	// Screen space is y-down, so a positive angle runs clockwise.
	const float rad = fill.angleDeg * kPi / 180.0f;
	const float dx = std::cos(rad);
	const float dy = std::sin(rad);

	float lo = std::numeric_limits<float>::max();
	float hi = std::numeric_limits<float>::lowest();

	for (float x : {bx0, bx1}) {
		for (float y : {by0, by1}) {
			lo = std::min(lo, x * dx + y * dy);
			hi = std::max(hi, x * dx + y * dy);
		}
	}

	const float span = std::max(hi - lo, 1e-3f);

	return [=](const Corner &c) { return std::pair<float, float>{(c.x * dx + c.y * dy - lo) / span, 0.0f}; };
}

// A COLR layer's gradient, defined in the glyph's own em space.
GradientFn layerGradient(const LayerPaint &paint, int &type)
{
	type = 0;

	if (paint.kind == LayerPaint::Kind::Linear) {
		type = 1;

		const float vx = paint.x1 - paint.x0;
		const float vy = paint.y1 - paint.y0;
		const float len2 = std::max(vx * vx + vy * vy, 1e-8f);
		const LayerPaint p = paint;

		return [=](const Corner &c) {
			return std::pair<float, float>{((c.ex - p.x0) * vx + (c.ey - p.y0) * vy) / len2, 0.0f};
		};
	}

	if (paint.kind == LayerPaint::Kind::Radial) {
		type = 2;

		const LayerPaint p = paint;

		return [=](const Corner &c) { return std::pair<float, float>{(c.ex - p.x0) / p.r, (c.ey - p.y0) / p.r}; };
	}

	return nullptr;
}

float staggerIndexOf(uint32_t ordinal, uint32_t word, uint32_t line, MotionOrder order)
{
	switch (order) {
	case MotionOrder::Together:
		return 0.0f;
	case MotionOrder::PerCharacter:
		return float(ordinal);
	case MotionOrder::PerWord:
		return float(word);
	case MotionOrder::PerLine:
		return float(line);
	}

	return 0.0f;
}

void append(GeometryBuffers &dst, const GeometryBuffers &src)
{
	const uint32_t base = uint32_t(dst.vertexCount());

	dst.positions.insert(dst.positions.end(), src.positions.begin(), src.positions.end());

	for (size_t i = 0; i < GeometryBuffers::AttributeCount; i++)
		dst.attributes[i].insert(dst.attributes[i].end(), src.attributes[i].begin(), src.attributes[i].end());

	for (uint32_t index : src.indices)
		dst.indices.push_back(base + index);
}

} // namespace

void GeometryBuffers::clear()
{
	positions.clear();

	for (auto &a : attributes)
		a.clear();

	indices.clear();
}

void GeometryBuffers::translate(float dx, float dy)
{
	if (dx == 0.0f && dy == 0.0f)
		return;

	for (size_t i = 0; i + 1 < positions.size(); i += 4) {
		positions[i] += dx;
		positions[i + 1] += dy;
	}

	auto &pivot = attributes[Pivot];

	for (size_t i = 0; i + 1 < pivot.size(); i += 4) {
		pivot[i] += dx;
		pivot[i + 1] += dy;
	}
}

void InkBounds::add(float ax0, float ay0, float ax1, float ay1)
{
	x0 = std::min(x0, std::min(ax0, ax1));
	y0 = std::min(y0, std::min(ay0, ay1));
	x1 = std::max(x1, std::max(ax0, ax1));
	y1 = std::max(y1, std::max(ay0, ay1));
}

FrameRequests collectRequests(const LayoutResult &layout)
{
	FrameRequests out;
	ShapeCache &cache = ShapeCache::instance();

	std::set<std::string> seen;

	for (const PositionedGlyph &g : layout.glyphs) {
		if (!g.font || !g.style)
			continue;

		const std::string key = fillKey(g.font, g.glyphIndex);
		const bool outline = wantsOutline(*g.style);
		const std::string stroke =
			outline ? strokeKey(g.font, g.glyphIndex, strokeWidthEm(*g.style, g.sizePx), g.style->outline.join)
				: std::string();

		if (seen.count(key) && (!outline || seen.count(stroke)))
			continue;

		seen.insert(key);

		const GlyphShapes &shapes = cache.glyph(g.font, g.glyphIndex);

		if (shapes.hasFill())
			out.shapes.push_back({key, &shapes.fill});

		for (size_t i = 0; i < shapes.layers.size(); i++)
			out.shapes.push_back({layerKey(g.font, g.glyphIndex, i), &shapes.layers[i].shape});

		if (shapes.bitmap)
			out.images.push_back({imageKey(g.font, g.glyphIndex), shapes.bitmap.get()});

		if (outline && !shapes.isColor() && shapes.hasFill() && !seen.count(stroke)) {
			seen.insert(stroke);

			if (const auto *info = cache.stroke(g.font, g.glyphIndex, strokeWidthEm(*g.style, g.sizePx),
							    g.style->outline.join))
				out.shapes.push_back({stroke, info});
		}
	}

	return out;
}

GeometryStats GeometryBuilder::build(const Document &doc, const LayoutResult &layout, const AtlasCache &atlas,
				     const ImageAtlas &images, const GeometryParams &params, GeometryBuffers &out)
{
	out.clear();

	GeometryStats stats;
	ShapeCache &cache = ShapeCache::instance();

	const MotionSpec &motion = doc.motion;

	// Layout is y-down, so "up" is -y.
	float dirX = 0.0f;
	float dirY = -1.0f;

	switch (motion.direction) {
	case MotionDirection::Up:
		break;
	case MotionDirection::Down:
		dirY = 1.0f;
		break;
	case MotionDirection::Left:
		dirX = -1.0f;
		dirY = 0.0f;
		break;
	case MotionDirection::Right:
		dirX = 1.0f;
		dirY = 0.0f;
		break;
	}

	const float omega = motion.speed * 2.0f * kPi;
	const float blurPixels = motion.motion == Motion::Blur ? std::max(motion.param, 0.0f) : 0.0f;

	const bool hasChange = params.changedStart < params.changedEnd;

	auto isChanged = [&](uint32_t offset) {
		return hasChange && offset >= params.changedStart && offset < params.changedEnd;
	};

	// The changed part staggers from its own first unit, so a counter's new
	// digit starts animating immediately rather than after the old ones' delay.
	float firstChanged = std::numeric_limits<float>::max();
	std::set<float> allUnits;
	std::set<float> changedUnits;

	for (const PositionedGlyph &g : layout.glyphs) {
		const float index = staggerIndexOf(g.ordinal, g.wordIndex, g.lineIndex, motion.order);

		allUnits.insert(index);

		if (isChanged(g.textOffset)) {
			firstChanged = std::min(firstChanged, index);
			changedUnits.insert(index);
		}
	}

	stats.units = uint32_t(allUnits.size());
	stats.changedUnits = uint32_t(changedUnits.size());

	auto motionAttrs = [&](QuadAttrs &a, uint32_t ordinal, uint32_t word, uint32_t line, uint32_t offset) {
		const float index = staggerIndexOf(ordinal, word, line, motion.order);

		a.motionMode = float(int(motion.motion));
		a.motionParam = motion.param;
		a.startAll = index * motion.stagger;
		a.duration = std::max(motion.duration, 1e-4f);
		a.staggerIndex = index;
		a.startChanged = isChanged(offset) ? (index - firstChanged) * motion.stagger : -1.0f;
		a.dirX = dirX;
		a.dirY = dirY;
		a.omega = omega;
	};

	// The gradient's box when it spans the whole text.
	const float boxX1 = std::max(layout.width, 1.0f);
	const float boxY1 = std::max(layout.height, 1.0f);

	GeometryBuffers shadows;
	GeometryBuffers outlines;
	GeometryBuffers fills;
	GeometryBuffers bars;

	for (const PositionedGlyph &g : layout.glyphs) {
		if (!g.font || !g.style)
			continue;

		const Style &style = *g.style;
		const GlyphShapes &shapes = cache.glyph(g.font, g.glyphIndex);
		const float size = g.sizePx;
		const float margin = aaMarginEm(size, blurPixels);

		QuadAttrs base;
		motionAttrs(base, g.ordinal, g.wordIndex, g.lineIndex, g.textOffset);

		const auto fillShape = shapes.hasFill() ? atlas.shape(fillKey(g.font, g.glyphIndex)) : std::nullopt;

		std::optional<Atlas::Shape> strokeShape;

		if (!shapes.isColor() && wantsOutline(style) && fillShape)
			strokeShape = atlas.shape(
				strokeKey(g.font, g.glyphIndex, strokeWidthEm(style, size), style.outline.join));

		// Fill alpha decides how much a fill hides what is behind it.
		const float fillAlpha = style.fill.type == FillType::Solid ? style.fill.color.a
									   : std::max(style.fill.color.a, style.fill.color2.a);

		// ---- shadow ----------------------------------------------------
		if (style.shadow.enabled && size > 0.0f) {
			const float sx = style.shadow.offsetX;
			const float sy = style.shadow.offsetY;

			if (fillShape) {
				QuadAttrs a = base;

				a.kind = QuadKind::Shadow;
				a.color = style.shadow.color;
				shapeAttrs(*fillShape, a.aBand, a.aShape);

				EmBox box = boxOf(*fillShape);

				if (strokeShape) {
					shapeAttrs(*strokeShape, a.bBand, a.bShape);
					box = unite(box, boxOf(*strokeShape));
					a.knockB = style.outline.color.a;
				}

				// The glyph in front sits (sx, sy) pixels back from the
				// shadow, which in em space is (sx, -sy) / size.
				a.knockA = shapes.isColor() ? 1.0f : fillAlpha;
				a.knockOffsetX = sx / size;
				a.knockOffsetY = -sy / size;

				const auto corners = glyphCorners(box, margin, g.x + sx, g.y + sy, size);

				emit(shadows, corners, a, nullptr);
				stats.ink.add(g.x + sx + box.x0 * size, g.y + sy - box.y1 * size, g.x + sx + box.x1 * size,
					      g.y + sy - box.y0 * size);
			} else if (shapes.bitmap) {
				if (const auto slot = images.slot(imageKey(g.font, g.glyphIndex))) {
					const BitmapGlyph &bmp = *shapes.bitmap;
					const float x0 = g.x + sx + bmp.left * size;
					const float y0 = g.y + sy - bmp.top * size;

					QuadAttrs a = base;
					a.kind = QuadKind::ImageTint;
					a.color = style.shadow.color;

					emit(shadows,
					     rectCorners(x0, y0, x0 + bmp.emWidth * size, y0 + bmp.emHeight * size, slot->u0,
							 slot->v0, slot->u1, slot->v1),
					     a, nullptr);
				}
			}
		}

		// ---- outline -----------------------------------------------------
		if (strokeShape && fillShape) {
			QuadAttrs a = base;

			a.kind = QuadKind::Outline;
			a.color = style.outline.color;
			shapeAttrs(*fillShape, a.aBand, a.aShape);
			shapeAttrs(*strokeShape, a.bBand, a.bShape);
			a.knockA = fillAlpha;

			const EmBox box = boxOf(*strokeShape);

			emit(outlines, glyphCorners(box, margin, g.x, g.y, size), a, nullptr);
			stats.ink.add(g.x + box.x0 * size, g.y - box.y1 * size, g.x + box.x1 * size, g.y - box.y0 * size);
		}

		// ---- fill --------------------------------------------------------
		if (!shapes.layers.empty()) {
			for (size_t i = 0; i < shapes.layers.size(); i++) {
				const auto layer = atlas.shape(layerKey(g.font, g.glyphIndex, i));

				if (!layer)
					continue;

				const LayerPaint &paint = shapes.layers[i].paint;

				QuadAttrs a = base;
				a.kind = QuadKind::Fill;
				shapeAttrs(*layer, a.aBand, a.aShape);

				const EmBox box = boxOf(*layer);
				GradientFn gradient;

				if (paint.kind == LayerPaint::Kind::Foreground) {
					a.color = style.fill.color;
					a.color2 = style.fill.color2;
					gradient = screenGradient(style.fill, 0.0f, 0.0f, boxX1, boxY1, a.gradientType);
				} else {
					a.color = paint.color;
					a.color2 = paint.color2;
					gradient = layerGradient(paint, a.gradientType);
				}

				emit(fills, glyphCorners(box, margin, g.x, g.y, size), a, gradient);
				stats.ink.add(g.x + box.x0 * size, g.y - box.y1 * size, g.x + box.x1 * size,
					      g.y - box.y0 * size);
			}
		} else if (shapes.bitmap) {
			if (const auto slot = images.slot(imageKey(g.font, g.glyphIndex))) {
				const BitmapGlyph &bmp = *shapes.bitmap;
				const float x0 = g.x + bmp.left * size;
				const float y0 = g.y - bmp.top * size;
				const float x1 = x0 + bmp.emWidth * size;
				const float y1 = y0 + bmp.emHeight * size;

				QuadAttrs a = base;
				a.kind = QuadKind::Image;
				a.color = Rgba{1.0f, 1.0f, 1.0f, 1.0f};

				emit(fills, rectCorners(x0, y0, x1, y1, slot->u0, slot->v0, slot->u1, slot->v1), a, nullptr);
				stats.ink.add(x0, y0, x1, y1);
			}
		} else if (fillShape) {
			QuadAttrs a = base;

			a.kind = QuadKind::Fill;
			a.color = style.fill.color;
			a.color2 = style.fill.color2;
			shapeAttrs(*fillShape, a.aBand, a.aShape);

			const EmBox box = boxOf(*fillShape);
			GradientFn gradient;

			if (style.fill.span == GradientSpan::Character)
				gradient = screenGradient(style.fill, g.x + box.x0 * size, g.y - box.y1 * size,
							  g.x + box.x1 * size, g.y - box.y0 * size, a.gradientType);
			else
				gradient = screenGradient(style.fill, 0.0f, 0.0f, boxX1, boxY1, a.gradientType);

			emit(fills, glyphCorners(box, margin, g.x, g.y, size), a, gradient);
			stats.ink.add(g.x + box.x0 * size, g.y - box.y1 * size, g.x + box.x1 * size, g.y - box.y0 * size);
		}
	}

	// ---- underline and strikeout ----------------------------------------
	for (const DecorationRect &d : layout.decorations) {
		if (!d.style || d.width <= 0.0f || d.height <= 0.0f)
			continue;

		QuadAttrs base;
		motionAttrs(base, d.ordinal, d.wordIndex, d.lineIndex, d.textOffset);

		const float x1 = d.x + d.width;
		const float y1 = d.y + d.height;

		if (d.style->shadow.enabled) {
			QuadAttrs a = base;

			a.kind = QuadKind::Solid;
			a.color = d.style->shadow.color;

			const float sx = d.style->shadow.offsetX;
			const float sy = d.style->shadow.offsetY;

			emit(shadows, rectCorners(d.x + sx, d.y + sy, x1 + sx, y1 + sy), a, nullptr);
			stats.ink.add(d.x + sx, d.y + sy, x1 + sx, y1 + sy);
		}

		QuadAttrs a = base;

		a.kind = QuadKind::Solid;
		a.color = d.style->fill.color;
		a.color2 = d.style->fill.color2;

		const GradientFn gradient =
			d.style->fill.span == GradientSpan::Character
				? screenGradient(d.style->fill, d.x, d.y, x1, y1, a.gradientType)
				: screenGradient(d.style->fill, 0.0f, 0.0f, boxX1, boxY1, a.gradientType);

		emit(bars, rectCorners(d.x, d.y, x1, y1), a, gradient);
		stats.ink.add(d.x, d.y, x1, y1);
	}

	append(out, shadows);
	append(out, outlines);
	append(out, fills);
	append(out, bars);

	return stats;
}

void GeometryBuilder::buildBackground(const Background &background, float width, float height, GeometryBuffers &out)
{
	out.clear();

	if (!background.enabled || width <= 0.0f || height <= 0.0f || background.color.a <= 0.0f)
		return;

	const float hw = width * 0.5f;
	const float hh = height * 0.5f;

	QuadAttrs a;

	a.kind = QuadKind::RoundRect;
	a.color = background.color;
	a.aBand = {hw, hh, std::clamp(background.radius, 0.0f, std::min(hw, hh)), 0.0f};

	// Em carries the pixel position relative to the centre, which is what the
	// rounded-rectangle distance function wants.
	emit(out, rectCorners(0.0f, 0.0f, width, height, -hw, -hh, hw, hh), a, nullptr);
}

} // namespace slugged
