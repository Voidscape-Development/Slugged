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
#include "settings.hpp"
#include "slugged_source.hpp"
#include "../util/log.hpp"

#include <cstring>

namespace slugged {
namespace migrate {

namespace {

const char *const kGdiPlus = "text_gdiplus";
const char *const kFreeType = "text_ft2_source";

// GDI+ stores colours as 0x00BBGGRR with a separate 0-100 opacity; Slugged uses
// 0xAABBGGRR.
uint32_t withOpacity(uint32_t bgr, long long opacityPercent)
{
	const uint32_t alpha = uint32_t((opacityPercent * 255) / 100) & 0xFF;

	return (bgr & 0x00FFFFFF) | (alpha << 24);
}

void copyFont(obs_data_t *legacy, obs_data_t *out)
{
	obs_data_t *font = obs_data_get_obj(legacy, "font");

	if (!font)
		return;

	obs_data_t *copy = obs_data_create();

	obs_data_set_string(copy, "face", obs_data_get_string(font, "face"));
	obs_data_set_string(copy, "style", obs_data_get_string(font, "style"));
	obs_data_set_int(copy, "size", obs_data_get_int(font, "size") > 0 ? obs_data_get_int(font, "size") : 64);
	obs_data_set_int(copy, "flags", obs_data_get_int(font, "flags"));

	obs_data_set_obj(out, "font", copy);
	obs_data_release(copy);
	obs_data_release(font);
}

void importGdiPlus(obs_data_t *legacy, obs_data_t *out)
{
	copyFont(legacy, out);

	obs_data_set_string(out, "text", obs_data_get_string(legacy, "text"));

	const bool useFile = obs_data_get_bool(legacy, "read_from_file");

	obs_data_set_string(out, "mode", useFile ? "file" : "text");
	obs_data_set_string(out, "file", obs_data_get_string(legacy, "file"));
	obs_data_set_bool(out, "chatlog", obs_data_get_bool(legacy, "chatlog"));
	obs_data_set_int(out, "chatlog_lines", obs_data_get_int(legacy, "chatlog_lines"));

	obs_data_set_int(out, "color",
			 withOpacity(uint32_t(obs_data_get_int(legacy, "color")), obs_data_get_int(legacy, "opacity")));
	obs_data_set_int(out, "opacity", 100);

	if (obs_data_get_bool(legacy, "gradient")) {
		obs_data_set_string(out, "fill", "linear");
		obs_data_set_int(out, "gradient_color",
				 withOpacity(uint32_t(obs_data_get_int(legacy, "gradient_color")),
					     obs_data_get_int(legacy, "gradient_opacity")));
		obs_data_set_double(out, "gradient_angle", obs_data_get_double(legacy, "gradient_dir"));
	}

	obs_data_set_bool(out, "outline", obs_data_get_bool(legacy, "outline"));

	// GDI+ centres its stroke on the edge; Slugged measures what shows outside.
	obs_data_set_double(out, "outline_width", double(obs_data_get_int(legacy, "outline_size")) * 0.5);
	obs_data_set_int(out, "outline_color",
			 withOpacity(uint32_t(obs_data_get_int(legacy, "outline_color")),
				     obs_data_get_int(legacy, "outline_opacity")));

	const long long bkOpacity = obs_data_get_int(legacy, "bk_opacity");

	obs_data_set_bool(out, "background", bkOpacity > 0);
	obs_data_set_int(out, "background_color",
			 withOpacity(uint32_t(obs_data_get_int(legacy, "bk_color")), bkOpacity));

	const std::string align = obs_data_get_string(legacy, "align");
	const std::string valign = obs_data_get_string(legacy, "valign");

	obs_data_set_string(out, "align", align == "center" ? "center" : (align == "right" ? "end" : "start"));
	obs_data_set_string(out, "valign", valign == "center" ? "middle" : (valign == "bottom" ? "bottom" : "top"));

	obs_data_set_bool(out, "extents", obs_data_get_bool(legacy, "extents"));
	obs_data_set_int(out, "extents_cx", obs_data_get_int(legacy, "extents_cx"));
	obs_data_set_int(out, "extents_cy", obs_data_get_int(legacy, "extents_cy"));
	obs_data_set_string(out, "wrap", obs_data_get_bool(legacy, "extents_wrap") ? "word" : "none");
}

void importFreeType(obs_data_t *legacy, obs_data_t *out)
{
	copyFont(legacy, out);

	obs_data_set_string(out, "text", obs_data_get_string(legacy, "text"));

	obs_data_set_string(out, "mode", obs_data_get_bool(legacy, "from_file") ? "file" : "text");
	obs_data_set_string(out, "file", obs_data_get_string(legacy, "text_file"));
	obs_data_set_bool(out, "chatlog", obs_data_get_bool(legacy, "log_mode"));
	obs_data_set_int(out, "chatlog_lines", obs_data_get_int(legacy, "log_lines"));

	// FreeType 2 always draws a top-to-bottom gradient from colour 1 to
	// colour 2; equal colours are what it calls a solid fill.
	const uint32_t color1 = uint32_t(obs_data_get_int(legacy, "color1"));
	const uint32_t color2 = uint32_t(obs_data_get_int(legacy, "color2"));

	obs_data_set_int(out, "color", color1 ? color1 : 0xFFFFFFFF);
	obs_data_set_int(out, "opacity", 100);

	if (color2 && color2 != color1) {
		obs_data_set_string(out, "fill", "linear");
		obs_data_set_int(out, "gradient_color", color2);
		obs_data_set_double(out, "gradient_angle", 90.0);
	}

	obs_data_set_bool(out, "outline", obs_data_get_bool(legacy, "outline"));
	obs_data_set_double(out, "outline_width", 1.0);
	obs_data_set_int(out, "outline_color", 0xFF000000);

	obs_data_set_bool(out, "shadow", obs_data_get_bool(legacy, "drop_shadow"));

	const long long customWidth = obs_data_get_int(legacy, "custom_width");

	if (customWidth > 0) {
		obs_data_set_bool(out, "extents", true);
		obs_data_set_int(out, "extents_cx", customWidth);
		obs_data_set_int(out, "extents_cy", 200);
	}

	obs_data_set_string(out, "wrap", obs_data_get_bool(legacy, "word_wrap") ? "word" : "none");
}

bool enumLegacy(void *param, obs_source_t *source)
{
	auto *names = static_cast<std::vector<std::string> *>(param);

	if (isLegacyTextSource(obs_source_get_unversioned_id(source))) {
		if (const char *name = obs_source_get_name(source))
			names->push_back(name);
	}

	return true;
}

struct Placement {
	obs_scene_t *scene = nullptr;
	obs_sceneitem_t *item = nullptr;
};

struct FindContext {
	obs_source_t *target = nullptr;
	std::vector<Placement> found;
};

bool findItems(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	auto *ctx = static_cast<FindContext *>(param);

	if (obs_sceneitem_get_source(item) == ctx->target) {
		obs_sceneitem_addref(item);
		ctx->found.push_back({scene, item});
	}

	// Groups hold items of their own.
	if (obs_sceneitem_is_group(item))
		obs_sceneitem_group_enum_items(item, findItems, param);

	return true;
}

bool enumScenes(void *param, obs_source_t *source)
{
	if (obs_scene_t *scene = obs_scene_from_source(source))
		obs_scene_enum_items(scene, findItems, param);

	return true;
}

int orderOf(obs_scene_t *scene, obs_sceneitem_t *target)
{
	struct Context {
		obs_sceneitem_t *target;
		int index = 0;
		int found = -1;
	} ctx{target};

	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *c = static_cast<Context *>(param);

			if (item == c->target)
				c->found = c->index;

			c->index++;
			return true;
		},
		&ctx);

	return ctx.found;
}

} // namespace

bool isLegacyTextSource(const char *sourceId)
{
	return sourceId && (std::strcmp(sourceId, kGdiPlus) == 0 || std::strcmp(sourceId, kFreeType) == 0);
}

std::vector<std::string> legacySourceNames()
{
	std::vector<std::string> names;

	obs_enum_sources(enumLegacy, &names);

	return names;
}

bool importSettings(obs_source_t *legacy, obs_data_t *target)
{
	if (!legacy || !target)
		return false;

	const char *id = obs_source_get_unversioned_id(legacy);

	if (!isLegacyTextSource(id))
		return false;

	obs_data_t *legacySettings = obs_source_get_settings(legacy);

	if (!legacySettings)
		return false;

	if (std::strcmp(id, kFreeType) == 0)
		importFreeType(legacySettings, target);
	else
		importGdiPlus(legacySettings, target);

	obs_data_release(legacySettings);

	// The new text has no per-character styling yet; let it be built from the
	// plain text.
	obs_data_erase(target, "content");
	obs_data_set_int(target, "schema", settings::kSchemaVersion);

	return true;
}

obs_source_t *convert(obs_source_t *legacy, const ConvertOptions &options)
{
	if (!legacy)
		return nullptr;

	obs_data_t *data = obs_data_create();

	if (!importSettings(legacy, data)) {
		obs_data_release(data);
		return nullptr;
	}

	const std::string name = obs_source_get_name(legacy);

	// Free the name first, so the replacement can take it.
	obs_source_set_name(legacy, (name + " (old)").c_str());

	obs_source_t *replacement = obs_source_create(kSourceId, name.c_str(), data, nullptr);

	obs_data_release(data);

	if (!replacement) {
		obs_source_set_name(legacy, name.c_str());
		return nullptr;
	}

	obs_source_copy_filters(replacement, legacy);

	FindContext ctx;
	ctx.target = legacy;

	obs_enum_scenes(enumScenes, &ctx);

	for (const Placement &p : ctx.found) {
		obs_sceneitem_t *item = obs_scene_add(p.scene, replacement);

		if (item) {
			obs_transform_info info;
			obs_sceneitem_crop crop;

			obs_sceneitem_get_info2(p.item, &info);
			obs_sceneitem_get_crop(p.item, &crop);

			obs_sceneitem_set_info2(item, &info);
			obs_sceneitem_set_crop(item, &crop);
			obs_sceneitem_set_visible(item, obs_sceneitem_visible(p.item));
			obs_sceneitem_set_locked(item, obs_sceneitem_locked(p.item));
			obs_sceneitem_set_blending_mode(item, obs_sceneitem_get_blending_mode(p.item));
			obs_sceneitem_set_scale_filter(item, obs_sceneitem_get_scale_filter(p.item));

			const int order = orderOf(p.scene, p.item);

			if (order >= 0)
				obs_sceneitem_set_order_position(item, order);
		}

		obs_sceneitem_remove(p.item);
		obs_sceneitem_release(p.item);
	}

	slugged_log(LOG_INFO, "converted '%s' (%s) to Slugged in %zu place(s)", name.c_str(),
		    obs_source_get_unversioned_id(legacy), ctx.found.size());

	if (options.deleteOriginal)
		obs_source_remove(legacy);

	return replacement;
}

} // namespace migrate
} // namespace slugged
