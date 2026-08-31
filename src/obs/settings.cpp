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
#include "migrate.hpp"

#include <algorithm>
#include <cmath>

#include <util/platform.h>

namespace slugged {
namespace settings {

const char *const kDocumentRevisionKey = "document_revision";
const char *const kVariablesKey = "variables";

namespace {

const char *kDocumentKey = "document";

// ---- style <-> obs_data ---------------------------------------------------

obs_data_t *fillToData(const Fill &fill)
{
	obs_data_t *data = obs_data_create();

	obs_data_set_int(data, "type", int(fill.type));
	obs_data_set_int(data, "color", fill.color.toObsColor());
	obs_data_set_double(data, "angle", fill.angleDeg);
	obs_data_set_bool(data, "per_glyph", fill.perGlyph);

	obs_data_array_t *stops = obs_data_array_create();

	for (const GradientStop &stop : fill.stops) {
		obs_data_t *item = obs_data_create();

		obs_data_set_double(item, "t", stop.t);
		obs_data_set_int(item, "color", stop.color.toObsColor());

		obs_data_array_push_back(stops, item);
		obs_data_release(item);
	}

	obs_data_set_array(data, "stops", stops);
	obs_data_array_release(stops);

	return data;
}

void fillFromData(obs_data_t *data, Fill &fill)
{
	if (!data)
		return;

	fill.type = Fill::Type(obs_data_get_int(data, "type"));
	fill.color = Rgba::fromObsColor(uint32_t(obs_data_get_int(data, "color")));
	fill.angleDeg = float(obs_data_get_double(data, "angle"));
	fill.perGlyph = obs_data_get_bool(data, "per_glyph");

	fill.stops.clear();

	obs_data_array_t *stops = obs_data_get_array(data, "stops");

	if (stops) {
		const size_t count = obs_data_array_count(stops);

		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(stops, i);

			GradientStop stop;

			stop.t = float(obs_data_get_double(item, "t"));
			stop.color = Rgba::fromObsColor(uint32_t(obs_data_get_int(item, "color")));

			fill.stops.push_back(stop);

			obs_data_release(item);
		}

		obs_data_array_release(stops);
	}
}

obs_data_t *styleToData(const Style &style)
{
	obs_data_t *data = obs_data_create();

	obs_data_set_string(data, "family", style.font.family.c_str());
	obs_data_set_string(data, "file", style.font.file.c_str());
	obs_data_set_int(data, "face_index", style.font.faceIndex);
	obs_data_set_int(data, "weight", style.font.weight);
	obs_data_set_bool(data, "italic", style.font.italic);
	obs_data_set_double(data, "size", style.sizePx);

	obs_data_t *axes = obs_data_create();

	for (const auto &[tag, value] : style.font.axes)
		obs_data_set_double(axes, tag.c_str(), value);

	obs_data_set_obj(data, "axes", axes);
	obs_data_release(axes);

	obs_data_t *fill = fillToData(style.fill);
	obs_data_set_obj(data, "fill", fill);
	obs_data_release(fill);

	obs_data_set_bool(data, "outline", style.outline.enabled);
	obs_data_set_double(data, "outline_width", style.outline.widthPx);
	obs_data_set_int(data, "outline_color", style.outline.color.toObsColor());
	obs_data_set_int(data, "outline_join", int(style.outline.join));
	obs_data_set_int(data, "outline_cap", int(style.outline.cap));

	obs_data_set_bool(data, "shadow", style.shadow.enabled);
	obs_data_set_double(data, "shadow_x", style.shadow.offsetX);
	obs_data_set_double(data, "shadow_y", style.shadow.offsetY);
	obs_data_set_int(data, "shadow_color", style.shadow.color.toObsColor());

	obs_data_set_double(data, "letter_spacing", style.letterSpacing);
	obs_data_set_double(data, "word_spacing", style.wordSpacing);
	obs_data_set_string(data, "language", style.language.c_str());

	obs_data_set_bool(data, "underline", style.underline);
	obs_data_set_bool(data, "strikeout", style.strikeout);

	return data;
}

void styleFromData(obs_data_t *data, Style &style)
{
	if (!data)
		return;

	style.font.family = obs_data_get_string(data, "family");
	style.font.file = obs_data_get_string(data, "file");
	style.font.faceIndex = int(obs_data_get_int(data, "face_index"));
	style.font.weight = int(obs_data_get_int(data, "weight"));
	style.font.italic = obs_data_get_bool(data, "italic");
	style.sizePx = float(obs_data_get_double(data, "size"));

	if (style.sizePx <= 0.0f)
		style.sizePx = 64.0f;

	if (style.font.weight <= 0)
		style.font.weight = 400;

	style.font.axes.clear();

	obs_data_t *axes = obs_data_get_obj(data, "axes");

	if (axes) {
		for (obs_data_item_t *item = obs_data_first(axes); item; obs_data_item_next(&item))
			style.font.axes[obs_data_item_get_name(item)] = float(obs_data_item_get_double(item));

		obs_data_release(axes);
	}

	obs_data_t *fill = obs_data_get_obj(data, "fill");
	fillFromData(fill, style.fill);
	obs_data_release(fill);

	style.outline.enabled = obs_data_get_bool(data, "outline");
	style.outline.widthPx = float(obs_data_get_double(data, "outline_width"));
	style.outline.color = Rgba::fromObsColor(uint32_t(obs_data_get_int(data, "outline_color")));
	style.outline.join = LineJoin(obs_data_get_int(data, "outline_join"));
	style.outline.cap = LineCap(obs_data_get_int(data, "outline_cap"));

	style.shadow.enabled = obs_data_get_bool(data, "shadow");
	style.shadow.offsetX = float(obs_data_get_double(data, "shadow_x"));
	style.shadow.offsetY = float(obs_data_get_double(data, "shadow_y"));
	style.shadow.color = Rgba::fromObsColor(uint32_t(obs_data_get_int(data, "shadow_color")));

	style.letterSpacing = float(obs_data_get_double(data, "letter_spacing"));
	style.wordSpacing = float(obs_data_get_double(data, "word_spacing"));
	style.language = obs_data_get_string(data, "language");

	style.underline = obs_data_get_bool(data, "underline");
	style.strikeout = obs_data_get_bool(data, "strikeout");
}

} // namespace

obs_data_t *documentToData(const Document &doc)
{
	obs_data_t *data = obs_data_create();

	obs_data_array_t *blocks = obs_data_array_create();

	for (const Block &block : doc.blocks) {
		obs_data_t *blockData = obs_data_create();

		obs_data_set_int(blockData, "align", int(block.align));
		obs_data_set_int(blockData, "direction", int(block.direction));
		obs_data_set_double(blockData, "line_height", block.lineHeight);
		obs_data_set_double(blockData, "space_before", block.spaceBefore);
		obs_data_set_double(blockData, "space_after", block.spaceAfter);

		obs_data_array_t *runs = obs_data_array_create();

		for (const Run &run : block.runs) {
			obs_data_t *runData = obs_data_create();

			obs_data_set_string(runData, "text", run.text.c_str());

			obs_data_t *style = styleToData(run.style);
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

	obs_data_set_array(data, "blocks", blocks);
	obs_data_array_release(blocks);

	obs_data_set_int(data, "size_mode", int(doc.sizeMode));
	obs_data_set_double(data, "box_width", doc.boxWidth);
	obs_data_set_double(data, "box_height", doc.boxHeight);
	obs_data_set_int(data, "wrap", int(doc.wrap));
	obs_data_set_int(data, "valign", int(doc.valign));
	obs_data_set_double(data, "padding", doc.padding);

	obs_data_set_bool(data, "background", doc.backgroundEnabled);
	obs_data_set_int(data, "background_color", doc.backgroundColor.toObsColor());

	obs_data_set_int(data, "motion", int(doc.motion.motion));
	obs_data_set_int(data, "motion_order", int(doc.motion.order));
	obs_data_set_int(data, "motion_direction", int(doc.motion.direction));
	obs_data_set_double(data, "motion_duration", doc.motion.duration);
	obs_data_set_double(data, "motion_stagger", doc.motion.stagger);
	obs_data_set_double(data, "motion_param", doc.motion.param);
	obs_data_set_double(data, "motion_speed", doc.motion.speed);
	obs_data_set_int(data, "motion_replay", int(doc.motion.trigger));
	obs_data_set_double(data, "motion_loop_interval", doc.motion.loopInterval);

	obs_data_set_bool(data, "scroll", doc.scroll.enabled);
	obs_data_set_double(data, "scroll_x", doc.scroll.speedX);
	obs_data_set_double(data, "scroll_y", doc.scroll.speedY);
	obs_data_set_bool(data, "scroll_loop", doc.scroll.loop);
	obs_data_set_double(data, "scroll_gap", doc.scroll.gap);

	obs_data_set_double(data, "opacity", doc.opacity);

	obs_data_t *defaultStyle = styleToData(doc.defaultStyle);
	obs_data_set_obj(data, "default_style", defaultStyle);
	obs_data_release(defaultStyle);

	return data;
}

void documentFromData(obs_data_t *data, Document &doc)
{
	if (!data)
		return;

	doc.blocks.clear();

	obs_data_t *defaultStyle = obs_data_get_obj(data, "default_style");

	if (defaultStyle) {
		styleFromData(defaultStyle, doc.defaultStyle);
		obs_data_release(defaultStyle);
	}

	obs_data_array_t *blocks = obs_data_get_array(data, "blocks");

	if (blocks) {
		const size_t blockCount = obs_data_array_count(blocks);

		for (size_t i = 0; i < blockCount; i++) {
			obs_data_t *blockData = obs_data_array_item(blocks, i);

			Block block;

			block.align = HAlign(obs_data_get_int(blockData, "align"));
			block.direction = Direction(obs_data_get_int(blockData, "direction"));
			block.lineHeight = float(obs_data_get_double(blockData, "line_height"));
			block.spaceBefore = float(obs_data_get_double(blockData, "space_before"));
			block.spaceAfter = float(obs_data_get_double(blockData, "space_after"));

			if (block.lineHeight <= 0.0f)
				block.lineHeight = 1.0f;

			obs_data_array_t *runs = obs_data_get_array(blockData, "runs");

			if (runs) {
				const size_t runCount = obs_data_array_count(runs);

				for (size_t r = 0; r < runCount; r++) {
					obs_data_t *runData = obs_data_array_item(runs, r);

					Run run;

					run.text = obs_data_get_string(runData, "text");
					run.style = doc.defaultStyle;

					obs_data_t *style = obs_data_get_obj(runData, "style");
					styleFromData(style, run.style);
					obs_data_release(style);

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

	doc.sizeMode = SizeMode(obs_data_get_int(data, "size_mode"));
	doc.boxWidth = float(obs_data_get_double(data, "box_width"));
	doc.boxHeight = float(obs_data_get_double(data, "box_height"));
	doc.wrap = WrapMode(obs_data_get_int(data, "wrap"));
	doc.valign = VAlign(obs_data_get_int(data, "valign"));
	doc.padding = float(obs_data_get_double(data, "padding"));

	doc.backgroundEnabled = obs_data_get_bool(data, "background");
	doc.backgroundColor = Rgba::fromObsColor(uint32_t(obs_data_get_int(data, "background_color")));

	doc.motion.motion = Motion(obs_data_get_int(data, "motion"));
	doc.motion.order = MotionOrder(obs_data_get_int(data, "motion_order"));
	doc.motion.direction = MotionDirection(obs_data_get_int(data, "motion_direction"));
	doc.motion.duration = float(obs_data_get_double(data, "motion_duration"));
	doc.motion.stagger = float(obs_data_get_double(data, "motion_stagger"));
	doc.motion.param = float(obs_data_get_double(data, "motion_param"));

	// A document written before wave speed existed stores nothing here, and a
	// wave standing perfectly still is not a sensible reading of "absent".
	if (obs_data_has_user_value(data, "motion_speed"))
		doc.motion.speed = float(obs_data_get_double(data, "motion_speed"));

	// This key used to be the boolean replayOnChange. obs_data reads a stored
	// bool back as 0 or 1, and Never and TextChange are the first two triggers,
	// so an old document migrates to exactly the behaviour it had.
	doc.motion.trigger = MotionTrigger(obs_data_get_int(data, "motion_replay"));

	if (obs_data_has_user_value(data, "motion_loop_interval"))
		doc.motion.loopInterval = float(obs_data_get_double(data, "motion_loop_interval"));

	doc.scroll.enabled = obs_data_get_bool(data, "scroll");
	doc.scroll.speedX = float(obs_data_get_double(data, "scroll_x"));
	doc.scroll.speedY = float(obs_data_get_double(data, "scroll_y"));
	doc.scroll.loop = obs_data_get_bool(data, "scroll_loop");
	doc.scroll.gap = float(obs_data_get_double(data, "scroll_gap"));

	// Fully transparent is a legitimate setting, so a missing key -- a document
	// written before opacity was stored -- is what falls back to opaque, not a
	// stored zero.
	doc.opacity = obs_data_has_user_value(data, "opacity") ? float(obs_data_get_double(data, "opacity")) : 1.0f;
	doc.opacity = std::clamp(doc.opacity, 0.0f, 1.0f);

	if (doc.motion.duration <= 0.0f)
		doc.motion.duration = 0.35f;

	if (doc.motion.loopInterval <= 0.0f)
		doc.motion.loopInterval = 5.0f;
}

// ---------------------------------------------------------------------------

VariableMap variablesFromData(obs_data_t *data, const char *key)
{
	VariableMap out;

	obs_data_array_t *array = obs_data_get_array(data, key);

	if (!array)
		return out;

	const size_t count = obs_data_array_count(array);

	for (size_t i = 0; i < count; i++) {
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

// obs_data_item_get_string returns null for an item that is not a string, and a
// hand-edited file has no obligation to hold only strings.
std::string itemString(obs_data_item_t *item)
{
	const char *value = obs_data_item_get_string(item);

	return value ? value : "";
}

} // namespace

VariableMap parseVariableFile(const std::string &text)
{
	// A leading brace can only be JSON: a "name=value" line cannot start with
	// one, because a brace is exactly what a variable name may not contain.
	const size_t first = text.find_first_not_of(" \t\r\n");

	if (first == std::string::npos)
		return {};

	if (text[first] != '{')
		return parseAssignments(text);

	obs_data_t *json = obs_data_create_from_json(text.c_str() + first);

	if (!json) {
		// Malformed JSON falls back to the line parser rather than being
		// dropped: a half-written file being appended to by a bot should not
		// blank every variable it defines.
		return parseAssignments(text);
	}

	VariableMap out;

	for (obs_data_item_t *item = obs_data_first(json); item; obs_data_item_next(&item)) {
		const std::string name = normaliseVariableName(obs_data_item_get_name(item));

		if (name.empty())
			continue;

		// Numbers and booleans are as useful in an overlay as strings, and a
		// bot writing JSON has no reason to quote a follower count.
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

void defaults(obs_data_t *data)
{
	obs_data_set_default_string(data, "text", "Slugged");
	obs_data_set_default_int(data, "mode", 0);

	obs_data_t *font = obs_data_create();

	obs_data_set_default_string(font, "face", "Sans Serif");
	obs_data_set_default_int(font, "size", 64);
	obs_data_set_default_obj(data, "font", font);
	obs_data_release(font);

	obs_data_set_default_int(data, "color", 0xFFFFFFFF);
	obs_data_set_default_int(data, "opacity", 100);

	obs_data_set_default_bool(data, "outline", false);
	obs_data_set_default_int(data, "outline_size", 2);
	obs_data_set_default_int(data, "outline_color", 0xFF000000);

	obs_data_set_default_bool(data, "shadow", false);
	obs_data_set_default_double(data, "shadow_x", 2.0);
	obs_data_set_default_double(data, "shadow_y", 2.0);
	obs_data_set_default_int(data, "shadow_color", 0x80000000);

	obs_data_set_default_int(data, "align", 0);
	obs_data_set_default_int(data, "valign", 0);

	obs_data_set_default_bool(data, "background", false);
	obs_data_set_default_int(data, "background_color", 0x80000000);

	obs_data_set_default_bool(data, "extents", false);
	obs_data_set_default_int(data, "extents_cx", 640);
	obs_data_set_default_int(data, "extents_cy", 200);
	obs_data_set_default_int(data, "wrap", int(WrapMode::Word));

	obs_data_set_default_bool(data, "chatlog", false);
	obs_data_set_default_int(data, "chatlog_lines", 6);

	obs_data_set_default_int(data, "motion", 0);
	obs_data_set_default_int(data, "motion_order", int(MotionOrder::PerGlyph));
	obs_data_set_default_int(data, "motion_direction", int(MotionDirection::Up));
	obs_data_set_default_double(data, "motion_duration", 0.35);
	obs_data_set_default_double(data, "motion_stagger", 0.03);
	obs_data_set_default_double(data, "motion_param", motionParamSpec(Motion::Slide).defaultValue);
	obs_data_set_default_double(data, "motion_speed", 0.5);
	obs_data_set_default_int(data, "motion_replay", int(MotionTrigger::TextChange));
	obs_data_set_default_double(data, "motion_loop_interval", 5.0);

	obs_data_set_default_bool(data, "scroll", false);
	obs_data_set_default_double(data, "scroll_x", 0.0);
	obs_data_set_default_double(data, "scroll_y", 0.0);
	obs_data_set_default_bool(data, "scroll_loop", true);
	obs_data_set_default_double(data, "scroll_gap", 64.0);
}

namespace {

bool modeIsFile(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);

	const bool file = obs_data_get_int(settings, "mode") == 1;

	obs_property_set_visible(obs_properties_get(props, "file"), file);
	obs_property_set_visible(obs_properties_get(props, "chatlog"), file);
	obs_property_set_visible(obs_properties_get(props, "chatlog_lines"), file);
	obs_property_set_visible(obs_properties_get(props, "text"), !file);

	return true;
}

bool extentsChanged(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);

	const bool fixed = obs_data_get_bool(settings, "extents");

	obs_property_set_visible(obs_properties_get(props, "extents_cx"), fixed);
	obs_property_set_visible(obs_properties_get(props, "extents_cy"), fixed);
	obs_property_set_visible(obs_properties_get(props, "wrap"), fixed);

	// Vertical alignment used to be hidden along with these, which made it look
	// like the setting did not exist rather than like it needed a box. It stays
	// visible: the overlay filter lays out against the source it is attached to
	// and so has a height to align within even with no fixed size of its own.

	return true;
}

bool shadowChanged(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);

	const bool shadow = obs_data_get_bool(settings, "shadow");

	obs_property_set_visible(obs_properties_get(props, "shadow_x"), shadow);
	obs_property_set_visible(obs_properties_get(props, "shadow_y"), shadow);
	obs_property_set_visible(obs_properties_get(props, "shadow_color"), shadow);

	return true;
}

bool outlineChanged(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);

	const bool outline = obs_data_get_bool(settings, "outline");

	obs_property_set_visible(obs_properties_get(props, "outline_size"), outline);
	obs_property_set_visible(obs_properties_get(props, "outline_color"), outline);

	return true;
}

bool backgroundChanged(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);

	obs_property_set_visible(obs_properties_get(props, "background_color"),
				 obs_data_get_bool(settings, "background"));

	return true;
}

// Each motion preset uses a different subset of the parameters below it, and a
// control that visibly does nothing is worse than no control at all.
//
// The amount control is more than shown or hidden: what it means changes with
// the preset, so its label, its range and its default follow. Sharing one
// -500..500 "Amount" across every preset is what made Pop useless, since Pop
// reads it as a scale and the shared default of 24 started every glyph
// twenty-four times too big.
bool motionChanged(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);

	const auto motion = Motion(obs_data_get_int(settings, "motion"));
	const auto order = MotionOrder(obs_data_get_int(settings, "motion_order"));
	const auto trigger = MotionTrigger(obs_data_get_int(settings, "motion_replay"));

	const bool animated = motion != Motion::None;

	// Wave never settles, so it has no transition to time; it does still take a
	// stagger, which is what spreads the ripple along the text.
	const bool timed = motionIsTransient(motion);

	const MotionParamSpec amount = motionParamSpec(motion);

	obs_property_set_visible(obs_properties_get(props, "motion_order"), animated);
	obs_property_set_visible(obs_properties_get(props, "motion_direction"), motionHasDirection(motion));
	obs_property_set_visible(obs_properties_get(props, "motion_duration"), timed);
	obs_property_set_visible(obs_properties_get(props, "motion_stagger"),
				 animated && order != MotionOrder::Together);
	obs_property_set_visible(obs_properties_get(props, "motion_speed"), motion == Motion::Wave);
	obs_property_set_visible(obs_properties_get(props, "motion_replay"), timed);
	obs_property_set_visible(obs_properties_get(props, "motion_loop_interval"),
				 timed && trigger == MotionTrigger::Loop);
	obs_property_set_visible(obs_properties_get(props, "motion_replay_now"), animated);

	obs_property_t *param = obs_properties_get(props, "motion_param");

	obs_property_set_visible(param, amount.label != nullptr);

	if (amount.label) {
		obs_property_set_description(param, obs_module_text(amount.label));
		obs_property_float_set_limits(param, amount.min, amount.max, amount.step);

		// Only rewrite the stored value when the preset it was tuned for
		// cannot have been this one. A distance of 48 px is a legitimate
		// slide and a nonsensical pop scale, so carrying it over would look
		// like the control had broken rather than changed meaning.
		const double current = obs_data_get_double(settings, "motion_param");

		if (current < amount.min || current > amount.max)
			obs_data_set_double(settings, "motion_param", amount.defaultValue);
	}

	return true;
}

bool scrollChanged(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);

	const bool scrolling = obs_data_get_bool(settings, "scroll");
	const bool loop = scrolling && obs_data_get_bool(settings, "scroll_loop");

	obs_property_set_visible(obs_properties_get(props, "scroll_x"), scrolling);
	obs_property_set_visible(obs_properties_get(props, "scroll_y"), scrolling);
	obs_property_set_visible(obs_properties_get(props, "scroll_loop"), scrolling);
	obs_property_set_visible(obs_properties_get(props, "scroll_gap"), loop);

	return true;
}

// Replays the one-shot presets on demand. Routed through the source's own proc
// handler rather than reaching into SluggedSource from here, so the same entry
// point serves the button, the hotkey and any script that wants to trigger an
// intro on cue.
bool replayClicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);

	auto *source = static_cast<obs_source_t *>(data);

	if (!source)
		return false;

	proc_handler_t *handler = obs_source_get_proc_handler(source);

	if (!handler)
		return false;

	calldata_t call = {};

	proc_handler_call(handler, "replay_motion", &call);
	calldata_free(&call);

	return false;
}

bool openEditorClicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);

	editor::openFor(static_cast<obs_source_t *>(data));

	return false;
}

// Applying the import rewrites the flat properties, so the dialog has to be
// refreshed for the user to see the imported values.
bool importClicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(property);

	auto *source = static_cast<obs_source_t *>(data);

	if (!source)
		return false;

	obs_data_t *settings = obs_source_get_settings(source);

	if (!settings)
		return false;

	const char *name = obs_data_get_string(settings, "import_from");

	bool changed = false;

	if (name && *name && migrate::importInto(settings, name)) {
		obs_source_update(source, settings);
		changed = true;
	}

	obs_data_release(settings);

	UNUSED_PARAMETER(props);

	return changed;
}

} // namespace

obs_properties_t *properties(void *sourceData)
{
	obs_properties_t *props = obs_properties_create();

	// The editor is the headline way to author a Slugged source; the flat
	// properties below stay available so the source is still fully usable from
	// the standard dialog, from scripts, and from obs-websocket.
	obs_properties_add_button2(props, "open_editor", obs_module_text("OpenEditor"), openEditorClicked, sourceData);

	// One-click adoption of an existing GDI+ or FreeType2 text source, so
	// switching an overlay over does not mean rebuilding every text element.
	obs_property_t *importList = obs_properties_add_list(props, "import_from", obs_module_text("Import"),
							     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	migrate::fillImportList(importList);

	obs_properties_add_button2(props, "import_apply", obs_module_text("Import.Apply"), importClicked, sourceData);

	obs_property_t *mode = obs_properties_add_list(props, "mode", obs_module_text("Mode"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(mode, obs_module_text("Mode.Document"), 0);
	obs_property_list_add_int(mode, obs_module_text("Mode.File"), 1);
	obs_property_set_modified_callback(mode, modeIsFile);

	obs_properties_add_text(props, "text", obs_module_text("Text"), OBS_TEXT_MULTILINE);

	obs_properties_add_path(props, "file", obs_module_text("File"), OBS_PATH_FILE, obs_module_text("File.Filter"),
				nullptr);

	obs_properties_add_bool(props, "chatlog", obs_module_text("Chatlog"));
	obs_properties_add_int(props, "chatlog_lines", obs_module_text("Chatlog.Lines"), 1, 1000, 1);

	obs_properties_add_font(props, "font", obs_module_text("Font"));

	obs_properties_add_color_alpha(props, "color", obs_module_text("Color"));
	obs_properties_add_int_slider(props, "opacity", obs_module_text("Opacity"), 0, 100, 1);

	obs_property_t *outline = obs_properties_add_bool(props, "outline", obs_module_text("Outline"));
	obs_property_set_modified_callback(outline, outlineChanged);

	obs_properties_add_int(props, "outline_size", obs_module_text("Outline.Size"), 1, 64, 1);
	obs_properties_add_color_alpha(props, "outline_color", obs_module_text("Outline.Color"));

	obs_property_t *shadow = obs_properties_add_bool(props, "shadow", obs_module_text("Shadow"));
	obs_property_set_modified_callback(shadow, shadowChanged);

	obs_properties_add_float(props, "shadow_x", obs_module_text("Shadow.X"), -200.0, 200.0, 1.0);
	obs_properties_add_float(props, "shadow_y", obs_module_text("Shadow.Y"), -200.0, 200.0, 1.0);
	obs_properties_add_color_alpha(props, "shadow_color", obs_module_text("Shadow.Color"));

	obs_property_t *align = obs_properties_add_list(props, "align", obs_module_text("Align"), OBS_COMBO_TYPE_LIST,
							OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(align, obs_module_text("Align.Left"), int(HAlign::Left));
	obs_property_list_add_int(align, obs_module_text("Align.Center"), int(HAlign::Center));
	obs_property_list_add_int(align, obs_module_text("Align.Right"), int(HAlign::Right));
	obs_property_list_add_int(align, obs_module_text("Align.Justify"), int(HAlign::Justify));

	// Alignment needs somewhere to align within, and an auto-sized source is
	// exactly as wide as its own text. Saying so here is the difference between
	// a control that looks broken and one that is waiting on a box.
	obs_property_set_long_description(align, obs_module_text("Align.Hint"));

	obs_property_t *valign = obs_properties_add_list(props, "valign", obs_module_text("VAlign"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(valign, obs_module_text("VAlign.Top"), int(VAlign::Top));
	obs_property_list_add_int(valign, obs_module_text("VAlign.Middle"), int(VAlign::Middle));
	obs_property_list_add_int(valign, obs_module_text("VAlign.Bottom"), int(VAlign::Bottom));

	obs_property_set_long_description(valign, obs_module_text("VAlign.Hint"));

	obs_property_t *background = obs_properties_add_bool(props, "background", obs_module_text("Background"));
	obs_property_set_modified_callback(background, backgroundChanged);

	obs_properties_add_color_alpha(props, "background_color", obs_module_text("Background.Color"));

	obs_property_t *extents = obs_properties_add_bool(props, "extents", obs_module_text("Extents"));
	obs_property_set_modified_callback(extents, extentsChanged);

	obs_properties_add_int(props, "extents_cx", obs_module_text("Extents.Width"), 1, 16384, 1);
	obs_properties_add_int(props, "extents_cy", obs_module_text("Extents.Height"), 1, 16384, 1);

	obs_property_t *wrap = obs_properties_add_list(props, "wrap", obs_module_text("Wrap"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(wrap, obs_module_text("Wrap.None"), int(WrapMode::None));
	obs_property_list_add_int(wrap, obs_module_text("Wrap.Word"), int(WrapMode::Word));
	obs_property_list_add_int(wrap, obs_module_text("Wrap.Character"), int(WrapMode::Character));

	obs_property_t *motion = obs_properties_add_list(props, "motion", obs_module_text("Motion"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(motion, obs_module_text("Motion.None"), int(Motion::None));
	obs_property_list_add_int(motion, obs_module_text("Motion.Fade"), int(Motion::Fade));
	obs_property_list_add_int(motion, obs_module_text("Motion.Slide"), int(Motion::Slide));
	obs_property_list_add_int(motion, obs_module_text("Motion.Pop"), int(Motion::Pop));
	obs_property_list_add_int(motion, obs_module_text("Motion.Typewriter"), int(Motion::Typewriter));
	obs_property_list_add_int(motion, obs_module_text("Motion.Wave"), int(Motion::Wave));
	obs_property_list_add_int(motion, obs_module_text("Motion.Blur"), int(Motion::Blur));
	obs_property_list_add_int(motion, obs_module_text("Motion.Rotate"), int(Motion::Rotate));
	obs_property_list_add_int(motion, obs_module_text("Motion.Bounce"), int(Motion::Bounce));

	obs_property_set_modified_callback(motion, motionChanged);

	obs_property_t *order = obs_properties_add_list(props, "motion_order", obs_module_text("Motion.Order"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(order, obs_module_text("Motion.Order.Together"), int(MotionOrder::Together));
	obs_property_list_add_int(order, obs_module_text("Motion.Order.Glyph"), int(MotionOrder::PerGlyph));
	obs_property_list_add_int(order, obs_module_text("Motion.Order.Word"), int(MotionOrder::PerWord));
	obs_property_list_add_int(order, obs_module_text("Motion.Order.Line"), int(MotionOrder::PerLine));

	// The order decides whether a stagger delay means anything, so it drives the
	// same visibility pass as the preset itself.
	obs_property_set_modified_callback(order, motionChanged);

	obs_property_t *direction = obs_properties_add_list(props, "motion_direction",
							    obs_module_text("Motion.Direction"), OBS_COMBO_TYPE_LIST,
							    OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(direction, obs_module_text("Motion.Direction.Up"), int(MotionDirection::Up));
	obs_property_list_add_int(direction, obs_module_text("Motion.Direction.Down"), int(MotionDirection::Down));
	obs_property_list_add_int(direction, obs_module_text("Motion.Direction.Left"), int(MotionDirection::Left));
	obs_property_list_add_int(direction, obs_module_text("Motion.Direction.Right"), int(MotionDirection::Right));

	obs_properties_add_float(props, "motion_duration", obs_module_text("Motion.Duration"), 0.01, 10.0, 0.01);
	obs_properties_add_float(props, "motion_stagger", obs_module_text("Motion.Stagger"), 0.0, 2.0, 0.005);

	// The label and range of this one are rewritten per preset by
	// motionChanged(); what is set here is only what an unconfigured source
	// starts out showing.
	obs_properties_add_float(props, "motion_param", obs_module_text("Motion.Amount"), -1000.0, 1000.0, 1.0);

	obs_properties_add_float_slider(props, "motion_speed", obs_module_text("Motion.Speed"), 0.05, 5.0, 0.05);

	// Which of these fires decides whether a one-shot preset is ever seen
	// again after the source is created.
	obs_property_t *replay = obs_properties_add_list(props, "motion_replay", obs_module_text("Motion.Replay"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(replay, obs_module_text("Motion.Replay.Never"), int(MotionTrigger::Never));
	obs_property_list_add_int(replay, obs_module_text("Motion.Replay.TextChange"), int(MotionTrigger::TextChange));
	obs_property_list_add_int(replay, obs_module_text("Motion.Replay.Show"), int(MotionTrigger::Show));
	obs_property_list_add_int(replay, obs_module_text("Motion.Replay.Loop"), int(MotionTrigger::Loop));

	obs_property_set_modified_callback(replay, motionChanged);

	obs_properties_add_float(props, "motion_loop_interval", obs_module_text("Motion.Replay.Interval"), 0.1, 3600.0,
				 0.1);

	obs_properties_add_button2(props, "motion_replay_now", obs_module_text("Motion.ReplayNow"), replayClicked,
				   sourceData);

	// ---- continuous scrolling -------------------------------------------
	obs_property_t *scroll = obs_properties_add_bool(props, "scroll", obs_module_text("Scroll"));

	obs_property_set_modified_callback(scroll, scrollChanged);

	obs_properties_add_float(props, "scroll_x", obs_module_text("Scroll.X"), -2000.0, 2000.0, 1.0);
	obs_properties_add_float(props, "scroll_y", obs_module_text("Scroll.Y"), -2000.0, 2000.0, 1.0);

	obs_property_t *scrollLoop = obs_properties_add_bool(props, "scroll_loop", obs_module_text("Scroll.Loop"));

	obs_property_set_modified_callback(scrollLoop, scrollChanged);

	obs_properties_add_float(props, "scroll_gap", obs_module_text("Scroll.Gap"), 0.0, 4000.0, 1.0);

	// ---- variables -------------------------------------------------------
	//
	// Anything written {name} in the text is replaced at render time. The list
	// is plain "name=value" strings so that a script, obs-websocket or
	// Streamer.bot can rewrite it with SetInputSettings without knowing
	// anything Slugged-specific.
	obs_properties_add_text(props, "variables_help", obs_module_text("Variables.Help"), OBS_TEXT_INFO);

	obs_properties_add_editable_list(props, kVariablesKey, obs_module_text("Variables"),
					 OBS_EDITABLE_LIST_TYPE_STRINGS, nullptr, nullptr);

	obs_properties_add_path(props, "variables_file", obs_module_text("Variables.File"), OBS_PATH_FILE,
				obs_module_text("Variables.File.Filter"), nullptr);

	return props;
}

void load(obs_data_t *data, Document &doc, SettingsSnapshot &snapshot)
{
	obs_data_t *documentData = obs_data_get_obj(data, kDocumentKey);

	const bool hasDocument = documentData != nullptr;

	if (documentData) {
		documentFromData(documentData, doc);
		obs_data_release(documentData);
	}

	// ---- read the flat properties ------------------------------------
	SettingsSnapshot current;

	current.text = obs_data_get_string(data, "text");

	obs_data_t *font = obs_data_get_obj(data, "font");

	if (font) {
		current.fontFace = obs_data_get_string(font, "face");
		current.fontStyle = obs_data_get_string(font, "style");
		current.fontSize = int(obs_data_get_int(font, "size"));
		current.fontFlags = uint32_t(obs_data_get_int(font, "flags"));

		obs_data_release(font);
	}

	current.color = uint32_t(obs_data_get_int(data, "color"));
	current.opacity = int(obs_data_get_int(data, "opacity"));
	current.outline = obs_data_get_bool(data, "outline");
	current.outlineSize = int(obs_data_get_int(data, "outline_size"));
	current.outlineColor = uint32_t(obs_data_get_int(data, "outline_color"));
	current.shadow = obs_data_get_bool(data, "shadow");
	current.shadowX = obs_data_get_double(data, "shadow_x");
	current.shadowY = obs_data_get_double(data, "shadow_y");
	current.shadowColor = uint32_t(obs_data_get_int(data, "shadow_color"));
	current.align = int(obs_data_get_int(data, "align"));
	current.valign = int(obs_data_get_int(data, "valign"));
	current.background = obs_data_get_bool(data, "background");
	current.backgroundColor = uint32_t(obs_data_get_int(data, "background_color"));
	current.extents = obs_data_get_bool(data, "extents");
	current.extentsWidth = int(obs_data_get_int(data, "extents_cx"));
	current.extentsHeight = int(obs_data_get_int(data, "extents_cy"));
	current.wrap = int(obs_data_get_int(data, "wrap"));
	current.documentRevision = obs_data_get_int(data, kDocumentRevisionKey);
	current.valid = true;

	// An update carrying a revision the snapshot has not seen came from the
	// editor, which writes the document and then mirrors the first run's style
	// into the flat properties. Diffing against that mirror would take a change
	// the user made to one run and spread it across every run, so the whole
	// flat-to-document pass is skipped and the snapshot simply catches up.
	if (snapshot.valid && current.documentRevision != snapshot.documentRevision) {
		snapshot = current;
		return;
	}

	// Only force every flat property into the document when there is no stored
	// document to contradict them. Doing it unconditionally on the first update
	// would flatten the per-run styling of a saved scene collection every time
	// OBS started, since the flat mirror can only describe one style.
	//
	// A stored document with no text in it cannot contradict anything, so it is
	// treated the same as no document at all -- that is what lets the editor
	// seed itself from the properties dialog.
	const bool first = !snapshot.valid && (!hasDocument || doc.empty());

	// After that, a property is written into the document only when it actually
	// changed. This is what lets the flat dialog and the editor coexist:
	// without it, every update() would overwrite per-run styling with the
	// dialog's single global style.
	auto changed = [&](bool differs) {
		return first || (snapshot.valid && differs);
	};

	if (doc.blocks.empty())
		doc.setPlainText(current.text);

	if (changed(current.text != snapshot.text))
		doc.setPlainText(current.text);

	auto forEachStyle = [&doc](auto &&fn) {
		fn(doc.defaultStyle);

		for (Block &block : doc.blocks)
			for (Run &run : block.runs)
				fn(run.style);
	};

	if (changed(current.fontFace != snapshot.fontFace || current.fontStyle != snapshot.fontStyle ||
		    current.fontSize != snapshot.fontSize || current.fontFlags != snapshot.fontFlags)) {
		forEachStyle([&](Style &style) {
			style.font.family = current.fontFace;
			style.sizePx = float(current.fontSize);

			// OBS's font picker reports the chosen face variant twice: as a
			// display string, and as a flag set. The flags are authoritative
			// where they are set, but a face whose variant is named only in
			// the style string ("Semibold", "Oblique") has none of them, so
			// both are read.
			const std::string s = current.fontStyle;
			const uint32_t flags = current.fontFlags;

			style.font.italic = (flags & OBS_FONT_ITALIC) != 0 || s.find("Italic") != std::string::npos ||
					    s.find("Oblique") != std::string::npos;

			if ((flags & OBS_FONT_BOLD) != 0 || s.find("Bold") != std::string::npos)
				style.font.weight = 700;
			else if (s.find("Light") != std::string::npos)
				style.font.weight = 300;
			else
				style.font.weight = 400;

			style.underline = (flags & OBS_FONT_UNDERLINE) != 0;
			style.strikeout = (flags & OBS_FONT_STRIKEOUT) != 0;
		});
	}

	if (changed(current.color != snapshot.color)) {
		forEachStyle([&](Style &style) {
			style.fill.type = Fill::Type::Solid;
			style.fill.color = Rgba::fromObsColor(current.color);
		});
	}

	// Opacity is a whole-source multiplier rather than a second alpha on the
	// fill: it is applied in the pixel shader, so it fades the outline and drop
	// shadow along with the text. The colour picker's own alpha channel stays
	// the way to make just the fill translucent.
	if (changed(current.opacity != snapshot.opacity))
		doc.opacity = std::clamp(float(current.opacity) / 100.0f, 0.0f, 1.0f);

	if (changed(current.outline != snapshot.outline || current.outlineSize != snapshot.outlineSize ||
		    current.outlineColor != snapshot.outlineColor)) {
		forEachStyle([&](Style &style) {
			style.outline.enabled = current.outline;
			style.outline.widthPx = float(current.outlineSize);
			style.outline.color = Rgba::fromObsColor(current.outlineColor);
		});
	}

	if (changed(current.shadow != snapshot.shadow || current.shadowX != snapshot.shadowX ||
		    current.shadowY != snapshot.shadowY || current.shadowColor != snapshot.shadowColor)) {
		forEachStyle([&](Style &style) {
			style.shadow.enabled = current.shadow;
			style.shadow.offsetX = float(current.shadowX);
			style.shadow.offsetY = float(current.shadowY);
			style.shadow.color = Rgba::fromObsColor(current.shadowColor);
		});
	}

	if (changed(current.align != snapshot.align)) {
		for (Block &block : doc.blocks)
			block.align = HAlign(current.align);
	}

	if (changed(current.valign != snapshot.valign))
		doc.valign = VAlign(current.valign);

	if (changed(current.background != snapshot.background || current.backgroundColor != snapshot.backgroundColor)) {
		doc.backgroundEnabled = current.background;
		doc.backgroundColor = Rgba::fromObsColor(current.backgroundColor);
	}

	if (changed(current.extents != snapshot.extents || current.extentsWidth != snapshot.extentsWidth ||
		    current.extentsHeight != snapshot.extentsHeight || current.wrap != snapshot.wrap)) {
		doc.sizeMode = current.extents ? SizeMode::Fixed : SizeMode::Auto;
		doc.boxWidth = float(current.extentsWidth);
		doc.boxHeight = float(current.extentsHeight);
		doc.wrap = current.extents ? WrapMode(current.wrap) : WrapMode::None;
	}

	// Motion and scrolling always come from the flat properties; the editor
	// writes them back so the two never disagree.
	doc.motion.motion = Motion(obs_data_get_int(data, "motion"));
	doc.motion.order = MotionOrder(obs_data_get_int(data, "motion_order"));
	doc.motion.direction = MotionDirection(obs_data_get_int(data, "motion_direction"));
	doc.motion.duration = float(obs_data_get_double(data, "motion_duration"));
	doc.motion.stagger = float(obs_data_get_double(data, "motion_stagger"));
	doc.motion.param = float(obs_data_get_double(data, "motion_param"));
	doc.motion.speed = float(obs_data_get_double(data, "motion_speed"));
	doc.motion.trigger = MotionTrigger(obs_data_get_int(data, "motion_replay"));
	doc.motion.loopInterval = float(obs_data_get_double(data, "motion_loop_interval"));

	// Scrolling only gained flat properties after the editor did, so a source
	// configured before that has its scroll in the stored document and nothing
	// in its settings. An absent key means "never set here", not "off".
	if (obs_data_has_user_value(data, "scroll")) {
		doc.scroll.enabled = obs_data_get_bool(data, "scroll");
		doc.scroll.speedX = float(obs_data_get_double(data, "scroll_x"));
		doc.scroll.speedY = float(obs_data_get_double(data, "scroll_y"));
		doc.scroll.loop = obs_data_get_bool(data, "scroll_loop");
		doc.scroll.gap = float(obs_data_get_double(data, "scroll_gap"));
	}

	if (doc.motion.duration <= 0.0f)
		doc.motion.duration = 0.35f;

	if (doc.motion.loopInterval <= 0.0f)
		doc.motion.loopInterval = 5.0f;

	if (doc.motion.speed <= 0.0f)
		doc.motion.speed = 0.5f;

	snapshot = current;
}

void save(obs_data_t *data, const Document &doc)
{
	obs_data_t *documentData = documentToData(doc);

	obs_data_set_obj(data, kDocumentKey, documentData);
	obs_data_release(documentData);

	// Marks this update as one where the document, not the flat mirror below,
	// is the thing that changed. load() reads it to know not to diff the mirror
	// back over the document it just wrote.
	obs_data_set_int(data, kDocumentRevisionKey, obs_data_get_int(data, kDocumentRevisionKey) + 1);

	// Keep the flat mirror in step so scripts and the properties dialog read
	// back what the editor produced. It can only carry one style for the whole
	// source, so it describes the first run.
	obs_data_set_string(data, "text", doc.plainText().c_str());

	obs_data_set_int(data, "motion", int(doc.motion.motion));
	obs_data_set_int(data, "motion_order", int(doc.motion.order));
	obs_data_set_int(data, "motion_direction", int(doc.motion.direction));
	obs_data_set_double(data, "motion_duration", doc.motion.duration);
	obs_data_set_double(data, "motion_stagger", doc.motion.stagger);
	obs_data_set_double(data, "motion_param", doc.motion.param);
	obs_data_set_double(data, "motion_speed", doc.motion.speed);
	obs_data_set_int(data, "motion_replay", int(doc.motion.trigger));
	obs_data_set_double(data, "motion_loop_interval", doc.motion.loopInterval);

	obs_data_set_bool(data, "scroll", doc.scroll.enabled);
	obs_data_set_double(data, "scroll_x", doc.scroll.speedX);
	obs_data_set_double(data, "scroll_y", doc.scroll.speedY);
	obs_data_set_bool(data, "scroll_loop", doc.scroll.loop);
	obs_data_set_double(data, "scroll_gap", doc.scroll.gap);

	if (!doc.blocks.empty() && !doc.blocks.front().runs.empty()) {
		const Style &style = doc.blocks.front().runs.front().style;

		obs_data_t *font = obs_data_create();

		obs_data_set_string(font, "face", style.font.family.c_str());
		obs_data_set_int(font, "size", int(style.sizePx));

		uint32_t flags = 0;

		if (style.font.weight >= 700)
			flags |= OBS_FONT_BOLD;

		if (style.font.italic)
			flags |= OBS_FONT_ITALIC;

		if (style.underline)
			flags |= OBS_FONT_UNDERLINE;

		if (style.strikeout)
			flags |= OBS_FONT_STRIKEOUT;

		obs_data_set_int(font, "flags", flags);

		obs_data_set_obj(data, "font", font);
		obs_data_release(font);

		obs_data_set_int(data, "color", style.fill.color.toObsColor());
		obs_data_set_bool(data, "outline", style.outline.enabled);
		obs_data_set_int(data, "outline_size", int(style.outline.widthPx));
		obs_data_set_int(data, "outline_color", style.outline.color.toObsColor());
		obs_data_set_bool(data, "shadow", style.shadow.enabled);
		obs_data_set_double(data, "shadow_x", style.shadow.offsetX);
		obs_data_set_double(data, "shadow_y", style.shadow.offsetY);
		obs_data_set_int(data, "shadow_color", style.shadow.color.toObsColor());
		obs_data_set_int(data, "align", int(doc.blocks.front().align));
	}

	obs_data_set_int(data, "opacity", int(std::lround(doc.opacity * 100.0f)));
	obs_data_set_int(data, "valign", int(doc.valign));
	obs_data_set_bool(data, "background", doc.backgroundEnabled);
	obs_data_set_int(data, "background_color", doc.backgroundColor.toObsColor());

	obs_data_set_bool(data, "extents", doc.sizeMode == SizeMode::Fixed);
	obs_data_set_int(data, "extents_cx", int(doc.boxWidth));
	obs_data_set_int(data, "extents_cy", int(doc.boxHeight));
	obs_data_set_int(data, "wrap", int(doc.wrap));
}

} // namespace settings
} // namespace slugged
