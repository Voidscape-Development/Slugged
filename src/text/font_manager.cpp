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

#include "font_manager.hpp"
#include "font_enum.hpp"
#include "../util/log.hpp"

#include <algorithm>
#include <cmath>

#include FT_MULTIPLE_MASTERS_H
#include FT_TRUETYPE_TABLES_H
#include FT_SFNT_NAMES_H

#include <hb.h>
#include <hb-ft.h>

namespace slugged {

FontFace::~FontFace()
{
	if (hb)
		hb_font_destroy(hb);

	if (ft)
		FT_Done_Face(ft);
}

FontManager &FontManager::instance()
{
	static FontManager mgr;

	return mgr;
}

FontManager::FontManager()
{
	if (FT_Init_FreeType(&_ft)) {
		_ft = nullptr;
		slugged_log(LOG_ERROR, "FreeType failed to initialise; text rendering is unavailable");
	}
}

FontManager::~FontManager()
{
	_cache.clear();
	_faces.clear();

	if (_ft)
		FT_Done_FreeType(_ft);
}

void FontManager::reset()
{
	std::lock_guard<std::mutex> lock(_mutex);

	_cache.clear();
	_faces.clear();
	_nextSlot = 0;
}

static float tagValue(const FT_Var_Axis &axis)
{
	return float(axis.def) / 65536.0f;
}

static std::string tagToString(FT_ULong tag)
{
	char buf[5] = {
		char((tag >> 24) & 0xFF), char((tag >> 16) & 0xFF), char((tag >> 8) & 0xFF), char(tag & 0xFF), 0,
	};

	return std::string(buf);
}

void FontManager::applyVariations(FontFace &face, const FontSpec &spec)
{
	if (!face.variable || spec.axes.empty())
		return;

	FT_MM_Var *mm = nullptr;

	if (FT_Get_MM_Var(face.ft, &mm) || !mm)
		return;

	std::vector<FT_Fixed> coords(mm->num_axis);

	for (FT_UInt i = 0; i < mm->num_axis; i++) {
		const std::string tag = tagToString(mm->axis[i].tag);

		// Start from the axis default, then honour any explicit setting,
		// clamped to what the font actually supports. An out-of-range request
		// is clamped rather than rejected so a style shared between two fonts
		// with different weight ranges still renders.
		float value = tagValue(mm->axis[i]);

		const auto it = spec.axes.find(tag);

		if (it != spec.axes.end())
			value = it->second;

		const float lo = float(mm->axis[i].minimum) / 65536.0f;
		const float hi = float(mm->axis[i].maximum) / 65536.0f;

		value = std::min(std::max(value, lo), hi);

		coords[i] = FT_Fixed(value * 65536.0f);
	}

	FT_Set_Var_Design_Coordinates(face.ft, mm->num_axis, coords.data());
	FT_Done_MM_Var(_ft, mm);
}

FontFace *FontManager::openFace(const std::string &path, int faceIndex, const FontSpec &spec)
{
	if (!_ft)
		return nullptr;

	auto face = std::make_unique<FontFace>();

	if (FT_New_Face(_ft, path.c_str(), faceIndex, &face->ft) || !face->ft) {
		slugged_log(LOG_WARNING, "could not open font file '%s' (face %d)", path.c_str(), faceIndex);
		return nullptr;
	}

	if (_nextSlot == 0xFF) {
		slugged_log(LOG_WARNING, "font slot table is full (256 faces); '%s' will not render", path.c_str());
		return nullptr;
	}

	face->slot = _nextSlot++;
	face->path = path;
	face->faceIndex = faceIndex;

	if (face->ft->family_name)
		face->family = face->ft->family_name;

	if (face->ft->style_name)
		face->style = face->ft->style_name;

	face->unitsPerEm = face->ft->units_per_EM ? float(face->ft->units_per_EM) : 1000.0f;

	const float inv = 1.0f / face->unitsPerEm;

	face->ascender = float(face->ft->ascender) * inv;
	face->descender = std::fabs(float(face->ft->descender)) * inv;

	const float lineSpacing = float(face->ft->height) * inv;
	face->lineGap = std::max(0.0f, lineSpacing - (face->ascender + face->descender));

	face->underlinePos = float(face->ft->underline_position) * inv;
	face->underlineThickness = float(face->ft->underline_thickness) * inv;

	if (face->underlineThickness <= 0.0f)
		face->underlineThickness = 0.05f;

	face->hasColor = FT_HAS_COLOR(face->ft) != 0;
	face->variable = FT_HAS_MULTIPLE_MASTERS(face->ft) != 0;

	applyVariations(*face, spec);

	// hb-ft reads glyph metrics through FreeType's *scaled* metrics, which are
	// all zero until a size is set on the face. Setting the size to one em
	// (in 26.6 fixed point) makes FreeType report metrics in font units, which
	// is exactly the space hb_font_set_scale is told to use below. Without this
	// every advance comes back as zero and all glyphs stack at the origin.
	//
	// Outlining is unaffected: atlas_cache loads glyphs with FT_LOAD_NO_SCALE,
	// which ignores the char size entirely.
	FT_Set_Char_Size(face->ft, 0, FT_F26Dot6(face->ft->units_per_EM) * 64, 72, 72);

	// HarfBuzz is created after variations so it shapes against the same
	// instance FreeType will outline. hb_ft_font_create_referenced keeps its own
	// reference, so face teardown order does not matter.
	face->hb = hb_ft_font_create_referenced(face->ft);

	if (face->hb) {
		// HarfBuzz positions are expressed in units where `scale` equals one
		// em, so dividing by the scale gives an em fraction regardless of what
		// units FreeType happened to report in. Reading the scale back rather
		// than assuming it keeps this correct across HarfBuzz versions and
		// avoids a hard-coded 26.6 conversion factor.
		int scaleX = 0;
		int scaleY = 0;

		hb_font_get_scale(face->hb, &scaleX, &scaleY);

		face->hbScaleX = scaleX ? float(scaleX) : face->unitsPerEm;
		face->hbScaleY = scaleY ? float(scaleY) : face->unitsPerEm;
	}

	FontFace *raw = face.get();
	_faces.push_back(std::move(face));

	return raw;
}

FontFace *FontManager::acquire(const FontSpec &spec)
{
	std::lock_guard<std::mutex> lock(_mutex);

	for (const Entry &e : _cache)
		if (e.spec == spec)
			return e.face;

	FontFace *face = nullptr;

	// An explicit file always wins: that is what makes a scene collection
	// portable to a machine where the font was never installed.
	if (!spec.file.empty())
		face = openFace(spec.file, spec.faceIndex, spec);

	if (!face) {
		FontFileRef ref;

		if (fontenum::match(spec.family, spec.weight, spec.italic, ref))
			face = openFace(ref.path, ref.faceIndex, spec);
	}

	if (!face) {
		FontFileRef ref;

		if (fontenum::match(fontenum::defaultFamily(), spec.weight, spec.italic, ref)) {
			slugged_log(LOG_WARNING, "font '%s' not found; falling back to '%s'", spec.family.c_str(),
				    fontenum::defaultFamily().c_str());

			face = openFace(ref.path, ref.faceIndex, spec);
		}
	}

	// Cache negative results too, so a missing font does not re-run platform
	// enumeration on every single layout pass.
	_cache.push_back(Entry{spec, face});

	return face;
}

FontFace *FontManager::fallbackFor(const FontSpec &spec, uint32_t codepoint)
{
	{
		std::lock_guard<std::mutex> lock(_mutex);

		// A face already open that covers the codepoint is always preferable to
		// opening another file.
		for (const std::unique_ptr<FontFace> &f : _faces)
			if (FT_Get_Char_Index(f->ft, codepoint))
				return f.get();
	}

	for (const FontFileRef &ref : fontenum::fallbacks(codepoint, spec.family)) {
		FontSpec fallbackSpec = spec;
		fallbackSpec.family.clear();
		fallbackSpec.file = ref.path;
		fallbackSpec.faceIndex = ref.faceIndex;

		FontFace *face = acquire(fallbackSpec);

		if (face && FT_Get_Char_Index(face->ft, codepoint))
			return face;
	}

	return nullptr;
}

std::vector<FontAxis> FontManager::axesOf(const FontSpec &spec)
{
	std::vector<FontAxis> out;

	FontFace *face = acquire(spec);

	if (!face || !face->variable)
		return out;

	FT_MM_Var *mm = nullptr;

	if (FT_Get_MM_Var(face->ft, &mm) || !mm)
		return out;

	for (FT_UInt i = 0; i < mm->num_axis; i++) {
		FontAxis axis;

		axis.tag = tagToString(mm->axis[i].tag);
		axis.name = mm->axis[i].name ? mm->axis[i].name : axis.tag;
		axis.minValue = float(mm->axis[i].minimum) / 65536.0f;
		axis.maxValue = float(mm->axis[i].maximum) / 65536.0f;
		axis.defaultValue = float(mm->axis[i].def) / 65536.0f;

		out.push_back(std::move(axis));
	}

	FT_Done_MM_Var(_ft, mm);

	return out;
}

} // namespace slugged
