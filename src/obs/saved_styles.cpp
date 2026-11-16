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

#include "saved_styles.hpp"
#include "settings.hpp"

#include <algorithm>
#include <mutex>

#include <obs-module.h>
#include <util/platform.h>

namespace slugged {
namespace styles {

namespace {

const char *const kStylesFile = "styles.json";

std::mutex g_mutex;
std::vector<SavedStyle> g_styles;

void writeLocked()
{
	char *dir = obs_module_config_path(nullptr);

	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}

	char *path = obs_module_config_path(kStylesFile);

	if (!path)
		return;

	obs_data_t *root = obs_data_create();
	obs_data_array_t *list = obs_data_array_create();

	for (const SavedStyle &style : g_styles) {
		obs_data_t *item = obs_data_create();

		obs_data_set_string(item, "name", style.name.c_str());

		obs_data_t *look = obs_data_create();
		settings::styleToData(look, style.style);
		obs_data_set_obj(item, "style", look);
		obs_data_release(look);

		obs_data_t *box = obs_data_create();
		settings::backgroundToData(box, style.background, style.padding);
		obs_data_set_obj(item, "box", box);
		obs_data_release(box);

		if (style.hasMotion) {
			obs_data_t *motion = obs_data_create();
			settings::motionToData(motion, style.motion, style.scroll);
			obs_data_set_obj(item, "motion", motion);
			obs_data_release(motion);
		}

		obs_data_array_push_back(list, item);
		obs_data_release(item);
	}

	obs_data_set_array(root, "styles", list);
	obs_data_array_release(list);

	obs_data_save_json_safe(root, path, "tmp", "bak");

	obs_data_release(root);
	bfree(path);
}

std::vector<SavedStyle>::iterator findLocked(const std::string &name)
{
	return std::find_if(g_styles.begin(), g_styles.end(), [&](const SavedStyle &s) { return s.name == name; });
}

} // namespace

void load()
{
	char *path = obs_module_config_path(kStylesFile);

	if (!path)
		return;

	obs_data_t *root = obs_data_create_from_json_file_safe(path, "bak");

	bfree(path);

	if (!root)
		return;

	std::vector<SavedStyle> loaded;

	obs_data_array_t *list = obs_data_get_array(root, "styles");
	const size_t count = list ? obs_data_array_count(list) : 0;

	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(list, i);

		SavedStyle style;

		style.name = obs_data_get_string(item, "name");

		if (obs_data_t *look = obs_data_get_obj(item, "style")) {
			style.style = settings::styleFromData(look);
			obs_data_release(look);
		}

		if (obs_data_t *box = obs_data_get_obj(item, "box")) {
			settings::backgroundFromData(box, style.background, style.padding);
			obs_data_release(box);
		}

		if (obs_data_t *motion = obs_data_get_obj(item, "motion")) {
			style.hasMotion = true;
			settings::motionFromData(motion, style.motion, style.scroll);
			obs_data_release(motion);
		}

		obs_data_release(item);

		if (!style.name.empty())
			loaded.push_back(std::move(style));
	}

	obs_data_array_release(list);
	obs_data_release(root);

	std::lock_guard<std::mutex> lock(g_mutex);

	g_styles = std::move(loaded);
}

std::vector<SavedStyle> all()
{
	std::lock_guard<std::mutex> lock(g_mutex);

	return g_styles;
}

bool find(const std::string &name, SavedStyle &out)
{
	std::lock_guard<std::mutex> lock(g_mutex);

	const auto it = findLocked(name);

	if (it == g_styles.end())
		return false;

	out = *it;

	return true;
}

void put(const SavedStyle &style)
{
	if (style.name.empty())
		return;

	std::lock_guard<std::mutex> lock(g_mutex);

	const auto it = findLocked(style.name);

	if (it != g_styles.end())
		*it = style;
	else
		g_styles.push_back(style);

	writeLocked();
}

void remove(const std::string &name)
{
	std::lock_guard<std::mutex> lock(g_mutex);

	const auto it = findLocked(name);

	if (it == g_styles.end())
		return;

	g_styles.erase(it);
	writeLocked();
}

bool rename(const std::string &from, const std::string &to)
{
	std::lock_guard<std::mutex> lock(g_mutex);

	const auto it = findLocked(from);

	if (to.empty() || it == g_styles.end() || findLocked(to) != g_styles.end())
		return false;

	it->name = to;
	writeLocked();

	return true;
}

SavedStyle capture(const Document &doc, const std::string &name, bool withMotion)
{
	SavedStyle style;

	style.name = name;
	style.style = doc.base;
	style.background = doc.background;
	style.padding = doc.padding;
	style.hasMotion = withMotion;

	if (withMotion) {
		style.motion = doc.motion;
		style.scroll = doc.scroll;
	}

	return style;
}

void apply(Document &doc, const SavedStyle &style)
{
	doc.base = style.style;
	doc.background = style.background;
	doc.padding = style.padding;

	if (style.hasMotion) {
		doc.motion = style.motion;
		doc.scroll = style.scroll;
	}
}

void apply(Document &doc, size_t start, size_t end, const SavedStyle &style)
{
	doc.clearStyle(start, end);
	doc.applyStyle(start, end, StyleOverride::diff(doc.base, style.style));
}

} // namespace styles
} // namespace slugged
