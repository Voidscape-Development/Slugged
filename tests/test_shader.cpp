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

// Runs the coverage solve from data/effects/slugged.effect on the CPU, against
// a real packed atlas, and checks it against a ground-truth winding test.
//
// The shader cannot run here, but its data plumbing is where porting mistakes
// hide: the texture format the band data arrives in, the UNORM decode, and how
// band lists and curves that wrap onto the next texture row are re-derived.
// This mirrors that logic line for line (in float, as the shader does) so a
// slughorn packing change that the shader does not follow shows up as a test
// failure rather than as broken glyphs on stream.

#include "harness.hpp"

#include "core/document.hpp"
#include "render/frame.hpp"
#include "text/font_enum.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace slugged;

namespace {

constexpr float kIndirection = 32.0f;
constexpr int kMaxBandCurves = 128;

struct Textures {
	const slughorn::Atlas::TextureData *curve;
	const slughorn::Atlas::TextureData *band;
};

struct Float4 {
	float x, y, z, w;
};

Float4 fetchCurve(const Textures &t, float x, float y)
{
	const uint32_t ix = uint32_t(x);
	const uint32_t iy = uint32_t(y);

	if (ix >= t.curve->width || iy >= t.curve->height)
		return {0, 0, 0, 0};

	Float4 v;
	std::memcpy(&v, &t.curve->bytes[(size_t(iy) * t.curve->width + ix) * 16], 16);
	return v;
}

// What the shader reads: the texel as UNORM, multiplied back up and rounded.
Float4 fetchBand(const Textures &t, float x, float y)
{
	const uint32_t ix = uint32_t(x);
	const uint32_t iy = uint32_t(y);
	const bool rg = t.band->format == slughorn::Atlas::TextureData::Format::RG16UI;
	const size_t channels = rg ? 2 : 4;

	if (ix >= t.band->width || iy >= t.band->height)
		return {0, 0, 0, 0};

	uint16_t raw[4] = {0, 0, 0, 0};
	std::memcpy(raw, &t.band->bytes[(size_t(iy) * t.band->width + ix) * channels * 2], channels * 2);

	auto decode = [](uint16_t v) {
		return std::floor(float(v) / 65535.0f * 65535.0f + 0.5f);
	};

	return {decode(raw[0]), decode(raw[1]), decode(raw[2]), decode(raw[3])};
}

void bandLoc(const Textures &t, float gx, float gy, float offset, float &x, float &y)
{
	const float w = float(t.band->width);
	const float px = gx + offset;
	const float row = std::floor(px / w);

	x = px - row * w;
	y = gy + row;
}

void curveLoc2(const Textures &t, float cx, float cy, float &x, float &y)
{
	const float w = float(t.curve->width);
	const float px = cx + 1.0f;
	const float row = std::floor(px / w);

	x = px - row * w;
	y = cy + row;
}

void rootFlags(float y1, float y2, float y3, float &r1, float &r2)
{
	const bool s1 = y1 < 0.0f, s2 = y2 < 0.0f, s3 = y3 < 0.0f;

	r1 = ((!s1 && s2) || (!s2 && s3)) ? 1.0f : 0.0f;
	r2 = ((!s3 && (s1 || s2)) || (s1 && !s2 && s3)) ? 1.0f : 0.0f;
}

void solveRoots(float a, float b, float c, float &t1, float &t2)
{
	const float disc = b * b - a * c;
	const float d = std::sqrt(std::max(disc, 0.0f));
	const float q = b >= 0.0f ? b + d : b - d;
	const float tFar = q / a;
	const float tNear = disc > 0.0f ? c / q : tFar;

	t1 = b >= 0.0f ? tNear : tFar;
	t2 = b >= 0.0f ? tFar : tNear;
}

// slug_SolveHorizPoly / slug_SolveVertPoly, selected by `axis`.
void solvePoly(const Float4 &p12, float p3x, float p3y, bool vertical, float &r0, float &r1)
{
	const float ax = p12.x - p12.z * 2.0f + p3x, ay = p12.y - p12.w * 2.0f + p3y;
	const float bx = p12.x - p12.z, by = p12.y - p12.w;

	const float a = vertical ? ax : ay;
	const float b = vertical ? bx : by;
	const float c = vertical ? p12.x : p12.y;

	float t1, t2;
	solveRoots(a, b, c, t1, t2);

	if (std::fabs(a) < 1.0f / 65536.0f)
		t1 = t2 = c * (0.5f / b);

	const float oa = vertical ? ay : ax;
	const float ob = vertical ? by : bx;
	const float oc = vertical ? p12.y : p12.x;

	r0 = (oa * t1 - ob * 2.0f) * t1 + oc;
	r1 = (oa * t2 - ob * 2.0f) * t2 + oc;
}

float coverage(const Textures &t, const slughorn::Atlas::Shape &s, float ex, float ey, float ppe)
{
	const float gx = float(s.bandTexX), gy = float(s.bandTexY);

	const float qY = std::clamp(std::floor(ey * s.bandScaleY + s.bandOffsetY), 0.0f, kIndirection - 1.0f);
	const float qX = std::clamp(std::floor(ex * s.bandScaleX + s.bandOffsetX), 0.0f, kIndirection - 1.0f);

	const float bandY = fetchBand(t, gx + qY, gy).x;
	const float bandX = fetchBand(t, gx + kIndirection + qX, gy).x;

	float cov[2] = {0, 0};
	float wgt[2] = {0, 0};

	for (int pass = 0; pass < 2; pass++) {
		const bool vertical = pass == 1;
		const Float4 header =
			vertical ? fetchBand(t, gx + 2.0f * kIndirection + float(s.bandMaxY) + 1.0f + bandX, gy)
				 : fetchBand(t, gx + 2.0f * kIndirection + bandY, gy);

		for (int i = 0; i < kMaxBandCurves; i++) {
			if (float(i) >= header.x)
				break;

			float lx, ly;
			bandLoc(t, gx, gy, header.y + float(i), lx, ly);

			const Float4 loc = fetchBand(t, lx, ly);

			Float4 p12 = fetchCurve(t, loc.x, loc.y);
			float cx, cy;
			curveLoc2(t, loc.x, loc.y, cx, cy);
			const Float4 p3v = fetchCurve(t, cx, cy);

			p12 = {p12.x - ex, p12.y - ey, p12.z - ex, p12.w - ey};
			const float p3x = p3v.x - ex, p3y = p3v.y - ey;

			if (!vertical && std::max(std::max(p12.x, p12.z), p3x) * ppe < -0.5f)
				break;

			if (vertical && std::max(std::max(p12.y, p12.w), p3y) * ppe < -0.5f)
				break;

			float f1, f2;

			if (vertical)
				rootFlags(p12.x, p12.z, p3x, f1, f2);
			else
				rootFlags(p12.y, p12.w, p3y, f1, f2);

			if (f1 + f2 <= 0.0f)
				continue;

			float r0, r1;
			solvePoly(p12, p3x, p3y, vertical, r0, r1);
			r0 *= ppe;
			r1 *= ppe;

			const float sign = vertical ? -1.0f : 1.0f;

			cov[pass] += sign * f1 * std::clamp(r0 + 0.5f, 0.0f, 1.0f);
			wgt[pass] = std::max(wgt[pass], f1 * std::clamp(1.0f - std::fabs(r0) * 2.0f, 0.0f, 1.0f));
			cov[pass] -= sign * f2 * std::clamp(r1 + 0.5f, 0.0f, 1.0f);
			wgt[pass] = std::max(wgt[pass], f2 * std::clamp(1.0f - std::fabs(r1) * 2.0f, 0.0f, 1.0f));
		}
	}

	const float c =
		std::max(std::fabs(cov[0] * wgt[0] + cov[1] * wgt[1]) / std::max(wgt[0] + wgt[1], 1.0f / 65536.0f),
			 std::min(std::fabs(cov[0]), std::fabs(cov[1])));

	return std::clamp(c, 0.0f, 1.0f);
}

// Nonzero winding of the original curves around a point, by flattening.
int winding(const slughorn::Atlas::Curves &curves, float px, float py)
{
	int w = 0;

	for (const auto &c : curves) {
		float x0 = c.x1, y0 = c.y1;

		for (int i = 1; i <= 16; i++) {
			const float t = float(i) / 16.0f, u = 1.0f - t;
			const float x1 = u * u * c.x1 + 2 * u * t * c.x2 + t * t * c.x3;
			const float y1 = u * u * c.y1 + 2 * u * t * c.y2 + t * t * c.y3;

			if ((y0 <= py) != (y1 <= py)) {
				const float xi = x0 + (py - y0) / (y1 - y0) * (x1 - x0);

				if (xi > px)
					w += y1 > y0 ? 1 : -1;
			}

			x0 = x1;
			y0 = y1;
		}
	}

	return w;
}

// Distance from a point to the flattened curves, to skip samples near an edge.
float edgeDistance(const slughorn::Atlas::Curves &curves, float px, float py)
{
	float best = 1e9f;

	for (const auto &c : curves) {
		for (int i = 0; i <= 16; i++) {
			const float t = float(i) / 16.0f, u = 1.0f - t;
			const float x = u * u * c.x1 + 2 * u * t * c.x2 + t * t * c.x3;
			const float y = u * u * c.y1 + 2 * u * t * c.y2 + t * t * c.y3;

			best = std::min(best, std::hypot(x - px, y - py));
		}
	}

	return best;
}

} // namespace

TEST(shader_coverage_matches_winding_on_a_packed_atlas)
{
	FontFileRef ref;

	if (!fontenum::match(fontenum::defaultFamily(), 400, false, ref))
		return;

	// Enough distinct glyphs, and outline rings (the densest shapes), that the
	// atlas spans many rows and lists and curves wrap across them.
	Document doc;
	doc.base.sizePx = 64.0f;
	doc.base.outline.enabled = true;
	doc.base.outline.widthPx = 3.0f;
	doc.setPlainText("The quick brown fox jumps over the lazy dog 0123456789 @&%$#!?\n"
			 "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz");

	FrameState state;
	FrameInput input;
	input.document = doc;

	const FrameOutput frame = buildFrame(input, state);

	CHECK(frame.atlas != nullptr);

	if (!frame.atlas)
		return;

	const Textures textures{&frame.atlas->getCurveTextureData(), &frame.atlas->getBandTextureData()};

	CHECK(textures.band->height > 1u);

	size_t samples = 0;
	size_t wrong = 0;

	// Every fill ("f…") and outline ring ("s…") the text put in the atlas.
	for (uint32_t glyph = 0; glyph < 400; glyph++) {
		for (const char *prefix : {"f", "s"}) {
			for (uint32_t font = 1; font < 8; font++) {
				std::string key =
					std::string(prefix) + std::to_string(font) + ":" + std::to_string(glyph);

				if (*prefix == 's')
					key += ":" +
					       std::to_string(int(
						       std::lround(ShapeCache::bucketWidth(6.0f / 64.0f) * 512.0f))) +
					       ":0";

				const auto shape = state.atlas.shape(key);

				if (!shape || shape->curves.empty())
					continue;

				const float x0 = float(shape->bearingX), x1 = x0 + float(shape->width);
				const float y1 = float(shape->bearingY), y0 = y1 - float(shape->height);

				for (int i = 1; i < 12; i++) {
					for (int j = 1; j < 12; j++) {
						const float ex = x0 + (x1 - x0) * float(i) / 12.0f;
						const float ey = y0 + (y1 - y0) * float(j) / 12.0f;

						if (edgeDistance(shape->curves, ex, ey) < 0.01f)
							continue;

						const bool inside = winding(shape->curves, ex, ey) != 0;
						const float cov = coverage(textures, *shape, ex, ey, 4000.0f);

						samples++;

						if ((cov > 0.5f) != inside)
							wrong++;
					}
				}
			}
		}
	}

	std::fprintf(stderr, "  %zu samples across a %ux%u band texture, %zu wrong\n", samples, textures.band->width,
		     textures.band->height, wrong);

	CHECK(samples > 1000);
	CHECK_EQ(wrong, size_t(0));
}
