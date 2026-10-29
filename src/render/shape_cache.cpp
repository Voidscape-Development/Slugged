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

#include "shape_cache.hpp"
#include "../text/font_manager.hpp"
#include "../util/log.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include "slughorn/canvas.hpp"

#include FT_OUTLINE_H
#include FT_COLOR_H

namespace slugged {

namespace {

// Past this many cached glyphs the cache starts over. A long-running chat log in
// a dozen scripts could otherwise grow it without bound.
constexpr size_t kMaxGlyphs = 20000;
constexpr size_t kMaxStrokes = 20000;

using slughorn::Atlas;

struct Decompose {
	slughorn::CurveDecomposer decomposer;
	std::vector<size_t> *starts;
	float scale;

	Decompose(Atlas::Curves &curves, std::vector<size_t> &s, float k) : decomposer(curves), starts(&s), scale(k) {}
};

int moveTo(const FT_Vector *to, void *user)
{
	auto *d = static_cast<Decompose *>(user);

	d->starts->push_back(d->decomposer.curves.size());
	d->decomposer.moveTo(float(to->x) * d->scale, float(to->y) * d->scale);

	return 0;
}

int lineTo(const FT_Vector *to, void *user)
{
	auto *d = static_cast<Decompose *>(user);

	d->decomposer.lineTo(float(to->x) * d->scale, float(to->y) * d->scale);

	return 0;
}

int conicTo(const FT_Vector *c, const FT_Vector *to, void *user)
{
	auto *d = static_cast<Decompose *>(user);

	d->decomposer.quadTo(float(c->x) * d->scale, float(c->y) * d->scale, float(to->x) * d->scale,
			     float(to->y) * d->scale);

	return 0;
}

int cubicTo(const FT_Vector *c1, const FT_Vector *c2, const FT_Vector *to, void *user)
{
	auto *d = static_cast<Decompose *>(user);

	d->decomposer.cubicTo(float(c1->x) * d->scale, float(c1->y) * d->scale, float(c2->x) * d->scale,
			      float(c2->y) * d->scale, float(to->x) * d->scale, float(to->y) * d->scale);

	return 0;
}

// Loads `glyphIndex` as an unscaled outline and decomposes it into em-space
// quadratic curves with exact contour boundaries.
bool loadOutline(FontInstance *font, uint32_t glyphIndex, Atlas::ShapeInfo &out)
{
	FT_Face face = font->file->ft;

	font->select();

	if (FT_Load_Glyph(face, glyphIndex, FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP))
		return false;

	if (face->glyph->format != FT_GLYPH_FORMAT_OUTLINE || face->glyph->outline.n_points <= 0)
		return false;

	Decompose d(out.curves, out.contourStarts, 1.0f / font->file->unitsPerEm);

	const FT_Outline_Funcs funcs = {moveTo, lineTo, conicTo, cubicTo, 0, 0};

	try {
		FT_Outline_Decompose(&face->glyph->outline, &funcs, &d);
	} catch (const std::exception &e) {
		slugged_log(LOG_WARNING, "glyph %u has an invalid outline: %s", glyphIndex, e.what());
		out.curves.clear();
		out.contourStarts.clear();
		return false;
	}

	out.autoMetrics = true;

	return !out.curves.empty();
}

void transformShape(Atlas::ShapeInfo &shape, const slughorn::Matrix &m)
{
	if (m.isIdentity())
		return;

	for (Atlas::Curve &c : shape.curves) {
		m.apply(c.x1, c.y1, c.x1, c.y1);
		m.apply(c.x2, c.y2, c.x2, c.y2);
		m.apply(c.x3, c.y3, c.x3, c.y3);
	}
}

// ---- colour layers -------------------------------------------------------------

Rgba paletteColor(FT_Color *palette, uint16_t count, uint32_t index, float alpha, bool &foreground)
{
	foreground = index == 0xFFFF;

	if (foreground || !palette || index >= count)
		return Rgba{1.0f, 1.0f, 1.0f, alpha};

	const FT_Color &c = palette[index];

	return Rgba{float(c.red) / 255.0f, float(c.green) / 255.0f, float(c.blue) / 255.0f,
		    float(c.alpha) / 255.0f * alpha};
}

struct ColrContext {
	FontInstance *font;
	FT_Face face;
	FT_Color *palette = nullptr;
	uint16_t paletteSize = 0;
	float emScale = 1.0f;
	std::vector<ColorLayer> *layers;
	int depth = 0;
};

void loadColrV0(ColrContext &ctx, uint32_t glyphIndex)
{
	FT_LayerIterator iterator = {};
	FT_UInt layerGlyph = 0;
	FT_UInt colorIndex = 0;

	while (FT_Get_Color_Glyph_Layer(ctx.face, glyphIndex, &layerGlyph, &colorIndex, &iterator)) {
		ColorLayer layer;

		if (!loadOutline(ctx.font, layerGlyph, layer.shape))
			continue;

		bool foreground = false;

		layer.paint.color = paletteColor(ctx.palette, ctx.paletteSize, colorIndex, 1.0f, foreground);
		layer.paint.kind = foreground ? LayerPaint::Kind::Foreground : LayerPaint::Kind::Solid;

		ctx.layers->push_back(std::move(layer));
	}
}

#if FREETYPE_MAJOR > 2 || (FREETYPE_MAJOR == 2 && FREETYPE_MINOR >= 11)

// A COLRv1 paint resolved down to what a layer can carry: a flat colour or a
// two-stop gradient. Multi-stop gradients keep their first and last stops.
bool resolveFill(ColrContext &ctx, const FT_COLR_Paint &paint, const slughorn::Matrix &m, LayerPaint &out)
{
	auto stops = [&](FT_ColorStopIterator iterator, Rgba &first, Rgba &last) {
		FT_ColorStop stop;
		bool any = false;

		while (FT_Get_Colorline_Stops(ctx.face, &stop, &iterator)) {
			bool foreground = false;
			const Rgba c = paletteColor(ctx.palette, ctx.paletteSize, stop.color.palette_index,
						    float(stop.color.alpha) / 16384.0f, foreground);

			if (!any)
				first = c;

			last = c;
			any = true;
		}

		return any;
	};

	const float k = ctx.emScale / 65536.0f;

	switch (paint.format) {
	case FT_COLR_PAINTFORMAT_SOLID: {
		bool foreground = false;

		out.color = paletteColor(ctx.palette, ctx.paletteSize, paint.u.solid.color.palette_index,
					 float(paint.u.solid.color.alpha) / 16384.0f, foreground);
		out.kind = foreground ? LayerPaint::Kind::Foreground : LayerPaint::Kind::Solid;
		return true;
	}
	case FT_COLR_PAINTFORMAT_LINEAR_GRADIENT: {
		const auto &g = paint.u.linear_gradient;

		if (!stops(g.colorline.color_stop_iterator, out.color, out.color2))
			return false;

		out.kind = LayerPaint::Kind::Linear;
		m.apply(float(g.p0.x) * k, float(g.p0.y) * k, out.x0, out.y0);
		m.apply(float(g.p1.x) * k, float(g.p1.y) * k, out.x1, out.y1);
		return true;
	}
	case FT_COLR_PAINTFORMAT_RADIAL_GRADIENT: {
		const auto &g = paint.u.radial_gradient;

		if (!stops(g.colorline.color_stop_iterator, out.color, out.color2))
			return false;

		out.kind = LayerPaint::Kind::Radial;
		m.apply(float(g.c1.x) * k, float(g.c1.y) * k, out.x0, out.y0);
		out.r = std::max(float(g.r1) * k * std::sqrt(std::fabs(m.xx * m.yy - m.xy * m.yx)), 1e-4f);
		return true;
	}
	case FT_COLR_PAINTFORMAT_SWEEP_GRADIENT: {
		const auto &g = paint.u.sweep_gradient;
		Rgba first, last;

		if (!stops(g.colorline.color_stop_iterator, first, last))
			return false;

		// A sweep has no two-stop equivalent; its average reads closest.
		out.kind = LayerPaint::Kind::Solid;
		out.color = Rgba{(first.r + last.r) * 0.5f, (first.g + last.g) * 0.5f, (first.b + last.b) * 0.5f,
				 (first.a + last.a) * 0.5f};
		return true;
	}
	default:
		return false;
	}
}

void walkPaint(ColrContext &ctx, FT_OpaquePaint opaque, const slughorn::Matrix &m)
{
	// Malformed fonts can contain paint cycles.
	if (++ctx.depth > 64) {
		ctx.depth--;
		return;
	}

	FT_COLR_Paint paint;

	if (!FT_Get_Paint(ctx.face, opaque, &paint)) {
		ctx.depth--;
		return;
	}

	const float k = ctx.emScale / 65536.0f;

	switch (paint.format) {
	case FT_COLR_PAINTFORMAT_COLR_LAYERS: {
		FT_OpaquePaint child = {};

		while (FT_Get_Paint_Layers(ctx.face, &paint.u.colr_layers.layer_iterator, &child))
			walkPaint(ctx, child, m);

		break;
	}
	case FT_COLR_PAINTFORMAT_GLYPH: {
		FT_COLR_Paint fill;
		LayerPaint resolved;

		if (FT_Get_Paint(ctx.face, paint.u.glyph.paint, &fill)) {
			// Transforms between a glyph and its fill move only the fill.
			slughorn::Matrix fillMatrix = m;
			FT_COLR_Paint inner = fill;

			for (int guard = 0; guard < 8; guard++) {
				if (inner.format == FT_COLR_PAINTFORMAT_TRANSFORM) {
					const FT_Affine23 &a = inner.u.transform.affine;
					fillMatrix = fillMatrix *
						     slughorn::Matrix{float(a.xx) / 65536.0f, float(a.yx) / 65536.0f,
								      float(a.xy) / 65536.0f, float(a.yy) / 65536.0f,
								      float(a.dx) * k,        float(a.dy) * k};

					if (!FT_Get_Paint(ctx.face, inner.u.transform.paint, &inner))
						break;
				} else if (inner.format == FT_COLR_PAINTFORMAT_TRANSLATE) {
					fillMatrix = fillMatrix *
						     slughorn::Matrix::translate(float(inner.u.translate.dx) * k,
										 float(inner.u.translate.dy) * k);

					if (!FT_Get_Paint(ctx.face, inner.u.translate.paint, &inner))
						break;
				} else {
					break;
				}
			}

			if (!resolveFill(ctx, inner, fillMatrix, resolved))
				resolved.color = Rgba{0.5f, 0.5f, 0.5f, 1.0f};
		}

		ColorLayer layer;

		if (loadOutline(ctx.font, paint.u.glyph.glyphID, layer.shape)) {
			transformShape(layer.shape, m);
			layer.paint = resolved;
			ctx.layers->push_back(std::move(layer));
		}

		break;
	}
	case FT_COLR_PAINTFORMAT_COLR_GLYPH: {
		FT_OpaquePaint child = {};

		if (FT_Get_Color_Glyph_Paint(ctx.face, paint.u.colr_glyph.glyphID, FT_COLOR_NO_ROOT_TRANSFORM, &child))
			walkPaint(ctx, child, m);

		break;
	}
	case FT_COLR_PAINTFORMAT_TRANSFORM: {
		const FT_Affine23 &a = paint.u.transform.affine;

		// FreeType's root transform is occasionally all zero; skip it.
		if (!a.xx && !a.xy && !a.yx && !a.yy) {
			walkPaint(ctx, paint.u.transform.paint, m);
			break;
		}

		const slughorn::Matrix t{float(a.xx) / 65536.0f, float(a.yx) / 65536.0f, float(a.xy) / 65536.0f,
					 float(a.yy) / 65536.0f, float(a.dx) * k,        float(a.dy) * k};

		walkPaint(ctx, paint.u.transform.paint, m * t);
		break;
	}
	case FT_COLR_PAINTFORMAT_TRANSLATE:
		walkPaint(ctx, paint.u.translate.paint,
			  m * slughorn::Matrix::translate(float(paint.u.translate.dx) * k,
							  float(paint.u.translate.dy) * k));
		break;
	case FT_COLR_PAINTFORMAT_SCALE: {
		const auto &s = paint.u.scale;
		const float sx = float(s.scale_x) / 65536.0f;
		const float sy = float(s.scale_y) / 65536.0f;
		const float cx = float(s.center_x) * k;
		const float cy = float(s.center_y) * k;

		walkPaint(ctx, s.paint, m * slughorn::Matrix{sx, 0.0f, 0.0f, sy, cx * (1.0f - sx), cy * (1.0f - sy)});
		break;
	}
	case FT_COLR_PAINTFORMAT_ROTATE: {
		const auto &r = paint.u.rotate;
		const float theta = float(r.angle) / 65536.0f * 3.14159265f;
		const float c = std::cos(theta);
		const float s = std::sin(theta);
		const float cx = float(r.center_x) * k;
		const float cy = float(r.center_y) * k;

		walkPaint(ctx, r.paint,
			  m * slughorn::Matrix{c, s, -s, c, cx * (1.0f - c) + s * cy, cy * (1.0f - c) - s * cx});
		break;
	}
	case FT_COLR_PAINTFORMAT_SKEW: {
		const auto &sk = paint.u.skew;
		const float tx = std::tan(float(sk.x_skew_angle) / 65536.0f * 3.14159265f);
		const float ty = std::tan(float(sk.y_skew_angle) / 65536.0f * 3.14159265f);
		const float cx = float(sk.center_x) * k;
		const float cy = float(sk.center_y) * k;

		walkPaint(ctx, sk.paint, m * slughorn::Matrix{1.0f, ty, tx, 1.0f, -tx * cy, -ty * cx});
		break;
	}
	case FT_COLR_PAINTFORMAT_COMPOSITE:
		// Blend modes are not supported; source over backdrop is the common
		// case and reads correctly for nearly every emoji.
		walkPaint(ctx, paint.u.composite.backdrop_paint, m);
		walkPaint(ctx, paint.u.composite.source_paint, m);
		break;
	default:
		break;
	}

	ctx.depth--;
}

bool loadColrV1(ColrContext &ctx, uint32_t glyphIndex)
{
	FT_OpaquePaint root = {};

	if (!FT_Get_Color_Glyph_Paint(ctx.face, glyphIndex, FT_COLOR_INCLUDE_ROOT_TRANSFORM, &root))
		return false;

	walkPaint(ctx, root, slughorn::Matrix::identity());

	return true;
}

#endif

void loadColorLayers(FontInstance *font, uint32_t glyphIndex, std::vector<ColorLayer> &layers)
{
	ColrContext ctx;

	ctx.font = font;
	ctx.face = font->file->ft;
	ctx.emScale = 1.0f / font->file->unitsPerEm;
	ctx.layers = &layers;

	FT_Palette_Data paletteData = {};

	if (!FT_Palette_Data_Get(ctx.face, &paletteData) && paletteData.num_palettes > 0 &&
	    !FT_Palette_Select(ctx.face, 0, &ctx.palette))
		ctx.paletteSize = paletteData.num_palette_entries;

#if FREETYPE_MAJOR > 2 || (FREETYPE_MAJOR == 2 && FREETYPE_MINOR >= 11)
	if (loadColrV1(ctx, glyphIndex) && !layers.empty())
		return;

	layers.clear();
#endif

	loadColrV0(ctx, glyphIndex);
}

// ---- colour bitmaps ------------------------------------------------------------

std::unique_ptr<BitmapGlyph> loadBitmap(FontInstance *font, uint32_t glyphIndex)
{
	FT_Face face = font->file->ft;

	if (face->num_fixed_sizes <= 0)
		return nullptr;

	// The largest strike gives the sharpest result once scaled.
	int best = 0;

	for (int i = 1; i < face->num_fixed_sizes; i++)
		if (face->available_sizes[i].y_ppem > face->available_sizes[best].y_ppem)
			best = i;

	if (FT_Select_Size(face, best))
		return nullptr;

	if (FT_Load_Glyph(face, glyphIndex, FT_LOAD_COLOR) || face->glyph->bitmap.pixel_mode != FT_PIXEL_MODE_BGRA)
		return nullptr;

	const FT_Bitmap &bmp = face->glyph->bitmap;
	const float ppem = float(face->size->metrics.y_ppem > 0 ? face->size->metrics.y_ppem : 1);

	auto out = std::make_unique<BitmapGlyph>();

	out->width = int(bmp.width);
	out->height = int(bmp.rows);
	out->rgba.resize(size_t(out->width) * size_t(out->height) * 4);

	for (int y = 0; y < out->height; y++) {
		const uint8_t *row = bmp.buffer + ptrdiff_t(y) * bmp.pitch;

		for (int x = 0; x < out->width; x++) {
			const uint8_t b = row[x * 4 + 0];
			const uint8_t g = row[x * 4 + 1];
			const uint8_t r = row[x * 4 + 2];
			const uint8_t a = row[x * 4 + 3];

			uint8_t *dst = &out->rgba[(size_t(y) * size_t(out->width) + size_t(x)) * 4];

			// FreeType hands over premultiplied BGRA.
			dst[0] = a ? uint8_t(std::min(255, r * 255 / a)) : 0;
			dst[1] = a ? uint8_t(std::min(255, g * 255 / a)) : 0;
			dst[2] = a ? uint8_t(std::min(255, b * 255 / a)) : 0;
			dst[3] = a;
		}
	}

	// Bitmap strikes are designed for their own ppem; scale them to sit on
	// the font's em like an outline glyph would.
	out->left = float(face->glyph->bitmap_left) / ppem;
	out->top = float(face->glyph->bitmap_top) / ppem;
	out->emWidth = float(out->width) / ppem;
	out->emHeight = float(out->height) / ppem;

	return out->width > 0 && out->height > 0 ? std::move(out) : nullptr;
}

} // namespace

ShapeCache &ShapeCache::instance()
{
	static ShapeCache cache;

	return cache;
}

float ShapeCache::bucketWidth(float widthEm)
{
	// 1/512 em: finer than any visible difference, coarse enough that a slider
	// drag reuses shapes.
	return std::round(widthEm * 512.0f) / 512.0f;
}

const GlyphShapes &ShapeCache::glyph(FontInstance *font, uint32_t glyphIndex)
{
	static const GlyphShapes empty;

	if (!font)
		return empty;

	const GlyphKey key{font->id, glyphIndex};
	auto it = _glyphs.find(key);

	if (it != _glyphs.end())
		return it->second;

	GlyphShapes shapes;

	if (font->file->hasColorLayers)
		loadColorLayers(font, glyphIndex, shapes.layers);

	if (shapes.layers.empty() && !loadOutline(font, glyphIndex, shapes.fill) && font->file->hasColorBitmaps)
		shapes.bitmap = loadBitmap(font, glyphIndex);

	// A colour glyph usually has a monochrome base outline too; keep it, since
	// it is what the shadow is cast from.
	if (!shapes.layers.empty())
		loadOutline(font, glyphIndex, shapes.fill);

	return _glyphs.emplace(key, std::move(shapes)).first->second;
}

const Atlas::ShapeInfo *ShapeCache::stroke(FontInstance *font, uint32_t glyphIndex, float widthEm, LineJoin join)
{
	if (!font)
		return nullptr;

	const float bucket = bucketWidth(widthEm);

	if (bucket <= 0.0f)
		return nullptr;

	const StrokeKey key{font->id, glyphIndex, int(std::lround(bucket * 512.0f)), int(join)};

	auto it = _strokes.find(key);

	if (it != _strokes.end())
		return it->second.get();

	std::unique_ptr<Atlas::ShapeInfo> result;

	const GlyphShapes &shapes = glyph(font, glyphIndex);

	if (shapes.hasFill()) {
		// Build the path contour by contour, so the stroker knows exactly
		// where each closed subpath starts instead of guessing from gaps.
		slughorn::canvas::Path path;

		const auto &curves = shapes.fill.curves;
		const auto &starts = shapes.fill.contourStarts;

		for (size_t c = 0; c < starts.size(); c++) {
			const size_t begin = starts[c];
			const size_t end = c + 1 < starts.size() ? starts[c + 1] : curves.size();

			if (begin >= end)
				continue;

			path.moveTo(curves[begin].x1, curves[begin].y1);

			for (size_t i = begin; i < end; i++)
				path.quadTo(curves[i].x2, curves[i].y2, curves[i].x3, curves[i].y3);

			path.closePath();
		}

		slughorn::canvas::LineJoin slugJoin = slughorn::canvas::LineJoin::Round;

		if (join == LineJoin::Miter)
			slugJoin = slughorn::canvas::LineJoin::Miter;
		else if (join == LineJoin::Bevel)
			slugJoin = slughorn::canvas::LineJoin::Bevel;

		try {
			if (path.strokePath(bucket, false, slugJoin, slughorn::canvas::LineCap::Butt, 4.0f)) {
				// Committed through a scratch atlas, which is the only way to
				// read the stroker's output back. Auto-metrics stay off so the
				// curves keep their true em coordinates.
				Atlas scratch;
				slughorn::canvas::Canvas canvas(scratch);

				canvas.setAutoMetrics(false);

				if (canvas.defineShape(path, "stroke")) {
					const auto shape = scratch.getShape("stroke");

					if (shape && !shape->curves.empty()) {
						result = std::make_unique<Atlas::ShapeInfo>();
						result->curves = shape->curves;
						result->contourStarts = shape->contourStarts;
						result->autoMetrics = true;
					}
				}
			}
		} catch (const std::exception &e) {
			slugged_log(LOG_WARNING, "could not stroke glyph %u: %s", glyphIndex, e.what());
			result.reset();
		}
	}

	return _strokes.emplace(key, std::move(result)).first->second.get();
}

void ShapeCache::evict(const std::vector<uint32_t> &instanceIds)
{
	if (!instanceIds.empty()) {
		const std::set<uint32_t> ids(instanceIds.begin(), instanceIds.end());

		for (auto it = _glyphs.begin(); it != _glyphs.end();)
			it = ids.count(it->first.first) ? _glyphs.erase(it) : std::next(it);

		for (auto it = _strokes.begin(); it != _strokes.end();)
			it = ids.count(std::get<0>(it->first)) ? _strokes.erase(it) : std::next(it);
	}

	trim();
}

void ShapeCache::trim()
{
	if (_glyphs.size() > kMaxGlyphs)
		_glyphs.clear();

	if (_strokes.size() > kMaxStrokes)
		_strokes.clear();
}

void ShapeCache::clear()
{
	_glyphs.clear();
	_strokes.clear();
}

} // namespace slugged
