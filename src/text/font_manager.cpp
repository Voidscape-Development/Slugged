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
#include <set>

#include FT_MULTIPLE_MASTERS_H
#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_TAGS_H

#include <hb.h>

namespace slugged {

namespace {

// Instances unused for this many rebuilds are closed, once there are more of
// them than kInstanceBudget. A slider drag through a variable font's weight axis
// creates an instance per step; they are cheap, but not free forever.
constexpr uint64_t kIdleRebuilds = 600;
constexpr size_t kInstanceBudget = 48;

std::string tagToString(FT_ULong tag)
{
	const char buf[5] = {
		char((tag >> 24) & 0xFF), char((tag >> 16) & 0xFF), char((tag >> 8) & 0xFF), char(tag & 0xFF), 0,
	};

	return std::string(buf);
}

bool hasTable(FT_Face face, FT_ULong tag)
{
	FT_ULong length = 0;

	return FT_Load_Sfnt_Table(face, tag, 0, nullptr, &length) == 0 && length > 0;
}

} // namespace

FontFile::~FontFile()
{
	if (hbFace)
		hb_face_destroy(hbFace);

	if (ft)
		FT_Done_Face(ft);
}

FontInstance::~FontInstance()
{
	if (hb)
		hb_font_destroy(hb);
}

void FontInstance::select() const
{
	if (!file || coords.empty())
		return;

	std::vector<FT_Fixed> copy = coords;

	FT_Set_Var_Design_Coordinates(file->ft, FT_UInt(copy.size()), copy.data());
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
	reset();

	if (_ft)
		FT_Done_FreeType(_ft);
}

void FontManager::reset()
{
	auto guard = lock();

	_specs.clear();
	_fallbacks.clear();
	_lastUsed.clear();
	_instances.clear();
	_files.clear();
}

void FontManager::refresh()
{
	auto guard = lock();

	_specs.erase(std::remove_if(_specs.begin(), _specs.end(), [](const SpecEntry &e) { return e.missing; }),
		     _specs.end());

	_fallbacks.clear();
}

FontFile *FontManager::openFile(const std::string &path, int faceIndex)
{
	if (!_ft || path.empty())
		return nullptr;

	for (const std::unique_ptr<FontFile> &f : _files)
		if (f->path == path && f->faceIndex == faceIndex)
			return f.get();

	auto file = std::make_unique<FontFile>();

	if (FT_New_Face(_ft, path.c_str(), faceIndex, &file->ft) || !file->ft) {
		slugged_log(LOG_WARNING, "could not open font file '%s' (face %d)", path.c_str(), faceIndex);
		return nullptr;
	}

	// HarfBuzz reads the file itself rather than going through FreeType, so
	// shaping uses the font's own metrics tables and works for bitmap-only
	// colour fonts, which FreeType cannot scale.
	hb_blob_t *blob = hb_blob_create_from_file_or_fail(path.c_str());

	if (!blob) {
		slugged_log(LOG_WARNING, "could not read font file '%s'", path.c_str());
		return nullptr;
	}

	file->hbFace = hb_face_create(blob, unsigned(faceIndex & 0xFFFF));
	hb_blob_destroy(blob);

	file->path = path;
	file->faceIndex = faceIndex;

	FT_Face ft = file->ft;

	if (ft->family_name)
		file->family = ft->family_name;

	if (ft->style_name)
		file->style = ft->style_name;

	file->unitsPerEm = ft->units_per_EM ? float(ft->units_per_EM) : 1000.0f;

	const float inv = 1.0f / file->unitsPerEm;

	file->ascender = float(ft->ascender) * inv;
	file->descender = std::fabs(float(ft->descender)) * inv;
	file->lineGap = std::max(0.0f, float(ft->height) * inv - (file->ascender + file->descender));

	// A bitmap-only face reports no scalable metrics at all.
	if (file->ascender <= 0.0f && file->descender <= 0.0f) {
		file->ascender = 0.95f;
		file->descender = 0.25f;
	}

	file->underlinePos = float(ft->underline_position) * inv;
	file->underlineThickness = float(ft->underline_thickness) * inv;

	if (file->underlineThickness <= 0.0f) {
		file->underlinePos = -0.1f;
		file->underlineThickness = 0.05f;
	}

	if (const auto *os2 = static_cast<const TT_OS2 *>(FT_Get_Sfnt_Table(ft, FT_SFNT_OS2))) {
		// Version 0xFFFF marks a synthesised table with no usable values.
		if (os2->version != 0xFFFF && os2->yStrikeoutSize > 0) {
			file->strikeoutPos = float(os2->yStrikeoutPosition) * inv;
			file->strikeoutThickness = float(os2->yStrikeoutSize) * inv;
		}
	}

	file->hasColorLayers = hasTable(ft, FT_MAKE_TAG('C', 'O', 'L', 'R'));
	file->hasColorBitmaps = hasTable(ft, FT_MAKE_TAG('C', 'B', 'D', 'T')) ||
				hasTable(ft, FT_MAKE_TAG('s', 'b', 'i', 'x'));
	file->variable = FT_HAS_MULTIPLE_MASTERS(ft) != 0;

	FontFile *raw = file.get();
	_files.push_back(std::move(file));

	return raw;
}

FontInstance *FontManager::instanceFor(FontFile *file, const FontSpec &spec)
{
	if (!file)
		return nullptr;

	std::vector<FT_Fixed> coords;

	if (file->variable) {
		FT_MM_Var *mm = nullptr;

		if (!FT_Get_MM_Var(file->ft, &mm) && mm) {
			coords.resize(mm->num_axis);

			// Start from the coordinates the face opened with, which carry a
			// named instance's settings when the matcher picked one.
			std::vector<FT_Fixed> current(mm->num_axis);

			const bool haveCurrent =
				FT_Get_Var_Design_Coordinates(file->ft, mm->num_axis, current.data()) == 0;

			for (FT_UInt i = 0; i < mm->num_axis; i++) {
				const FT_Var_Axis &axis = mm->axis[i];
				const std::string tag = tagToString(axis.tag);

				float value = float(haveCurrent ? current[i] : axis.def) / 65536.0f;

				// The style's weight drives a variable font's weight axis, so
				// "Semibold" means the same thing for static and variable
				// families.
				if (tag == "wght")
					value = float(spec.weight);

				const auto it = spec.axes.find(tag);

				if (it != spec.axes.end())
					value = it->second;

				const float lo = float(axis.minimum) / 65536.0f;
				const float hi = float(axis.maximum) / 65536.0f;

				coords[i] = FT_Fixed(std::lround(std::clamp(value, lo, hi) * 65536.0f));
			}

			FT_Done_MM_Var(_ft, mm);
		}
	}

	for (const std::unique_ptr<FontInstance> &inst : _instances)
		if (inst->file == file && inst->coords == coords)
			return inst.get();

	auto inst = std::make_unique<FontInstance>();

	inst->file = file;
	inst->id = _nextId++;
	inst->coords = coords;
	inst->hb = hb_font_create(file->hbFace);

	hb_font_set_scale(inst->hb, int(file->unitsPerEm), int(file->unitsPerEm));

	if (!coords.empty()) {
		std::vector<float> design(coords.size());

		for (size_t i = 0; i < coords.size(); i++)
			design[i] = float(coords[i]) / 65536.0f;

		hb_font_set_var_coords_design(inst->hb, design.data(), unsigned(design.size()));
	}

	FontInstance *raw = inst.get();
	_instances.push_back(std::move(inst));

	return raw;
}

FontInstance *FontManager::acquire(const FontSpec &spec)
{
	auto guard = lock();

	for (const SpecEntry &e : _specs) {
		if (e.spec == spec) {
			if (e.instance)
				_lastUsed[e.instance->id] = _rebuild;

			return e.instance;
		}
	}

	SpecEntry entry;
	entry.spec = spec;

	FontFile *file = nullptr;

	// An explicit file always wins: that is what makes a scene collection
	// portable to a machine where the font was never installed.
	if (!spec.file.empty())
		file = openFile(spec.file, spec.faceIndex);

	const std::string family = spec.family.empty() ? fontenum::defaultFamily() : spec.family;

	if (!file) {
		FontFileRef ref;

		if (fontenum::match(family, spec.weight, spec.italic, ref))
			file = openFile(ref.path, ref.faceIndex);
	}

	if (!file) {
		entry.missing = true;

		FontFileRef ref;

		if (fontenum::match(fontenum::defaultFamily(), spec.weight, spec.italic, ref)) {
			slugged_log(LOG_WARNING, "font '%s' not found; using '%s'", family.c_str(),
				    fontenum::defaultFamily().c_str());

			file = openFile(ref.path, ref.faceIndex);
		}
	}

	entry.instance = instanceFor(file, spec);

	if (entry.instance)
		_lastUsed[entry.instance->id] = _rebuild;

	_specs.push_back(entry);

	return entry.instance;
}

bool FontManager::isEmoji(uint32_t cp)
{
	return (cp >= 0x1F300 && cp <= 0x1FAFF) || (cp >= 0x1F1E6 && cp <= 0x1F1FF) || cp == 0x1F004 || cp == 0x1F0CF ||
	       cp == 0x1F18E || (cp >= 0x1F191 && cp <= 0x1F19A);
}

FontInstance *FontManager::fallbackFor(const FontSpec &spec, uint32_t codepoint, bool preferColor)
{
	auto guard = lock();

	const FallbackKey key{codepoint, spec.family, spec.weight, spec.italic, preferColor};

	auto it = _fallbacks.find(key);

	if (it == _fallbacks.end()) {
		std::pair<std::string, int> chosen;
		std::pair<std::string, int> firstCovering;

		for (const FontFileRef &ref : fontenum::fallbacks(codepoint, spec.family)) {
			FontFile *file = openFile(ref.path, ref.faceIndex);

			if (!file || !FT_Get_Char_Index(file->ft, codepoint))
				continue;

			if (firstCovering.first.empty())
				firstCovering = {ref.path, ref.faceIndex};

			if (!preferColor || file->hasColorLayers || file->hasColorBitmaps) {
				chosen = {ref.path, ref.faceIndex};
				break;
			}
		}

		if (chosen.first.empty())
			chosen = firstCovering;

		it = _fallbacks.emplace(key, chosen).first;
	}

	if (it->second.first.empty())
		return nullptr;

	FontSpec fallback;

	fallback.file = it->second.first;
	fallback.faceIndex = it->second.second;
	fallback.weight = spec.weight;
	fallback.italic = spec.italic;

	return acquire(fallback);
}

std::vector<FontAxis> FontManager::axesOf(const FontSpec &spec)
{
	auto guard = lock();

	std::vector<FontAxis> out;

	FontInstance *inst = acquire(spec);

	if (!inst || !inst->file->variable)
		return out;

	FT_MM_Var *mm = nullptr;

	if (FT_Get_MM_Var(inst->file->ft, &mm) || !mm)
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

std::vector<uint32_t> FontManager::collect()
{
	auto guard = lock();

	_rebuild++;

	std::vector<uint32_t> evicted;

	if (_instances.size() <= kInstanceBudget)
		return evicted;

	std::set<const FontInstance *> doomed;

	for (const std::unique_ptr<FontInstance> &inst : _instances) {
		const auto used = _lastUsed.find(inst->id);
		const uint64_t last = used == _lastUsed.end() ? 0 : used->second;

		if (last + kIdleRebuilds < _rebuild)
			doomed.insert(inst.get());
	}

	if (doomed.empty())
		return evicted;

	_specs.erase(std::remove_if(_specs.begin(), _specs.end(),
				    [&](const SpecEntry &e) { return doomed.count(e.instance) > 0; }),
		     _specs.end());

	_instances.erase(std::remove_if(_instances.begin(), _instances.end(),
					[&](const std::unique_ptr<FontInstance> &inst) {
						if (!doomed.count(inst.get()))
							return false;

						evicted.push_back(inst->id);
						_lastUsed.erase(inst->id);
						return true;
					}),
			 _instances.end());

	// Close files nothing refers to any more.
	_files.erase(std::remove_if(_files.begin(), _files.end(),
				    [&](const std::unique_ptr<FontFile> &file) {
					    for (const std::unique_ptr<FontInstance> &inst : _instances)
						    if (inst->file == file.get())
							    return false;

					    return true;
				    }),
		     _files.end());

	// Fallback answers name files by path, so they survive the file closing.
	return evicted;
}

} // namespace slugged
