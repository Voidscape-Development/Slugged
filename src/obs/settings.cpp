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

#include "settings.hpp"
#include "editor_bridge.hpp"
#include "../core/markup.hpp"
#include "../text/font_enum.hpp"
#include "../util/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>

#include <util/platform.h>

namespace slugged {
namespace settings {

const char *const kVariablesKey = "variables";

namespace {

const char *const kSchemaKey = "schema";
const char *const kContentKey = "content";

// ---------------------------------------------------------------------------
// Typed access to obs_data
// ---------------------------------------------------------------------------

void put(obs_data_t *d, const char *k, const std::string &v)
{
	obs_data_set_string(d, k, v.c_str());
}

void put(obs_data_t *d, const char *k, int v)
{
	obs_data_set_int(d, k, v);
}

void put(obs_data_t *d, const char *k, bool v)
{
	obs_data_set_bool(d, k, v);
}

void put(obs_data_t *d, const char *k, float v)
{
	obs_data_set_double(d, k, double(v));
}

void put(obs_data_t *d, const char *k, const Rgba &v)
{
	obs_data_set_int(d, k, v.toObsColor());
}

void put(obs_data_t *d, const char *k, const AxisMap &v)
{
	obs_data_t *obj = obs_data_create();

	for (const auto &[tag, value] : v)
		obs_data_set_double(obj, tag.c_str(), double(value));

	obs_data_set_obj(d, k, obj);
	obs_data_release(obj);
}

template<typename E, typename = std::enable_if_t<std::is_enum_v<E>>> void put(obs_data_t *d, const char *k, E v)
{
	obs_data_set_string(d, k, enumToString(v));
}

void putDefault(obs_data_t *d, const char *k, const std::string &v)
{
	obs_data_set_default_string(d, k, v.c_str());
}

void putDefault(obs_data_t *d, const char *k, int v)
{
	obs_data_set_default_int(d, k, v);
}

void putDefault(obs_data_t *d, const char *k, bool v)
{
	obs_data_set_default_bool(d, k, v);
}

void putDefault(obs_data_t *d, const char *k, float v)
{
	obs_data_set_default_double(d, k, double(v));
}

void putDefault(obs_data_t *d, const char *k, const Rgba &v)
{
	obs_data_set_default_int(d, k, v.toObsColor());
}

void putDefault(obs_data_t *, const char *, const AxisMap &) {}

template<typename E, typename = std::enable_if_t<std::is_enum_v<E>>> void putDefault(obs_data_t *d, const char *k, E v)
{
	obs_data_set_default_string(d, k, enumToString(v));
}

void get(obs_data_t *d, const char *k, std::string &out)
{
	const char *s = obs_data_get_string(d, k);
	out = s ? s : "";
}

void get(obs_data_t *d, const char *k, int &out)
{
	out = int(obs_data_get_int(d, k));
}

void get(obs_data_t *d, const char *k, bool &out)
{
	out = obs_data_get_bool(d, k);
}

void get(obs_data_t *d, const char *k, float &out)
{
	out = float(obs_data_get_double(d, k));
}

void get(obs_data_t *d, const char *k, Rgba &out)
{
	out = Rgba::fromObsColor(uint32_t(obs_data_get_int(d, k)));
}

void get(obs_data_t *d, const char *k, AxisMap &out)
{
	out.clear();

	obs_data_t *obj = obs_data_get_obj(d, k);

	if (!obj)
		return;

	for (obs_data_item_t *item = obs_data_first(obj); item; obs_data_item_next(&item))
		out[obs_data_item_get_name(item)] = float(obs_data_item_get_double(item));

	obs_data_release(obj);
}

template<typename E, typename = std::enable_if_t<std::is_enum_v<E>>> void get(obs_data_t *d, const char *k, E &out)
{
	out = enumFromString(obs_data_get_string(d, k), out);
}

// ---------------------------------------------------------------------------
// Base style
//
// The family, size, italic and underline/strikeout flags of the base style live
// in OBS's own font object, so the dialog's font picker edits them directly;
// "font_weight" refines the picker's bold flag to a real weight.
// ---------------------------------------------------------------------------

bool isFontPickerKey(const char *key)
{
	static const char *const keys[] = {"font_family", "size", "italic", "font_weight", "underline", "strikeout"};

	for (const char *k : keys)
		if (std::strcmp(k, key) == 0)
			return true;

	return false;
}

bool contains(const std::string &haystack, const char *needle)
{
	return haystack.find(needle) != std::string::npos;
}

int weightFromStyleName(const std::string &style)
{
	if (contains(style, "Thin") || contains(style, "Hairline"))
		return 100;
	if (contains(style, "ExtraLight") || contains(style, "Extra Light") || contains(style, "UltraLight"))
		return 200;
	if (contains(style, "SemiBold") || contains(style, "Semibold") || contains(style, "Semi Bold") ||
	    contains(style, "DemiBold") || contains(style, "Demibold"))
		return 600;
	if (contains(style, "ExtraBold") || contains(style, "Extra Bold") || contains(style, "UltraBold"))
		return 800;
	if (contains(style, "Black") || contains(style, "Heavy"))
		return 900;
	if (contains(style, "Bold"))
		return 700;
	if (contains(style, "Medium"))
		return 500;
	if (contains(style, "Light"))
		return 300;

	return 400;
}

void writeFontPicker(obs_data_t *data, const Style &style)
{
	obs_data_t *font = obs_data_create();

	put(font, "face", style.font.family);
	obs_data_set_int(font, "size", std::max(1, int(std::lround(style.sizePx))));

	uint32_t flags = 0;

	if (style.font.weight >= 600)
		flags |= OBS_FONT_BOLD;

	if (style.font.italic)
		flags |= OBS_FONT_ITALIC;

	if (style.underline)
		flags |= OBS_FONT_UNDERLINE;

	if (style.strikeout)
		flags |= OBS_FONT_STRIKEOUT;

	obs_data_set_int(font, "flags", flags);

	std::string styleName = style.font.weight >= 600 ? "Bold" : "Regular";

	if (style.font.italic)
		styleName = style.font.weight >= 600 ? "Bold Italic" : "Italic";

	put(font, "style", styleName);

	obs_data_set_obj(data, "font", font);
	obs_data_release(font);

	obs_data_set_int(data, "font_weight", style.font.weight);
}

void readFontPicker(obs_data_t *data, Style &style)
{
	obs_data_t *font = obs_data_get_obj(data, "font");

	if (!font)
		return;

	const std::string face = obs_data_get_string(font, "face");
	const std::string styleName = obs_data_get_string(font, "style");
	const long long size = obs_data_get_int(font, "size");
	const uint32_t flags = uint32_t(obs_data_get_int(font, "flags"));

	obs_data_release(font);

	style.font.family = face;
	style.sizePx = size > 0 ? float(size) : 64.0f;
	style.font.italic = (flags & OBS_FONT_ITALIC) != 0 || contains(styleName, "Italic") ||
			    contains(styleName, "Oblique");
	style.underline = (flags & OBS_FONT_UNDERLINE) != 0;
	style.strikeout = (flags & OBS_FONT_STRIKEOUT) != 0;

	int derived = weightFromStyleName(styleName);

	if ((flags & OBS_FONT_BOLD) && derived < 600)
		derived = 700;

	// The precise weight wins only while it agrees with the picker about
	// whether the text is bold; ticking or unticking Bold in the picker is
	// then never ignored.
	const int precise = int(obs_data_get_int(data, "font_weight"));

	style.font.weight = precise > 0 && (precise >= 600) == (derived >= 600) ? precise : derived;
}

void writeBaseStyle(obs_data_t *data, const Style &style)
{
	writeFontPicker(data, style);

#define SLUGGED_X(name, type, member, key) \
	if (!isFontPickerKey(key))         \
		put(data, key, style.member);
	SLUGGED_STYLE_FIELDS(SLUGGED_X)
#undef SLUGGED_X
}

Style readBaseStyle(obs_data_t *data)
{
	Style style;

#define SLUGGED_X(name, type, member, key) \
	if (!isFontPickerKey(key))         \
		get(data, key, style.member);
	SLUGGED_STYLE_FIELDS(SLUGGED_X)
#undef SLUGGED_X

	readFontPicker(data, style);

	style.sizePx = std::max(style.sizePx, 1.0f);
	style.outline.widthPx = std::max(style.outline.widthPx, 0.0f);

	return style;
}

void writeOverride(obs_data_t *obj, const StyleOverride &o)
{
#define SLUGGED_X(name, type, member, key) \
	if (o.name)                        \
		put(obj, key, *o.name);
	SLUGGED_STYLE_FIELDS(SLUGGED_X)
#undef SLUGGED_X
}

StyleOverride readOverride(obs_data_t *obj)
{
	StyleOverride o;

#define SLUGGED_X(name, type, member, key)        \
	if (obs_data_has_user_value(obj, key)) { \
		type value{};                      \
		get(obj, key, value);              \
		o.name = value;                    \
	}
	SLUGGED_STYLE_FIELDS(SLUGGED_X)
#undef SLUGGED_X

	return o;
}

void writeParagraphOverride(obs_data_t *obj, const ParagraphOverride &o)
{
#define SLUGGED_X(name, type, member, key) \
	if (o.name)                        \
		put(obj, key, *o.name);
	SLUGGED_PARAGRAPH_FIELDS(SLUGGED_X)
#undef SLUGGED_X
}

ParagraphOverride readParagraphOverride(obs_data_t *obj)
{
	ParagraphOverride o;

#define SLUGGED_X(name, type, member, key)        \
	if (obs_data_has_user_value(obj, key)) { \
		type value{};                      \
		get(obj, key, value);              \
		o.name = value;                    \
	}
	SLUGGED_PARAGRAPH_FIELDS(SLUGGED_X)
#undef SLUGGED_X

	return o;
}

// ---------------------------------------------------------------------------
// Document-level fields
// ---------------------------------------------------------------------------

void writeLayout(obs_data_t *data, const Document &doc)
{
#define SLUGGED_X(name, type, member, key) put(data, key, doc.paragraph.member);
	SLUGGED_PARAGRAPH_FIELDS(SLUGGED_X)
#undef SLUGGED_X

	put(data, "extents", doc.sizeMode == SizeMode::Fixed);
	obs_data_set_int(data, "extents_cx", std::lround(doc.boxWidth));
	obs_data_set_int(data, "extents_cy", std::lround(doc.boxHeight));
	put(data, "wrap", doc.wrap);
	put(data, "valign", doc.valign);
	put(data, "padding", doc.padding);

	put(data, "background", doc.background.enabled);
	put(data, "background_color", doc.background.color);
	put(data, "background_radius", doc.background.radius);

	obs_data_set_int(data, "opacity", std::lround(doc.opacity * 100.0f));

	const MotionSpec &m = doc.motion;

	put(data, "motion", m.motion);
	put(data, "motion_order", m.order);
	put(data, "motion_direction", m.direction);
	put(data, "motion_duration", m.duration);
	put(data, "motion_stagger", m.stagger);
	put(data, "motion_param", m.param);
	put(data, "motion_speed", m.speed);
	put(data, "motion_replay", m.trigger);
	put(data, "motion_scope", m.scope);
	put(data, "motion_exit", m.exit);
	put(data, "motion_loop_interval", m.loopInterval);

	const ScrollSpec &s = doc.scroll;

	put(data, "scroll", s.enabled);
	put(data, "scroll_x", s.speedX);
	put(data, "scroll_y", s.speedY);
	put(data, "scroll_loop", s.loop);
	put(data, "scroll_gap", s.gap);
}

void readLayout(obs_data_t *data, Document &doc)
{
#define SLUGGED_X(name, type, member, key) get(data, key, doc.paragraph.member);
	SLUGGED_PARAGRAPH_FIELDS(SLUGGED_X)
#undef SLUGGED_X

	doc.paragraph.lineHeight = doc.paragraph.lineHeight > 0.0f ? doc.paragraph.lineHeight : 1.0f;

	doc.sizeMode = obs_data_get_bool(data, "extents") ? SizeMode::Fixed : SizeMode::Auto;
	doc.boxWidth = std::max(1.0f, float(obs_data_get_int(data, "extents_cx")));
	doc.boxHeight = std::max(1.0f, float(obs_data_get_int(data, "extents_cy")));
	get(data, "wrap", doc.wrap);
	get(data, "valign", doc.valign);
	get(data, "padding", doc.padding);

	get(data, "background", doc.background.enabled);
	get(data, "background_color", doc.background.color);
	get(data, "background_radius", doc.background.radius);

	doc.opacity = std::clamp(float(obs_data_get_int(data, "opacity")) / 100.0f, 0.0f, 1.0f);

	MotionSpec &m = doc.motion;

	get(data, "motion", m.motion);
	get(data, "motion_order", m.order);
	get(data, "motion_direction", m.direction);
	get(data, "motion_duration", m.duration);
	get(data, "motion_stagger", m.stagger);
	get(data, "motion_param", m.param);
	get(data, "motion_speed", m.speed);
	get(data, "motion_replay", m.trigger);
	get(data, "motion_scope", m.scope);
	get(data, "motion_exit", m.exit);
	get(data, "motion_loop_interval", m.loopInterval);

	m.duration = m.duration > 0.0f ? m.duration : 0.35f;
	m.stagger = std::max(m.stagger, 0.0f);
	m.speed = m.speed > 0.0f ? m.speed : 0.5f;
	m.loopInterval = m.loopInterval > 0.0f ? m.loopInterval : 5.0f;

	ScrollSpec &s = doc.scroll;

	get(data, "scroll", s.enabled);
	get(data, "scroll_x", s.speedX);
	get(data, "scroll_y", s.speedY);
	get(data, "scroll_loop", s.loop);
	get(data, "scroll_gap", s.gap);

	s.gap = std::max(s.gap, 0.0f);
}

void writeContent(obs_data_t *data, const Document &doc)
{
	obs_data_t *content = obs_data_create();
	obs_data_array_t *blocks = obs_data_array_create();

	for (const Block &block : doc.blocks) {
		obs_data_t *blockData = obs_data_create();

		obs_data_t *paragraph = obs_data_create();
		writeParagraphOverride(paragraph, block.paragraph);
		obs_data_set_obj(blockData, "paragraph", paragraph);
		obs_data_release(paragraph);

		obs_data_array_t *runs = obs_data_array_create();

		for (const Run &run : block.runs) {
			obs_data_t *runData = obs_data_create();

			obs_data_set_string(runData, "text", run.text.c_str());

			obs_data_t *style = obs_data_create();
			writeOverride(style, run.style);
			obs_data_set_obj(runData, "style", style);
			obs_data_release(style);

			obs_data_array_push_back(runs, runData);
			obs_data_release(runData);
		}

		obs_data_set_array(blockData, "runs", runs);
		obs_data_array_release(runs);

		obs_data_array_push_back(blocks, blockData);
		obs_data_release(blockData);
	}

	obs_data_set_array(content, "blocks", blocks);
	obs_data_array_release(blocks);

	obs_data_set_obj(data, kContentKey, content);
	obs_data_release(content);
}

// ---------------------------------------------------------------------------
// Migration from schema 1
// ---------------------------------------------------------------------------

// A schema-1 key that held an enum as a number. Returns -1 when the key holds
// something else (already migrated, or never set).
long long legacyInt(obs_data_t *data, const char *key)
{
	obs_data_item_t *item = obs_data_item_byname(data, key);

	if (!item)
		return -1;

	long long value = -1;

	if (obs_data_item_gettype(item) == OBS_DATA_NUMBER && obs_data_item_has_user_value(item))
		value = obs_data_item_get_int(item);

	obs_data_item_release(&item);

	return value;
}

template<typename E> void migrateEnum(obs_data_t *data, const char *key, std::initializer_list<E> byIndex)
{
	const long long index = legacyInt(data, key);

	if (index < 0)
		return;

	obs_data_erase(data, key);

	if (size_t(index) < byIndex.size())
		obs_data_set_string(data, key, enumToString(*(byIndex.begin() + index)));
}

// A full schema-1 style object, which every run used to carry in full.
Style legacyStyle(obs_data_t *obj, const Style &fallback)
{
	Style s = fallback;

	if (!obj)
		return s;

	s.font.family = obs_data_get_string(obj, "family");
	s.font.file = obs_data_get_string(obj, "file");
	s.font.faceIndex = int(obs_data_get_int(obj, "face_index"));
	s.font.weight = int(obs_data_get_int(obj, "weight")) > 0 ? int(obs_data_get_int(obj, "weight")) : 400;
	s.font.italic = obs_data_get_bool(obj, "italic");
	s.sizePx = obs_data_get_double(obj, "size") > 0.0 ? float(obs_data_get_double(obj, "size")) : 64.0f;

	get(obj, "axes", s.font.axes);

	if (obs_data_t *fill = obs_data_get_obj(obj, "fill")) {
		const long long type = obs_data_get_int(fill, "type");

		s.fill.type = type == 1 ? FillType::Linear : (type == 2 ? FillType::Radial : FillType::Solid);
		s.fill.color = Rgba::fromObsColor(uint32_t(obs_data_get_int(fill, "color")));
		s.fill.angleDeg = float(obs_data_get_double(fill, "angle"));

		if (obs_data_array_t *stops = obs_data_get_array(fill, "stops")) {
			const size_t count = obs_data_array_count(stops);

			if (count >= 2) {
				obs_data_t *first = obs_data_array_item(stops, 0);
				obs_data_t *last = obs_data_array_item(stops, count - 1);

				s.fill.color = Rgba::fromObsColor(uint32_t(obs_data_get_int(first, "color")));
				s.fill.color2 = Rgba::fromObsColor(uint32_t(obs_data_get_int(last, "color")));

				obs_data_release(first);
				obs_data_release(last);
			}

			obs_data_array_release(stops);
		}

		obs_data_release(fill);
	}

	s.outline.enabled = obs_data_get_bool(obj, "outline");

	// Schema 1 measured the whole stroke, half of which sat under the fill;
	// the width now is what shows outside the letter.
	s.outline.widthPx = float(obs_data_get_double(obj, "outline_width")) * 0.5f;
	s.outline.color = Rgba::fromObsColor(uint32_t(obs_data_get_int(obj, "outline_color")));

	const long long join = obs_data_get_int(obj, "outline_join");
	s.outline.join = join == 0 ? LineJoin::Miter : (join == 1 ? LineJoin::Bevel : LineJoin::Round);

	s.shadow.enabled = obs_data_get_bool(obj, "shadow");
	s.shadow.offsetX = float(obs_data_get_double(obj, "shadow_x"));
	s.shadow.offsetY = float(obs_data_get_double(obj, "shadow_y"));
	s.shadow.color = Rgba::fromObsColor(uint32_t(obs_data_get_int(obj, "shadow_color")));

	s.letterSpacing = float(obs_data_get_double(obj, "letter_spacing"));
	s.wordSpacing = float(obs_data_get_double(obj, "word_spacing"));
	s.language = obs_data_get_string(obj, "language");
	s.underline = obs_data_get_bool(obj, "underline");
	s.strikeout = obs_data_get_bool(obj, "strikeout");

	return s;
}

const HAlign kLegacyAlign[] = {HAlign::Start, HAlign::Center, HAlign::End, HAlign::Justify};

// Converts the schema-1 "document" (every run a full style) into a base style
// plus per-run overrides.
void migrateDocument(obs_data_t *data)
{
	obs_data_t *old = obs_data_get_obj(data, "document");

	if (!old)
		return;

	Document doc;
	bool haveBase = false;

	if (obs_data_array_t *blocks = obs_data_get_array(old, "blocks")) {
		std::vector<std::pair<long long, std::vector<std::pair<std::string, Style>>>> parsed;

		for (size_t i = 0; i < obs_data_array_count(blocks); i++) {
			obs_data_t *blockData = obs_data_array_item(blocks, i);

			std::vector<std::pair<std::string, Style>> runs;

			if (obs_data_array_t *runArray = obs_data_get_array(blockData, "runs")) {
				for (size_t r = 0; r < obs_data_array_count(runArray); r++) {
					obs_data_t *runData = obs_data_array_item(runArray, r);
					obs_data_t *styleData = obs_data_get_obj(runData, "style");

					runs.emplace_back(obs_data_get_string(runData, "text"),
							  legacyStyle(styleData, Style{}));

					obs_data_release(styleData);
					obs_data_release(runData);
				}

				obs_data_array_release(runArray);
			}

			parsed.emplace_back(obs_data_get_int(blockData, "align"), std::move(runs));
			obs_data_release(blockData);
		}

		obs_data_array_release(blocks);

		// The first run's style was what schema 1 mirrored into the flat
		// properties, so it becomes the base.
		for (const auto &block : parsed) {
			if (!block.second.empty()) {
				doc.base = block.second.front().second;
				haveBase = true;
				break;
			}
		}

		if (!parsed.empty())
			doc.paragraph.align = kLegacyAlign[std::clamp<long long>(parsed.front().first, 0, 3)];

		for (const auto &[align, runs] : parsed) {
			Block block;

			const HAlign a = kLegacyAlign[std::clamp<long long>(align, 0, 3)];

			if (a != doc.paragraph.align)
				block.paragraph.align = a;

			for (const auto &[text, style] : runs)
				block.runs.push_back(Run{text, StyleOverride::diff(doc.base, style)});

			doc.blocks.push_back(std::move(block));
		}
	}

	if (obs_data_has_user_value(old, "padding"))
		obs_data_set_double(data, "padding", obs_data_get_double(old, "padding"));

	obs_data_release(old);

	if (haveBase) {
		writeBaseStyle(data, doc.base);

#define SLUGGED_X(name, type, member, key) put(data, key, doc.paragraph.member);
		SLUGGED_PARAGRAPH_FIELDS(SLUGGED_X)
#undef SLUGGED_X
	}

	doc.normalize();
	writeContent(data, doc);
	obs_data_set_string(data, "text", doc.plainText().c_str());
}

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

// One visibility pass for every property whose relevance depends on another.
// A control that visibly does nothing is worse than no control.
bool refreshVisibility(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	auto show = [props](const char *name, bool visible) {
		if (obs_property_t *p = obs_properties_get(props, name))
			obs_property_set_visible(p, visible);
	};

	const bool file = std::strcmp(obs_data_get_string(settings, "mode"), "file") == 0;

	show("text", !file);
	show("file", file);
	show("chatlog", file);
	show("chatlog_lines", file && obs_data_get_bool(settings, "chatlog"));

	const auto fill = enumFromString(obs_data_get_string(settings, "fill"), FillType::Solid);

	show("gradient_color", fill != FillType::Solid);
	show("gradient_angle", fill == FillType::Linear);
	show("gradient_span", fill != FillType::Solid);

	const bool fixed = obs_data_get_bool(settings, "extents");

	show("extents_cx", fixed);
	show("extents_cy", fixed);
	show("wrap", fixed);

	const auto motion = enumFromString(obs_data_get_string(settings, "motion"), Motion::None);
	const auto order = enumFromString(obs_data_get_string(settings, "motion_order"), MotionOrder::PerCharacter);
	const auto trigger = enumFromString(obs_data_get_string(settings, "motion_replay"), MotionTrigger::TextChange);

	const bool animated = motion != Motion::None;
	const bool timed = motionIsTransient(motion);

	show("motion_order", animated);
	show("motion_direction", motionHasDirection(motion));
	show("motion_duration", timed);
	show("motion_stagger", animated && order != MotionOrder::Together);
	show("motion_speed", motion == Motion::Wave);
	show("motion_replay", timed);
	show("motion_scope", timed && trigger == MotionTrigger::TextChange);
	show("motion_exit", timed && trigger == MotionTrigger::TextChange);
	show("motion_loop_interval", timed && trigger == MotionTrigger::Loop);
	show("motion_replay_now", animated);

	// The amount control's meaning, and so its label and range, follow the
	// preset. The stored value is only replaced when it cannot have been meant
	// for this preset: 48 px is a fine slide and a nonsense pop scale.
	const MotionParamSpec amount = motionParamSpec(motion);

	if (obs_property_t *param = obs_properties_get(props, "motion_param")) {
		obs_property_set_visible(param, amount.label != nullptr);

		if (amount.label) {
			obs_property_set_description(param, obs_module_text(amount.label));
			obs_property_float_set_limits(param, amount.min, amount.max, amount.step);

			const double current = obs_data_get_double(settings, "motion_param");

			if (current < amount.min || current > amount.max)
				obs_data_set_double(settings, "motion_param", amount.defaultValue);
		}
	}

	show("scroll_gap", obs_data_get_bool(settings, "scroll_loop"));

	return true;
}

bool callProc(void *data, const char *proc)
{
	auto *source = static_cast<obs_source_t *>(data);

	if (!source)
		return false;

	if (proc_handler_t *handler = obs_source_get_proc_handler(source)) {
		calldata_t call = {};
		proc_handler_call(handler, proc, &call);
		calldata_free(&call);
	}

	return false;
}

bool replayClicked(obs_properties_t *, obs_property_t *, void *data)
{
	return callProc(data, "replay_motion");
}

bool timerToggleClicked(obs_properties_t *, obs_property_t *, void *data)
{
	return callProc(data, "timer_toggle");
}

bool timerResetClicked(obs_properties_t *, obs_property_t *, void *data)
{
	return callProc(data, "timer_reset");
}

bool openEditorClicked(obs_properties_t *, obs_property_t *, void *data)
{
	editor::openFor(static_cast<obs_source_t *>(data));

	return false;
}

template<typename E>
obs_property_t *addEnumList(obs_properties_t *props, const char *key, const char *label,
			    std::initializer_list<std::pair<E, const char *>> items)
{
	obs_property_t *list = obs_properties_add_list(props, key, obs_module_text(label), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);

	for (const auto &[value, text] : items)
		obs_property_list_add_string(list, obs_module_text(text), enumToString(value));

	obs_property_set_modified_callback(list, refreshVisibility);

	return list;
}

obs_properties_t *group(obs_properties_t *props, const char *key, const char *label, obs_group_type type)
{
	obs_properties_t *content = obs_properties_create();
	obs_property_t *g = obs_properties_add_group(props, key, obs_module_text(label), type, content);

	if (type == OBS_GROUP_CHECKABLE)
		obs_property_set_modified_callback(g, refreshVisibility);

	return content;
}

} // namespace

// ---------------------------------------------------------------------------

void migrate(obs_data_t *data)
{
	if (obs_data_get_int(data, kSchemaKey) >= kSchemaVersion)
		return;

	migrateEnum(data, "align", {HAlign::Start, HAlign::Center, HAlign::End, HAlign::Justify});
	migrateEnum(data, "valign", {VAlign::Top, VAlign::Middle, VAlign::Bottom});
	migrateEnum(data, "wrap", {WrapMode::None, WrapMode::Word, WrapMode::Character});
	migrateEnum(data, "motion",
		    {Motion::None, Motion::Fade, Motion::Slide, Motion::Pop, Motion::Typewriter, Motion::Wave,
		     Motion::Blur, Motion::Spin, Motion::Bounce});
	migrateEnum(data, "motion_order",
		    {MotionOrder::Together, MotionOrder::PerCharacter, MotionOrder::PerWord, MotionOrder::PerLine});
	migrateEnum(data, "motion_direction",
		    {MotionDirection::Up, MotionDirection::Down, MotionDirection::Left, MotionDirection::Right});
	migrateEnum(data, "motion_replay",
		    {MotionTrigger::Never, MotionTrigger::TextChange, MotionTrigger::Show, MotionTrigger::Loop});

	const long long mode = legacyInt(data, "mode");

	if (mode >= 0) {
		obs_data_erase(data, "mode");
		obs_data_set_string(data, "mode", mode == 1 ? "file" : "text");
	}

	if (obs_data_has_user_value(data, "outline_size")) {
		obs_data_set_double(data, "outline_width", double(obs_data_get_int(data, "outline_size")) * 0.5);
		obs_data_erase(data, "outline_size");
	}

	migrateDocument(data);

	obs_data_erase(data, "document");
	obs_data_erase(data, "document_revision");
	obs_data_erase(data, "import_from");

	obs_data_set_int(data, kSchemaKey, kSchemaVersion);
}

void defaults(obs_data_t *data)
{
	const Style style;
	const Document doc;

#define SLUGGED_X(name, type, member, key) \
	if (!isFontPickerKey(key))         \
		putDefault(data, key, style.member);
	SLUGGED_STYLE_FIELDS(SLUGGED_X)
#undef SLUGGED_X

	obs_data_t *font = obs_data_create();

	obs_data_set_default_string(font, "face", fontenum::defaultFamily().c_str());
	obs_data_set_default_string(font, "style", "Regular");
	obs_data_set_default_int(font, "size", 64);
	obs_data_set_default_obj(data, "font", font);
	obs_data_release(font);

	obs_data_set_default_int(data, "font_weight", 0);

#define SLUGGED_X(name, type, member, key) putDefault(data, key, doc.paragraph.member);
	SLUGGED_PARAGRAPH_FIELDS(SLUGGED_X)
#undef SLUGGED_X

	obs_data_set_default_string(data, "text", "Slugged");
	obs_data_set_default_bool(data, "markup", false);
	obs_data_set_default_string(data, "mode", "text");
	obs_data_set_default_bool(data, "chatlog", false);
	obs_data_set_default_int(data, "chatlog_lines", 6);

	obs_data_set_default_bool(data, "extents", false);
	obs_data_set_default_int(data, "extents_cx", std::lround(doc.boxWidth));
	obs_data_set_default_int(data, "extents_cy", std::lround(doc.boxHeight));
	putDefault(data, "wrap", doc.wrap);
	putDefault(data, "valign", doc.valign);
	putDefault(data, "padding", doc.padding);

	putDefault(data, "background", doc.background.enabled);
	putDefault(data, "background_color", doc.background.color);
	putDefault(data, "background_radius", doc.background.radius);

	obs_data_set_default_int(data, "opacity", 100);

	const MotionSpec &m = doc.motion;

	putDefault(data, "motion", m.motion);
	putDefault(data, "motion_order", m.order);
	putDefault(data, "motion_direction", m.direction);
	putDefault(data, "motion_duration", m.duration);
	putDefault(data, "motion_stagger", m.stagger);
	putDefault(data, "motion_param", m.param);
	putDefault(data, "motion_speed", m.speed);
	putDefault(data, "motion_replay", m.trigger);
	putDefault(data, "motion_scope", m.scope);
	putDefault(data, "motion_exit", m.exit);
	putDefault(data, "motion_loop_interval", m.loopInterval);

	const ScrollSpec &s = doc.scroll;

	putDefault(data, "scroll", s.enabled);
	putDefault(data, "scroll_x", s.speedX);
	putDefault(data, "scroll_y", s.speedY);
	putDefault(data, "scroll_loop", s.loop);
	putDefault(data, "scroll_gap", s.gap);
}

obs_properties_t *properties(obs_source_t *source)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_button2(props, "open_editor", obs_module_text("OpenEditor"), openEditorClicked, source);

	// ---- text ------------------------------------------------------------
	obs_properties_t *text = group(props, "group_text", "Group.Text", OBS_GROUP_NORMAL);

	obs_property_t *mode = obs_properties_add_list(text, "mode", obs_module_text("Mode"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(mode, obs_module_text("Mode.Text"), "text");
	obs_property_list_add_string(mode, obs_module_text("Mode.File"), "file");
	obs_property_set_modified_callback(mode, refreshVisibility);

	obs_properties_add_text(text, "text", obs_module_text("Text"), OBS_TEXT_MULTILINE);

	obs_properties_add_path(text, "file", obs_module_text("File"), OBS_PATH_FILE, obs_module_text("File.Filter"),
				nullptr);

	obs_property_t *chatlog = obs_properties_add_bool(text, "chatlog", obs_module_text("Chatlog"));
	obs_property_set_modified_callback(chatlog, refreshVisibility);
	obs_properties_add_int(text, "chatlog_lines", obs_module_text("Chatlog.Lines"), 1, 1000, 1);

	obs_property_t *markup = obs_properties_add_bool(text, "markup", obs_module_text("Markup"));
	obs_property_set_long_description(markup, obs_module_text("Markup.Hint"));

	// ---- font and fill ---------------------------------------------------
	obs_properties_t *look = group(props, "group_font", "Group.Font", OBS_GROUP_NORMAL);

	obs_properties_add_font(look, "font", obs_module_text("Font"));

	addEnumList<FillType>(look, "fill", "Fill",
			      {{FillType::Solid, "Fill.Solid"},
			       {FillType::Linear, "Fill.Linear"},
			       {FillType::Radial, "Fill.Radial"}});

	obs_properties_add_color_alpha(look, "color", obs_module_text("Color"));
	obs_properties_add_color_alpha(look, "gradient_color", obs_module_text("Fill.Color2"));
	obs_properties_add_float_slider(look, "gradient_angle", obs_module_text("Fill.Angle"), -180.0, 180.0, 1.0);

	addEnumList<GradientSpan>(look, "gradient_span", "Fill.Span",
				  {{GradientSpan::Text, "Fill.Span.Text"},
				   {GradientSpan::Character, "Fill.Span.Character"}});

	obs_properties_add_int_slider(look, "opacity", obs_module_text("Opacity"), 0, 100, 1);
	obs_properties_add_float(look, "letter_spacing", obs_module_text("LetterSpacing"), -200.0, 200.0, 0.5);

	obs_property_t *digits = obs_properties_add_bool(look, "tabular_digits", obs_module_text("TabularDigits"));
	obs_property_set_long_description(digits, obs_module_text("TabularDigits.Hint"));

	// ---- outline ---------------------------------------------------------
	obs_properties_t *outline = group(props, "outline", "Outline", OBS_GROUP_CHECKABLE);

	obs_properties_add_float_slider(outline, "outline_width", obs_module_text("Outline.Width"), 0.5, 64.0, 0.5);
	obs_properties_add_color_alpha(outline, "outline_color", obs_module_text("Outline.Color"));

	addEnumList<LineJoin>(outline, "outline_join", "Outline.Join",
			      {{LineJoin::Round, "Outline.Join.Round"},
			       {LineJoin::Miter, "Outline.Join.Miter"},
			       {LineJoin::Bevel, "Outline.Join.Bevel"}});

	// ---- shadow ----------------------------------------------------------
	obs_properties_t *shadow = group(props, "shadow", "Shadow", OBS_GROUP_CHECKABLE);

	obs_properties_add_float(shadow, "shadow_x", obs_module_text("Shadow.X"), -200.0, 200.0, 1.0);
	obs_properties_add_float(shadow, "shadow_y", obs_module_text("Shadow.Y"), -200.0, 200.0, 1.0);
	obs_properties_add_color_alpha(shadow, "shadow_color", obs_module_text("Shadow.Color"));

	// ---- layout ----------------------------------------------------------
	obs_properties_t *layout = group(props, "group_layout", "Group.Layout", OBS_GROUP_NORMAL);

	obs_property_t *align = addEnumList<HAlign>(layout, "align", "Align",
						    {{HAlign::Start, "Align.Start"},
						     {HAlign::Center, "Align.Center"},
						     {HAlign::End, "Align.End"},
						     {HAlign::Justify, "Align.Justify"}});
	obs_property_set_long_description(align, obs_module_text("Align.Hint"));

	obs_property_t *valign = addEnumList<VAlign>(
		layout, "valign", "VAlign",
		{{VAlign::Top, "VAlign.Top"}, {VAlign::Middle, "VAlign.Middle"}, {VAlign::Bottom, "VAlign.Bottom"}});
	obs_property_set_long_description(valign, obs_module_text("VAlign.Hint"));

	obs_property_t *extents = obs_properties_add_bool(layout, "extents", obs_module_text("Extents"));
	obs_property_set_modified_callback(extents, refreshVisibility);

	obs_properties_add_int(layout, "extents_cx", obs_module_text("Extents.Width"), 1, 16384, 1);
	obs_properties_add_int(layout, "extents_cy", obs_module_text("Extents.Height"), 1, 16384, 1);

	addEnumList<WrapMode>(layout, "wrap", "Wrap",
			      {{WrapMode::None, "Wrap.None"},
			       {WrapMode::Word, "Wrap.Word"},
			       {WrapMode::Character, "Wrap.Character"}});

	obs_properties_add_float(layout, "padding", obs_module_text("Padding"), 0.0, 1000.0, 1.0);
	obs_properties_add_float_slider(layout, "line_height", obs_module_text("LineHeight"), 0.5, 4.0, 0.05);

	addEnumList<Direction>(layout, "direction", "Direction",
			       {{Direction::Auto, "Direction.Auto"},
				{Direction::LTR, "Direction.LTR"},
				{Direction::RTL, "Direction.RTL"}});

	// ---- background ------------------------------------------------------
	obs_properties_t *background = group(props, "background", "Background", OBS_GROUP_CHECKABLE);

	obs_properties_add_color_alpha(background, "background_color", obs_module_text("Background.Color"));
	obs_properties_add_float_slider(background, "background_radius", obs_module_text("Background.Radius"), 0.0,
					200.0, 1.0);

	// ---- motion ----------------------------------------------------------
	obs_properties_t *motion = group(props, "group_motion", "Group.Motion", OBS_GROUP_NORMAL);

	addEnumList<Motion>(motion, "motion", "Motion",
			    {{Motion::None, "Motion.None"},
			     {Motion::Fade, "Motion.Fade"},
			     {Motion::Slide, "Motion.Slide"},
			     {Motion::Pop, "Motion.Pop"},
			     {Motion::Typewriter, "Motion.Typewriter"},
			     {Motion::Wave, "Motion.Wave"},
			     {Motion::Blur, "Motion.Blur"},
			     {Motion::Spin, "Motion.Spin"},
			     {Motion::Bounce, "Motion.Bounce"}});

	addEnumList<MotionOrder>(motion, "motion_order", "Motion.Order",
				 {{MotionOrder::Together, "Motion.Order.Together"},
				  {MotionOrder::PerCharacter, "Motion.Order.Character"},
				  {MotionOrder::PerWord, "Motion.Order.Word"},
				  {MotionOrder::PerLine, "Motion.Order.Line"}});

	addEnumList<MotionDirection>(motion, "motion_direction", "Motion.Direction",
				     {{MotionDirection::Up, "Motion.Direction.Up"},
				      {MotionDirection::Down, "Motion.Direction.Down"},
				      {MotionDirection::Left, "Motion.Direction.Left"},
				      {MotionDirection::Right, "Motion.Direction.Right"}});

	obs_properties_add_float(motion, "motion_duration", obs_module_text("Motion.Duration"), 0.01, 10.0, 0.01);
	obs_properties_add_float(motion, "motion_stagger", obs_module_text("Motion.Stagger"), 0.0, 2.0, 0.005);

	// Label and range are rewritten per preset by refreshVisibility().
	obs_properties_add_float(motion, "motion_param", obs_module_text("Motion.Amount"), -1000.0, 1000.0, 1.0);

	obs_properties_add_float_slider(motion, "motion_speed", obs_module_text("Motion.Speed"), 0.05, 5.0, 0.05);

	addEnumList<MotionTrigger>(motion, "motion_replay", "Motion.Replay",
				   {{MotionTrigger::Never, "Motion.Replay.Never"},
				    {MotionTrigger::TextChange, "Motion.Replay.TextChange"},
				    {MotionTrigger::Show, "Motion.Replay.Show"},
				    {MotionTrigger::Activate, "Motion.Replay.Activate"},
				    {MotionTrigger::Loop, "Motion.Replay.Loop"}});

	addEnumList<MotionScope>(motion, "motion_scope", "Motion.Scope",
				 {{MotionScope::Changed, "Motion.Scope.Changed"},
				  {MotionScope::All, "Motion.Scope.All"}});

	obs_properties_add_bool(motion, "motion_exit", obs_module_text("Motion.Exit"));

	obs_properties_add_float(motion, "motion_loop_interval", obs_module_text("Motion.Replay.Interval"), 0.1, 3600.0,
				 0.1);

	obs_properties_add_button2(motion, "motion_replay_now", obs_module_text("Motion.ReplayNow"), replayClicked,
				   source);

	// ---- scrolling -------------------------------------------------------
	obs_properties_t *scroll = group(props, "scroll", "Scroll", OBS_GROUP_CHECKABLE);

	obs_properties_add_float(scroll, "scroll_x", obs_module_text("Scroll.X"), -4000.0, 4000.0, 1.0);
	obs_properties_add_float(scroll, "scroll_y", obs_module_text("Scroll.Y"), -4000.0, 4000.0, 1.0);

	obs_property_t *scrollLoop = obs_properties_add_bool(scroll, "scroll_loop", obs_module_text("Scroll.Loop"));
	obs_property_set_modified_callback(scrollLoop, refreshVisibility);

	obs_properties_add_float(scroll, "scroll_gap", obs_module_text("Scroll.Gap"), 0.0, 4000.0, 1.0);

	// ---- variables and timer ---------------------------------------------
	obs_properties_t *variables = group(props, "group_variables", "Group.Variables", OBS_GROUP_NORMAL);

	obs_properties_add_text(variables, "variables_help", obs_module_text("Variables.Help"), OBS_TEXT_INFO);
	obs_properties_add_editable_list(variables, kVariablesKey, obs_module_text("Variables"),
					 OBS_EDITABLE_LIST_TYPE_STRINGS, nullptr, nullptr);
	obs_properties_add_path(variables, "variables_file", obs_module_text("Variables.File"), OBS_PATH_FILE,
				obs_module_text("Variables.File.Filter"), nullptr);

	obs_properties_add_button2(variables, "timer_toggle", obs_module_text("Timer.Toggle"), timerToggleClicked,
				   source);
	obs_properties_add_button2(variables, "timer_reset", obs_module_text("Timer.Reset"), timerResetClicked, source);

	return props;
}

void contentFromData(obs_data_t *data, Document &doc)
{
	doc.blocks.clear();

	obs_data_t *content = obs_data_get_obj(data, kContentKey);

	if (content) {
		if (obs_data_array_t *blocks = obs_data_get_array(content, "blocks")) {
			for (size_t i = 0; i < obs_data_array_count(blocks); i++) {
				obs_data_t *blockData = obs_data_array_item(blocks, i);

				Block block;

				if (obs_data_t *paragraph = obs_data_get_obj(blockData, "paragraph")) {
					block.paragraph = readParagraphOverride(paragraph);
					obs_data_release(paragraph);
				}

				if (obs_data_array_t *runs = obs_data_get_array(blockData, "runs")) {
					for (size_t r = 0; r < obs_data_array_count(runs); r++) {
						obs_data_t *runData = obs_data_array_item(runs, r);

						Run run;
						run.text = obs_data_get_string(runData, "text");

						if (obs_data_t *style = obs_data_get_obj(runData, "style")) {
							run.style = readOverride(style);
							obs_data_release(style);
						}

						block.runs.push_back(std::move(run));
						obs_data_release(runData);
					}

					obs_data_array_release(runs);
				}

				doc.blocks.push_back(std::move(block));
				obs_data_release(blockData);
			}

			obs_data_array_release(blocks);
		}

		obs_data_release(content);
	}

	if (doc.blocks.empty())
		doc.blocks.push_back(Block{});

	doc.normalize();
}

SourceConfig load(obs_data_t *data)
{
	SourceConfig config;
	Document &doc = config.document;

	doc.base = readBaseStyle(data);
	readLayout(data, doc);

	config.text = obs_data_get_string(data, "text");
	config.markup = obs_data_get_bool(data, "markup");

	if (config.markup) {
		doc.blocks = parseMarkup(config.text);
	} else {
		contentFromData(data, doc);

		// Something other than the editor changed `text` (the dialog, a
		// script, obs-websocket). Bring the content in line with a minimal
		// edit, so everything that did not change keeps its styling.
		if (doc.plainText() != config.text) {
			doc.setPlainText(config.text);
			config.contentAdjusted = true;
		}
	}

	config.mode = std::strcmp(obs_data_get_string(data, "mode"), "file") == 0 ? SourceMode::File : SourceMode::Text;

	config.file.path = obs_data_get_string(data, "file");
	config.file.chatlog = obs_data_get_bool(data, "chatlog");
	config.file.chatlogLines = int(obs_data_get_int(data, "chatlog_lines"));

	config.variablesFile.path = obs_data_get_string(data, "variables_file");
	config.variables = variablesFromData(data, kVariablesKey);

	return config;
}

void saveContent(obs_data_t *data, const Document &doc)
{
	writeContent(data, doc);
	obs_data_set_string(data, "text", doc.plainText().c_str());
}

void save(obs_data_t *data, const Document &doc, bool markup)
{
	writeBaseStyle(data, doc.base);
	writeLayout(data, doc);

	if (!markup) {
		writeContent(data, doc);
		obs_data_set_string(data, "text", doc.plainText().c_str());
	}

	obs_data_set_int(data, kSchemaKey, kSchemaVersion);
}

// ---------------------------------------------------------------------------
// Variables
// ---------------------------------------------------------------------------

VariableMap variablesFromData(obs_data_t *data, const char *key)
{
	VariableMap out;

	obs_data_array_t *array = obs_data_get_array(data, key);

	if (!array)
		return out;

	for (size_t i = 0; i < obs_data_array_count(array); i++) {
		obs_data_t *item = obs_data_array_item(array, i);

		const char *value = obs_data_get_string(item, "value");

		std::string name;
		std::string parsed;

		if (value && parseAssignment(value, name, parsed))
			out[name] = std::move(parsed);

		obs_data_release(item);
	}

	obs_data_array_release(array);

	return out;
}

void variablesToData(obs_data_t *data, const char *key, const VariableMap &values)
{
	obs_data_array_t *array = obs_data_array_create();

	for (const auto &[name, value] : values) {
		obs_data_t *item = obs_data_create();

		obs_data_set_string(item, "value", (name + "=" + value).c_str());

		obs_data_array_push_back(array, item);
		obs_data_release(item);
	}

	obs_data_set_array(data, key, array);
	obs_data_array_release(array);
}

namespace {

std::string itemString(obs_data_item_t *item)
{
	const char *value = obs_data_item_get_string(item);

	return value ? value : "";
}

} // namespace

VariableMap parseVariableFile(const std::string &text)
{
	// A leading brace can only be JSON: a variable name may not contain one.
	const size_t first = text.find_first_not_of(" \t\r\n");

	if (first == std::string::npos)
		return {};

	if (text[first] != '{')
		return parseAssignments(text);

	obs_data_t *json = obs_data_create_from_json(text.c_str() + first);

	// A half-written file falls back to the line parser rather than blanking
	// every variable it defines.
	if (!json)
		return parseAssignments(text);

	VariableMap out;

	for (obs_data_item_t *item = obs_data_first(json); item; obs_data_item_next(&item)) {
		const std::string name = normaliseVariableName(obs_data_item_get_name(item));

		if (name.empty())
			continue;

		// Numbers and booleans are as useful in an overlay as strings.
		switch (obs_data_item_gettype(item)) {
		case OBS_DATA_STRING:
			out[name] = itemString(item);
			break;
		case OBS_DATA_NUMBER:
			out[name] = obs_data_item_numtype(item) == OBS_DATA_NUM_INT
					    ? std::to_string(obs_data_item_get_int(item))
					    : std::to_string(obs_data_item_get_double(item));
			break;
		case OBS_DATA_BOOLEAN:
			out[name] = obs_data_item_get_bool(item) ? "true" : "false";
			break;
		default:
			break;
		}
	}

	obs_data_release(json);

	return out;
}

namespace {

const char *const kGlobalVariablesFile = "variables.json";

} // namespace

void loadGlobalVariables()
{
	char *path = obs_module_config_path(kGlobalVariablesFile);

	if (!path)
		return;

	obs_data_t *data = obs_data_create_from_json_file_safe(path, "bak");

	bfree(path);

	if (!data)
		return;

	VariableMap values;

	for (obs_data_item_t *item = obs_data_first(data); item; obs_data_item_next(&item)) {
		const std::string name = normaliseVariableName(obs_data_item_get_name(item));

		if (!name.empty())
			values[name] = itemString(item);
	}

	obs_data_release(data);

	GlobalVariables::replace(values);

	// Loading is not a change that needs writing back.
	GlobalVariables::consumeUnsaved();
}

void saveGlobalVariables()
{
	char *dir = obs_module_config_path(nullptr);

	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}

	char *path = obs_module_config_path(kGlobalVariablesFile);

	if (!path)
		return;

	obs_data_t *data = obs_data_create();

	for (const auto &[name, value] : GlobalVariables::snapshot())
		obs_data_set_string(data, name.c_str(), value.c_str());

	obs_data_save_json_safe(data, path, "tmp", "bak");

	obs_data_release(data);
	bfree(path);
}

} // namespace settings
} // namespace slugged
