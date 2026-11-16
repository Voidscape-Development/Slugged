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

#include "start_points.hpp"
#include "settings.hpp"

#include <cstring>

#include <obs-module.h>

namespace slugged {

namespace {

const Rgba kAccent{0.96f, 0.78f, 0.29f, 1.0f};
const Rgba kPlate{0.07f, 0.08f, 0.10f, 0.85f};

// Replaces the document's text with `lines`, one paragraph each.
void setLines(Document &doc, std::initializer_list<const char *> lines)
{
	std::string text;

	for (const char *line : lines) {
		if (!text.empty())
			text += '\n';

		text += line;
	}

	doc.blocks.clear();
	doc.setPlainText(text);
}

// Styles the whole of paragraph `index`.
void styleLine(Document &doc, size_t index, const StyleOverride &style)
{
	size_t start = 0;

	for (size_t i = 0; i < doc.blocks.size(); i++) {
		const size_t length = doc.blocks[i].size();

		if (i == index) {
			doc.applyStyle(start, start + length, style);
			return;
		}

		start += length + 1;
	}
}

void centre(Document &doc)
{
	doc.paragraph.align = HAlign::Center;
}

} // namespace

const std::vector<StartPoint> &startPoints()
{
	static const std::vector<StartPoint> points = {
		{"plain", "Start.Plain", "Start.Plain.About"},
		{"title", "Start.Title", "Start.Title.About"},
		{"clock", "Start.Clock", "Start.Clock.About"},
		{"countdown", "Start.Countdown", "Start.Countdown.About"},
		{"timer", "Start.Timer", "Start.Timer.About"},
		{"ticker", "Start.Ticker", "Start.Ticker.About"},
		{"credits", "Start.Credits", "Start.Credits.About"},
		{"bot", "Start.Bot", "Start.Bot.About"},
		{"file", "Start.File", "Start.File.About"},
	};

	return points;
}

bool isUntouched(obs_data_t *settings)
{
	if (!settings)
		return false;

	// A new source still says what it was created saying.
	const char *initial = obs_data_get_default_string(settings, "text");

	return initial && std::strcmp(obs_data_get_string(settings, "text"), initial) == 0 &&
	       std::strcmp(obs_data_get_string(settings, "mode"), "file") != 0;
}

bool applyStartPoint(obs_data_t *settings, const std::string &id)
{
	bool known = false;

	for (const StartPoint &point : startPoints())
		known = known || id == point.id;

	if (!known || !settings)
		return false;

	const settings::SourceConfig current = settings::load(settings);

	// Everything starts from defaults except the font the user already has.
	Document doc;

	doc.base.font.family = current.document.base.font.family;
	doc.base.font.file = current.document.base.font.file;
	doc.base.font.faceIndex = current.document.base.font.faceIndex;

	VariableMap variables = current.variables;
	const char *mode = "text";

	if (id == "plain") {
		setLines(doc, {""});
	} else if (id == "title") {
		setLines(doc, {obs_module_text("Start.Title.Line1"), obs_module_text("Start.Title.Line2")});

		doc.base.sizePx = 56.0f;
		doc.base.font.weight = 700;

		StyleOverride second;
		second.size = 30.0f;
		second.weight = 400;
		styleLine(doc, 1, second);

		doc.background.enabled = true;
		doc.background.color = kPlate;
		doc.background.radius = 14.0f;
		doc.padding = 22.0f;

		doc.motion.motion = Motion::Slide;
		doc.motion.order = MotionOrder::Together;
		doc.motion.direction = MotionDirection::Right;
		doc.motion.trigger = MotionTrigger::Show;
	} else if (id == "clock") {
		setLines(doc, {"{time}"});

		doc.base.sizePx = 120.0f;
		doc.base.font.weight = 700;
	} else if (id == "countdown") {
		setLines(doc, {obs_module_text("Start.Countdown.Line1"), "{countdown:5:00}"});

		doc.base.sizePx = 140.0f;
		doc.base.font.weight = 700;
		centre(doc);

		StyleOverride first;
		first.size = 34.0f;
		first.weight = 500;
		first.letterSpacing = 5.0f;
		first.color = kAccent;
		styleLine(doc, 0, first);

		doc.motion.motion = Motion::Fade;
		doc.motion.order = MotionOrder::PerLine;
		doc.motion.trigger = MotionTrigger::Show;
	} else if (id == "timer") {
		setLines(doc, {"{stream_time}"});

		doc.base.sizePx = 96.0f;
		doc.base.font.weight = 700;
	} else if (id == "ticker") {
		setLines(doc, {obs_module_text("Start.Ticker.Line1")});

		doc.base.sizePx = 40.0f;
		doc.base.font.weight = 600;

		doc.sizeMode = SizeMode::Fixed;
		doc.boxWidth = 1280.0f;
		doc.boxHeight = 76.0f;
		doc.valign = VAlign::Middle;
		doc.padding = 12.0f;

		doc.background.enabled = true;
		doc.background.color = kPlate;

		doc.scroll.enabled = true;
		doc.scroll.speedX = -120.0f;
		doc.scroll.speedY = 0.0f;
		doc.scroll.loop = true;
	} else if (id == "credits") {
		setLines(doc, {obs_module_text("Start.Credits.Line1"), "", obs_module_text("Start.Credits.Line2"),
			       obs_module_text("Start.Credits.Line3"), obs_module_text("Start.Credits.Line4")});

		doc.base.sizePx = 44.0f;
		centre(doc);

		StyleOverride heading;
		heading.weight = 700;
		heading.size = 56.0f;
		heading.color = kAccent;
		styleLine(doc, 2, heading);

		doc.sizeMode = SizeMode::Fixed;
		doc.boxWidth = 900.0f;
		doc.boxHeight = 600.0f;

		doc.scroll.enabled = true;
		doc.scroll.speedX = 0.0f;
		doc.scroll.speedY = -60.0f;
		doc.scroll.loop = false;
	} else if (id == "bot") {
		const std::string lead = obs_module_text("Start.Bot.Line1");

		setLines(doc, {(lead + " {follower}").c_str()});

		doc.base.sizePx = 64.0f;
		doc.base.font.weight = 700;

		StyleOverride value;
		value.color = kAccent;
		doc.applyStyle(lead.size() + 1, doc.size(), value);

		doc.motion.motion = Motion::Pop;
		doc.motion.param = motionParamSpec(Motion::Pop).defaultValue;
		doc.motion.trigger = MotionTrigger::TextChange;
		doc.motion.scope = MotionScope::Changed;

		// So the value shows something until a bot sets it.
		if (!variables.count("follower"))
			variables["follower"] = obs_module_text("Start.Bot.Sample");
	} else if (id == "file") {
		setLines(doc, {""});
		mode = "file";
	}

	settings::save(settings, doc, false);

	obs_data_set_bool(settings, "markup", false);
	obs_data_set_string(settings, "mode", mode);
	settings::variablesToData(settings, settings::kVariablesKey, variables);

	return true;
}

} // namespace slugged
