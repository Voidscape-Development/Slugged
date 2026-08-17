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

#include "migrate.hpp"
#include "../util/log.hpp"

#include <cstring>
#include <string>

namespace slugged {
namespace migrate {

namespace {

const char *const kGdiPlus = "text_gdiplus";
const char *const kGdiPlusV2 = "text_gdiplus_v2";
const char *const kFreeType = "text_ft2_source";
const char *const kFreeTypeV2 = "text_ft2_source_v2";

// GDI+ stores colours as 0x00BBGGRR with a separate 0-100 opacity, while
// Slugged (and OBS's newer colour_alpha properties) use 0xAABBGGRR. Rebuild the
// alpha channel from the opacity so imported text keeps its transparency.
uint32_t withOpacity(uint32_t bgr, long long opacityPercent)
{
	const uint32_t alpha = uint32_t((opacityPercent * 255) / 100) & 0xFF;

	return (bgr & 0x00FFFFFF) | (alpha << 24);
}

struct LegacyFont {
	std::string face;
	std::string style;
	int size = 64;
	uint32_t flags = 0;
};

LegacyFont readFont(obs_data_t *settings)
{
	LegacyFont font;

	obs_data_t *data = obs_data_get_obj(settings, "font");

	if (!data)
		return font;

	font.face = obs_data_get_string(data, "face");
	font.style = obs_data_get_string(data, "style");
	font.size = int(obs_data_get_int(data, "size"));
	font.flags = uint32_t(obs_data_get_int(data, "flags"));

	if (font.size <= 0)
		font.size = 64;

	obs_data_release(data);

	return font;
}

void importGdiPlus(obs_data_t *legacy, obs_data_t *out)
{
	const LegacyFont font = readFont(legacy);

	obs_data_t *fontData = obs_data_create();

	obs_data_set_string(fontData, "face", font.face.c_str());
	obs_data_set_string(fontData, "style", font.style.c_str());
	obs_data_set_int(fontData, "size", font.size);
	obs_data_set_int(fontData, "flags", font.flags);

	obs_data_set_obj(out, "font", fontData);
	obs_data_release(fontData);

	obs_data_set_string(out, "text", obs_data_get_string(legacy, "text"));

	const bool useFile = obs_data_get_bool(legacy, "read_from_file") || obs_data_get_bool(legacy, "use_file");

	obs_data_set_int(out, "mode", useFile ? 1 : 0);
	obs_data_set_string(out, "file", obs_data_get_string(legacy, "file"));

	obs_data_set_bool(out, "chatlog", obs_data_get_bool(legacy, "chatlog"));
	obs_data_set_int(out, "chatlog_lines", obs_data_get_int(legacy, "chatlog_lines"));

	const long long opacity = obs_data_get_int(legacy, "opacity");

	obs_data_set_int(out, "color", withOpacity(uint32_t(obs_data_get_int(legacy, "color")), opacity));
	obs_data_set_int(out, "opacity", 100);

	obs_data_set_bool(out, "outline", obs_data_get_bool(legacy, "outline"));
	obs_data_set_int(out, "outline_size", obs_data_get_int(legacy, "outline_size"));
	obs_data_set_int(out, "outline_color",
			 withOpacity(uint32_t(obs_data_get_int(legacy, "outline_color")),
				     obs_data_get_int(legacy, "outline_opacity")));

	const long long bkOpacity = obs_data_get_int(legacy, "bk_opacity");

	obs_data_set_bool(out, "background", bkOpacity > 0);
	obs_data_set_int(out, "background_color",
			 withOpacity(uint32_t(obs_data_get_int(legacy, "bk_color")), bkOpacity));

	// GDI+ alignment is stored as a string.
	const std::string align = obs_data_get_string(legacy, "align");
	const std::string valign = obs_data_get_string(legacy, "valign");

	obs_data_set_int(out, "align",
			 align == "center" ? int(HAlign::Center)
					   : (align == "right" ? int(HAlign::Right) : int(HAlign::Left)));

	obs_data_set_int(out, "valign",
			 valign == "center" ? int(VAlign::Middle)
					    : (valign == "bottom" ? int(VAlign::Bottom) : int(VAlign::Top)));

	const bool extents = obs_data_get_bool(legacy, "extents");

	obs_data_set_bool(out, "extents", extents);
	obs_data_set_int(out, "extents_cx", obs_data_get_int(legacy, "extents_cx"));
	obs_data_set_int(out, "extents_cy", obs_data_get_int(legacy, "extents_cy"));
	obs_data_set_int(out, "wrap",
			 obs_data_get_bool(legacy, "extents_wrap") ? int(WrapMode::Word) : int(WrapMode::None));

	// Import writes the flat properties and lets the source rebuild a document
	// from them, and those properties carry only one colour. A GDI+ gradient is
	// therefore flattened to its primary colour here; the editor can restore a
	// real gradient afterwards, with more control than GDI+ offered.
	if (obs_data_get_bool(legacy, "gradient"))
		slugged_log(LOG_INFO,
			    "'%s' used a gradient fill; imported as its primary colour -- "
			    "re-create the gradient in the Slugged editor",
			    obs_data_get_string(legacy, "text"));
}

void importFreeType(obs_data_t *legacy, obs_data_t *out)
{
	const LegacyFont font = readFont(legacy);

	obs_data_t *fontData = obs_data_create();

	obs_data_set_string(fontData, "face", font.face.c_str());
	obs_data_set_string(fontData, "style", font.style.c_str());
	obs_data_set_int(fontData, "size", font.size);
	obs_data_set_int(fontData, "flags", font.flags);

	obs_data_set_obj(out, "font", fontData);
	obs_data_release(fontData);

	obs_data_set_string(out, "text", obs_data_get_string(legacy, "text"));

	const bool useFile = obs_data_get_bool(legacy, "from_file");

	obs_data_set_int(out, "mode", useFile ? 1 : 0);
	obs_data_set_string(out, "file", obs_data_get_string(legacy, "text_file"));

	obs_data_set_bool(out, "chatlog", obs_data_get_bool(legacy, "log_mode"));
	obs_data_set_int(out, "chatlog_lines", obs_data_get_int(legacy, "log_lines"));

	// The FreeType source has no alpha channel of its own; colour1 is the top
	// of its vertical gradient and is the closest single-colour equivalent.
	const uint32_t color1 = uint32_t(obs_data_get_int(legacy, "color1"));

	obs_data_set_int(out, "color", color1 ? color1 : 0xFFFFFFFF);
	obs_data_set_int(out, "opacity", 100);

	obs_data_set_bool(out, "outline", obs_data_get_bool(legacy, "outline"));
	obs_data_set_int(out, "outline_size", 2);
	obs_data_set_int(out, "outline_color", 0xFF000000);

	obs_data_set_bool(out, "shadow", obs_data_get_bool(legacy, "drop_shadow"));

	const long long customWidth = obs_data_get_int(legacy, "custom_width");

	if (customWidth > 0) {
		obs_data_set_bool(out, "extents", true);
		obs_data_set_int(out, "extents_cx", customWidth);
		obs_data_set_int(out, "extents_cy", 200);
	}

	obs_data_set_int(out, "wrap",
			 obs_data_get_bool(legacy, "word_wrap") ? int(WrapMode::Word) : int(WrapMode::None));
}

struct EnumContext {
	obs_property_t *list = nullptr;
};

bool enumSource(void *param, obs_source_t *source)
{
	auto *ctx = static_cast<EnumContext *>(param);

	const char *id = obs_source_get_unversioned_id(source);

	if (isLegacyTextSource(id)) {
		const char *name = obs_source_get_name(source);

		if (name)
			obs_property_list_add_string(ctx->list, name, name);
	}

	return true;
}

} // namespace

bool isLegacyTextSource(const char *sourceId)
{
	if (!sourceId)
		return false;

	return std::strcmp(sourceId, kGdiPlus) == 0 || std::strcmp(sourceId, kGdiPlusV2) == 0 ||
	       std::strcmp(sourceId, kFreeType) == 0 || std::strcmp(sourceId, kFreeTypeV2) == 0;
}

void fillImportList(obs_property_t *list)
{
	if (!list)
		return;

	obs_property_list_add_string(list, obs_module_text("Import.None"), "");

	EnumContext ctx;
	ctx.list = list;

	obs_enum_sources(enumSource, &ctx);
}

bool importInto(obs_data_t *targetSettings, const char *legacySourceName)
{
	if (!targetSettings || !legacySourceName || !*legacySourceName)
		return false;

	obs_source_t *legacy = obs_get_source_by_name(legacySourceName);

	if (!legacy)
		return false;

	const char *id = obs_source_get_unversioned_id(legacy);

	bool ok = false;

	if (isLegacyTextSource(id)) {
		obs_data_t *legacySettings = obs_source_get_settings(legacy);

		if (legacySettings) {
			const bool freetype = std::strcmp(id, kFreeType) == 0 || std::strcmp(id, kFreeTypeV2) == 0;

			if (freetype)
				importFreeType(legacySettings, targetSettings);
			else
				importGdiPlus(legacySettings, targetSettings);

			// Clearing the stored document forces the flat properties just
			// written to be rebuilt into a fresh document on the next
			// update, rather than being merged into whatever was there.
			obs_data_erase(targetSettings, "document");

			obs_data_release(legacySettings);

			slugged_log(LOG_INFO, "imported settings from '%s' (%s)", legacySourceName, id);

			ok = true;
		}
	}

	obs_source_release(legacy);

	return ok;
}

} // namespace migrate
} // namespace slugged
