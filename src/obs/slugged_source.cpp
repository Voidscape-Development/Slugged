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

#include "slugged_source.hpp"
#include "editor_bridge.hpp"
#include "host_tokens.hpp"
#include "settings.hpp"
#include "../core/markup.hpp"
#include "../core/text_feed.hpp"
#include "../core/tokens.hpp"
#include "../render/rebuild_worker.hpp"
#include "../render/renderer.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <graphics/vec4.h>
#include <obs-module.h>

namespace slugged {

const char *const kSourceId = "slugged_text";
const char *const kFilterId = "slugged_text_filter";

namespace {

using settings::SourceConfig;

// Text offsets [start, end) that changed in the latest content change, so the
// replay that follows animates only those characters.
struct Range {
	uint32_t start = 0;
	uint32_t end = 0;
};

// What the last uploaded frame measured.
struct Metrics {
	float width = 0.0f;
	float height = 0.0f;
	float contentX0 = 0.0f;
	float contentY0 = 0.0f;
	float contentX1 = 0.0f;
	float contentY1 = 0.0f;
	bool clip = false;
	uint32_t units = 0;
	uint32_t changedUnits = 0;
};

// One Slugged source or filter.
//
// Threads: update() and the procs, hotkeys and properties buttons arrive on
// arbitrary threads and only touch the config snapshot (under `configMutex`), the
// token tables (under `tokenMutex`) or atomics. Everything else belongs to the
// graphics thread, where video_tick and video_render both run. Layout and atlas
// building happen on the RebuildWorker.
struct SluggedSource {
	obs_source_t *source = nullptr;
	bool isFilter = false;

	std::mutex configMutex;
	std::shared_ptr<const SourceConfig> config;
	uint64_t configVersion = 0;

	std::mutex tokenMutex;
	TokenContext tokens;

	std::atomic<bool> timerRunning{true};
	std::atomic<bool> timerResetRequested{false};
	std::atomic<bool> replayRequested{false};
	std::atomic<bool> nextMessageRequested{false};

	std::atomic<uint32_t> width{0};
	std::atomic<uint32_t> height{0};

	// The editor reads these from the UI thread.
	std::atomic<bool> editorAttached{false};
	std::atomic<float> timerSeconds{0.0f};

	std::mutex editMutex;
	std::shared_ptr<const EditMap> editMap;

	obs_hotkey_id replayHotkey = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id timerToggleHotkey = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id timerResetHotkey = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id counterUpHotkey = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id counterDownHotkey = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id counterResetHotkey = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id nextMessageHotkey = OBS_INVALID_HOTKEY_ID;

	// ---- graphics thread -------------------------------------------------

	std::shared_ptr<const SourceConfig> current;
	uint64_t seenConfigVersion = 0;

	TextFeed feed;
	TextFeed variableFeed;
	uint64_t hostGeneration = 0;

	// The document as authored -- file contents or markup applied, tokens not
	// yet expanded -- and what of it is shown: all of it, or one line when the
	// lines take turns.
	Document full;
	Document authored;
	std::string authoredPlain;
	bool authoredValid = false;

	// Taking turns: which line is up, and for how long it has been.
	bool rotating = false;
	size_t rotateIndex = 0;
	float rotateClock = 0.0f;
	bool rotateAdvanced = false;
	std::minstd_rand rotateRandom{std::random_device{}()};

	// Counting a number up to its new value: the text being counted from, the
	// content that started it, and how far along it is.
	bool counting = false;
	std::string countFrom;
	std::string countContent;
	float countClock = 0.0f;
	std::string shownText;

	// The effects' clock, which nothing resets.
	float loopTime = 0.0f;

	// Text filled with another source's picture: which source, rendered into
	// a texture each frame it is needed.
	std::string fillName;
	obs_weak_source_t *fillSource = nullptr;
	gs_texrender_t *fillRender = nullptr;
	gs_texrender_t *filterRender = nullptr;
	bool renderingFill = false;

	std::string lastExpanded;
	std::string lastContent;
	bool built = false;
	bool builtForEditor = false;

	uint32_t targetWidth = 0;
	uint32_t targetHeight = 0;

	std::shared_ptr<RebuildWorker::Job> job = std::make_shared<RebuildWorker::Job>();
	uint64_t nextFrameId = 1;
	uint64_t appliedFrameId = 0;

	std::optional<FrameOutput> pendingUpload;
	uint64_t pendingUploadId = 0;

	// A replay waiting for the frame that shows the text it is for.
	uint64_t replayOnFrame = 0;
	bool replayChangedOnly = false;
	bool replayIsTextChange = false;

	Range animRange;
	Metrics metrics;

	float uptime = 0.0f;
	float timer = 0.0f;

	float animTime = 0.0f;
	float animEnd = 0.0f;
	bool scopeChanged = false;

	bool exitActive = false;
	float exitTime = 0.0f;
	float exitEnd = 0.0f;
	bool releaseOutgoing = false;

	float travelX = 0.0f;
	float travelY = 0.0f;

	Renderer renderer;
};

// ---- small helpers -----------------------------------------------------------

std::shared_ptr<const SourceConfig> configOf(SluggedSource *ctx)
{
	std::lock_guard<std::mutex> lock(ctx->configMutex);

	return ctx->config;
}

// The span of `next` that differs from `prev`, by common prefix and suffix.
Range diffRange(const std::string &prev, const std::string &next)
{
	const size_t limit = std::min(prev.size(), next.size());

	size_t prefix = 0;

	while (prefix < limit && prev[prefix] == next[prefix])
		prefix++;

	while (prefix > 0 && prefix < next.size() && (uint8_t(next[prefix]) & 0xC0) == 0x80)
		prefix--;

	size_t suffix = 0;

	while (suffix < limit - prefix && prev[prev.size() - 1 - suffix] == next[next.size() - 1 - suffix])
		suffix++;

	return Range{uint32_t(prefix), uint32_t(next.size() - suffix)};
}

// Moves an animating range through an edit elsewhere in the text, so a clock
// ticking over next to a counter mid-animation does not cut the animation off.
Range shiftRange(Range range, const std::string &prev, const std::string &next)
{
	const Range edit = diffRange(prev, next);
	const uint32_t oldEditEnd = uint32_t(edit.end + prev.size() - next.size());

	if (edit.start >= range.end)
		return range;

	if (oldEditEnd <= range.start) {
		const int64_t delta = int64_t(next.size()) - int64_t(prev.size());

		return Range{uint32_t(int64_t(range.start) + delta), uint32_t(int64_t(range.end) + delta)};
	}

	return Range{std::min(range.start, edit.start), std::max(range.end, edit.end)};
}

float passDuration(const MotionSpec &motion, uint32_t units)
{
	return motion.passDuration(units) + 0.25f;
}

// Whether anything in `doc` is filled with another source's picture.
bool usesSourceFill(const Document &doc)
{
	if (doc.fillSource.empty())
		return false;

	if (doc.base.fill.type == FillType::Source)
		return true;

	for (const Block &block : doc.blocks)
		for (const Run &run : block.runs)
			if (run.style.fillType && *run.style.fillType == FillType::Source)
				return true;

	return false;
}

// Points the source fill at the source called `name`, or at nothing. A source
// used as a fill is kept "showing", so a media source plays while it shows
// through the letters even when no scene shows it.
void bindFillSource(SluggedSource *ctx, const std::string &name)
{
	if (name == ctx->fillName && (ctx->fillSource || name.empty()))
		return;

	if (ctx->fillSource) {
		if (obs_source_t *old = obs_weak_source_get_source(ctx->fillSource)) {
			obs_source_dec_showing(old);
			obs_source_release(old);
		}

		obs_weak_source_release(ctx->fillSource);
		ctx->fillSource = nullptr;
	}

	ctx->fillName = name;

	if (name.empty())
		return;

	obs_source_t *source = obs_get_source_by_name(name.c_str());

	if (!source)
		return;

	// A source cannot fill itself, nor be filled by a scene showing it.
	if (source != ctx->source) {
		ctx->fillSource = obs_source_get_weak_source(source);
		obs_source_inc_showing(source);
	}

	obs_source_release(source);
}

// ---- settings ----------------------------------------------------------------

void updateSource(void *data, obs_data_t *settingsData)
{
	auto *ctx = static_cast<SluggedSource *>(data);

	settings::migrate(settingsData);

	auto config = std::make_shared<const SourceConfig>(settings::load(settingsData));

	// Keep the stored styling in step with text set from outside, so the next
	// change is diffed against what is actually shown.
	if (config->contentAdjusted)
		settings::saveContent(settingsData, config->document);

	{
		std::lock_guard<std::mutex> lock(ctx->tokenMutex);

		ctx->tokens.setSourceVariables(config->variables);
	}

	std::lock_guard<std::mutex> lock(ctx->configMutex);

	ctx->config = std::move(config);
	ctx->configVersion++;
}

// ---- procs and hotkeys -------------------------------------------------------
//
// Everything a Slugged source can be driven with from outside is a proc on the
// source, so a Lua or Python script reaches all of it through
// obs_source_get_proc_handler without linking against the plugin.

void setVariableProc(void *data, calldata_t *call)
{
	auto *ctx = static_cast<SluggedSource *>(data);

	const char *name = nullptr;
	const char *value = nullptr;

	if (!calldata_get_string(call, "name", &name) || !name || !*name)
		return;

	if (!calldata_get_string(call, "value", &value) || !value)
		value = "";

	std::lock_guard<std::mutex> lock(ctx->tokenMutex);

	ctx->tokens.set(name, value);
}

void clearVariablesProc(void *data, calldata_t *)
{
	auto *ctx = static_cast<SluggedSource *>(data);

	std::lock_guard<std::mutex> lock(ctx->tokenMutex);

	ctx->tokens.clearRuntime();
}

// Steps a numeric variable: the counter behind a death counter or a score.
// The value is changed where it lives, so it lasts as long as that place does:
// a value a script set stays a script value, a shared one stays shared, and
// anything else is kept in the source's own variables list, which is saved with
// the scene collection.
void stepVariable(SluggedSource *ctx, std::string name, double delta, bool reset)
{
	const auto config = configOf(ctx);

	if (name.empty())
		name = config ? config->counterVariable : "count";

	name = normaliseVariableName(name);

	if (name.empty())
		return;

	std::string current;
	TokenContext::Layer layer;

	{
		std::lock_guard<std::mutex> lock(ctx->tokenMutex);

		layer = ctx->tokens.find(name, current);
	}

	double value = 0.0;
	parseNumber(current, value);

	const std::string next = formatPlainNumber(reset ? 0.0 : value + delta);

	switch (layer) {
	case TokenContext::Layer::Runtime:
	case TokenContext::Layer::File: {
		// A file cannot be written back; a script value wins over it.
		std::lock_guard<std::mutex> lock(ctx->tokenMutex);

		ctx->tokens.set(name, next);
		return;
	}
	case TokenContext::Layer::Global:
		GlobalVariables::set(name, next);
		return;
	case TokenContext::Layer::Source:
	case TokenContext::Layer::None:
		break;
	}

	obs_data_t *data = obs_source_get_settings(ctx->source);

	if (!data)
		return;

	VariableMap values = settings::variablesFromData(data, settings::kVariablesKey);
	values[name] = next;
	settings::variablesToData(data, settings::kVariablesKey, values);

	obs_source_update(ctx->source, data);
	obs_data_release(data);
}

double stepOf(SluggedSource *ctx, calldata_t *call)
{
	double amount = 0.0;

	if (call && calldata_get_float(call, "amount", &amount) && amount != 0.0)
		return amount;

	const auto config = configOf(ctx);

	return config && config->counterStep != 0.0 ? config->counterStep : 1.0;
}

std::string nameOf(calldata_t *call)
{
	const char *name = nullptr;

	return call && calldata_get_string(call, "name", &name) && name ? name : "";
}

void incrementProc(void *data, calldata_t *call)
{
	auto *ctx = static_cast<SluggedSource *>(data);

	stepVariable(ctx, nameOf(call), stepOf(ctx, call), false);
}

void decrementProc(void *data, calldata_t *call)
{
	auto *ctx = static_cast<SluggedSource *>(data);

	stepVariable(ctx, nameOf(call), -stepOf(ctx, call), false);
}

void resetVariableProc(void *data, calldata_t *call)
{
	stepVariable(static_cast<SluggedSource *>(data), nameOf(call), 0.0, true);
}

void getVariableProc(void *data, calldata_t *call)
{
	auto *ctx = static_cast<SluggedSource *>(data);
	const std::string name = nameOf(call);

	std::string value;

	{
		std::lock_guard<std::mutex> lock(ctx->tokenMutex);

		// Anything a token could say: a variable, or a live value.
		if (ctx->tokens.find(name, value) == TokenContext::Layer::None) {
			const std::string token = "{" + name + "}";
			const std::string expanded = ctx->tokens.expand(token);

			value = expanded == token ? std::string() : expanded;
		}
	}

	calldata_set_string(call, "value", value.c_str());
}

void nextMessageProc(void *data, calldata_t *)
{
	static_cast<SluggedSource *>(data)->nextMessageRequested = true;
}

void replayProc(void *data, calldata_t *)
{
	static_cast<SluggedSource *>(data)->replayRequested = true;
}

void timerStartProc(void *data, calldata_t *)
{
	static_cast<SluggedSource *>(data)->timerRunning = true;
}

void timerPauseProc(void *data, calldata_t *)
{
	static_cast<SluggedSource *>(data)->timerRunning = false;
}

void timerToggleProc(void *data, calldata_t *)
{
	auto *ctx = static_cast<SluggedSource *>(data);

	ctx->timerRunning = !ctx->timerRunning;
}

void timerResetProc(void *data, calldata_t *)
{
	static_cast<SluggedSource *>(data)->timerResetRequested = true;
}

void replayHotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		replayProc(data, nullptr);
}

void timerToggleHotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		timerToggleProc(data, nullptr);
}

void timerResetHotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		timerResetProc(data, nullptr);
}

void counterUpHotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		incrementProc(data, nullptr);
}

void counterDownHotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		decrementProc(data, nullptr);
}

void counterResetHotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		resetVariableProc(data, nullptr);
}

void nextMessageHotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		nextMessageProc(data, nullptr);
}

// ---- lifecycle ---------------------------------------------------------------

void *createSource(obs_data_t *settingsData, obs_source_t *source, bool isFilter)
{
	auto *ctx = new SluggedSource();

	ctx->source = source;
	ctx->isFilter = isFilter;

	updateSource(ctx, settingsData);

	if (proc_handler_t *procs = obs_source_get_proc_handler(source)) {
		proc_handler_add(procs, "void set_variable(string name, string value)", setVariableProc, ctx);
		proc_handler_add(procs, "void clear_variables()", clearVariablesProc, ctx);
		proc_handler_add(procs, "void replay_motion()", replayProc, ctx);
		proc_handler_add(procs, "void timer_start()", timerStartProc, ctx);
		proc_handler_add(procs, "void timer_pause()", timerPauseProc, ctx);
		proc_handler_add(procs, "void timer_toggle()", timerToggleProc, ctx);
		proc_handler_add(procs, "void timer_reset()", timerResetProc, ctx);
		proc_handler_add(procs, "void get_variable(in string name, out string value)", getVariableProc, ctx);
		proc_handler_add(procs, "void increment_variable(string name, float amount)", incrementProc, ctx);
		proc_handler_add(procs, "void decrement_variable(string name, float amount)", decrementProc, ctx);
		proc_handler_add(procs, "void reset_variable(string name)", resetVariableProc, ctx);
		proc_handler_add(procs, "void next_message()", nextMessageProc, ctx);
	}

	ctx->replayHotkey = obs_hotkey_register_source(source, "Slugged.Replay", obs_module_text("Hotkey.Replay"),
						       replayHotkey, ctx);
	ctx->timerToggleHotkey = obs_hotkey_register_source(
		source, "Slugged.TimerToggle", obs_module_text("Hotkey.TimerToggle"), timerToggleHotkey, ctx);
	ctx->timerResetHotkey = obs_hotkey_register_source(source, "Slugged.TimerReset",
							   obs_module_text("Hotkey.TimerReset"), timerResetHotkey, ctx);
	ctx->counterUpHotkey = obs_hotkey_register_source(source, "Slugged.CounterUp",
							  obs_module_text("Hotkey.CounterUp"), counterUpHotkey, ctx);
	ctx->counterDownHotkey = obs_hotkey_register_source(
		source, "Slugged.CounterDown", obs_module_text("Hotkey.CounterDown"), counterDownHotkey, ctx);
	ctx->counterResetHotkey = obs_hotkey_register_source(
		source, "Slugged.CounterReset", obs_module_text("Hotkey.CounterReset"), counterResetHotkey, ctx);
	ctx->nextMessageHotkey = obs_hotkey_register_source(
		source, "Slugged.NextMessage", obs_module_text("Hotkey.NextMessage"), nextMessageHotkey, ctx);

	return ctx;
}

void *createTextSource(obs_data_t *settingsData, obs_source_t *source)
{
	return createSource(settingsData, source, false);
}

void *createFilterSource(obs_data_t *settingsData, obs_source_t *source)
{
	return createSource(settingsData, source, true);
}

void destroySource(void *data)
{
	auto *ctx = static_cast<SluggedSource *>(data);

	// The editor holds this source; close it before the state goes away.
	editor::closeFor(ctx->source);

	for (obs_hotkey_id id : {ctx->replayHotkey, ctx->timerToggleHotkey, ctx->timerResetHotkey, ctx->counterUpHotkey,
				 ctx->counterDownHotkey, ctx->counterResetHotkey, ctx->nextMessageHotkey})
		if (id != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(id);

	{
		std::lock_guard<std::mutex> lock(ctx->job->mutex);

		ctx->job->cancelled = true;
		ctx->job->pending.reset();
		ctx->job->ready.reset();
	}

	bindFillSource(ctx, std::string());

	obs_enter_graphics();
	ctx->renderer.release();

	if (ctx->fillRender)
		gs_texrender_destroy(ctx->fillRender);

	if (ctx->filterRender)
		gs_texrender_destroy(ctx->filterRender);

	obs_leave_graphics();

	delete ctx;
}

uint32_t getWidth(void *data)
{
	return static_cast<SluggedSource *>(data)->width;
}

uint32_t getHeight(void *data)
{
	return static_cast<SluggedSource *>(data)->height;
}

// ---- tick --------------------------------------------------------------------

// The lines that take turns: every paragraph with something in it.
std::vector<size_t> messageLines(const Document &doc)
{
	std::vector<size_t> lines;

	for (size_t i = 0; i < doc.blocks.size(); i++)
		if (doc.blocks[i].plainText().find_first_not_of(" \t\r") != std::string::npos)
			lines.push_back(i);

	return lines;
}

// Whether the lines take turns right now. An open editor shows them all, so
// every line can be edited where it is.
bool wantsRotation(SluggedSource *ctx)
{
	return ctx->full.rotate.enabled && !ctx->editorAttached && messageLines(ctx->full).size() > 1;
}

// Sets `authored` to what is shown of `full`: all of it, or the line whose
// turn it is.
void selectShown(SluggedSource *ctx)
{
	ctx->authored = ctx->full;
	ctx->rotating = wantsRotation(ctx);

	if (ctx->rotating) {
		const std::vector<size_t> lines = messageLines(ctx->full);

		ctx->rotateIndex %= lines.size();
		ctx->authored.blocks = {ctx->full.blocks[lines[ctx->rotateIndex]]};
	}

	ctx->authoredPlain = ctx->authored.plainText();
	ctx->authoredValid = true;
}

// Moves on to the next line, or a random other one when shuffling.
void advanceMessage(SluggedSource *ctx)
{
	const size_t count = messageLines(ctx->full).size();

	if (count < 2)
		return;

	if (ctx->full.rotate.shuffle) {
		std::uniform_int_distribution<size_t> pick(1, count - 1);
		ctx->rotateIndex = (ctx->rotateIndex + pick(ctx->rotateRandom)) % count;
	} else {
		ctx->rotateIndex = (ctx->rotateIndex + 1) % count;
	}

	ctx->rotateClock = 0.0f;
	ctx->rotateAdvanced = true;

	selectShown(ctx);
}

// Rebuilds `full` from the config and, in file mode, the file's contents, and
// from it `authored`.
void refreshAuthored(SluggedSource *ctx, const SourceConfig &config)
{
	ctx->full = config.document;

	if (config.mode == settings::SourceMode::File) {
		const std::string &text = ctx->feed.text();

		// The file's text replaces the configured text through the same
		// minimal edit a script's would, so styling the first line of a
		// template keeps applying to whatever the file says there.
		if (config.markup)
			ctx->full.blocks = parseMarkup(text);
		else
			ctx->full.setPlainText(text);
	}

	selectShown(ctx);
}

// `expanded` is the text as it will be shown: tokens expanded, and any number
// that is counting up part of the way there.
void scheduleBuild(SluggedSource *ctx, const std::string &expanded, const std::string &content)
{
	const MotionSpec &motion = ctx->authored.motion;

	const uint64_t id = ctx->nextFrameId++;

	const bool rotated = ctx->rotateAdvanced;
	ctx->rotateAdvanced = false;

	if (!ctx->built) {
		// The first text a source shows plays its intro.
		ctx->replayOnFrame = id;
		ctx->replayChangedOnly = false;
		ctx->replayIsTextChange = false;
		ctx->animRange = Range{};
	} else if (rotated && motionIsTransient(motion.motion)) {
		// The next line taking its turn is a new message, whatever plays the
		// motion otherwise: all of it comes in, after the last one goes out.
		ctx->animRange = Range{};
		ctx->replayOnFrame = id;
		ctx->replayChangedOnly = false;
		ctx->replayIsTextChange = true;
	} else if (content != ctx->lastContent) {
		ctx->animRange = diffRange(ctx->lastExpanded, expanded);

		if (motion.trigger == MotionTrigger::TextChange && motionIsTransient(motion.motion)) {
			ctx->replayOnFrame = id;
			ctx->replayChangedOnly = motion.scope == MotionScope::Changed;
			ctx->replayIsTextChange = true;
		}
	} else if (motion.live && motion.trigger == MotionTrigger::TextChange && motionIsTransient(motion.motion) &&
		   expanded != ctx->lastExpanded) {
		// Asked for: a live value ticking over animates like any other change,
		// so a countdown's digits move as they turn.
		ctx->animRange = diffRange(ctx->lastExpanded, expanded);
		ctx->replayOnFrame = id;
		ctx->replayChangedOnly = motion.scope == MotionScope::Changed;
		ctx->replayIsTextChange = true;
	} else {
		ctx->animRange = shiftRange(ctx->animRange, ctx->lastExpanded, expanded);
	}

	FrameInput input;

	input.document = ctx->authored;
	input.writtenSize = uint32_t(ctx->authoredPlain.size());
	input.wantEditMap = ctx->editorAttached;

	{
		std::lock_guard<std::mutex> lock(ctx->tokenMutex);

		input.document.expandTokens(ctx->tokens, input.tokenSpans);
	}

	// A number counting up shows a value no token holds; the minimal edit
	// keeps the styling of its digits.
	if (ctx->counting && input.document.plainText() != expanded)
		input.document.setPlainText(expanded);

	ctx->builtForEditor = input.wantEditMap;

	if (ctx->isFilter && ctx->targetWidth > 0 && ctx->targetHeight > 0 &&
	    input.document.sizeMode != SizeMode::Fixed) {
		// An overlay lays out against the source it sits on, so alignment
		// has a box to align in.
		input.overrideBox = true;
		input.boxWidth = float(ctx->targetWidth);
		input.boxHeight = float(ctx->targetHeight);
	}

	input.params.changedStart = ctx->animRange.start;
	input.params.changedEnd = ctx->animRange.end;

	RebuildWorker::instance().submit(ctx->job, std::move(input), id);

	ctx->lastExpanded = expanded;
	ctx->lastContent = content;
	ctx->built = true;
}

void applyReadyFrame(SluggedSource *ctx)
{
	std::optional<FrameOutput> ready;
	uint64_t id = 0;

	{
		std::lock_guard<std::mutex> lock(ctx->job->mutex);

		if (!ctx->job->ready || ctx->job->readyId <= ctx->appliedFrameId)
			return;

		ready = std::move(ctx->job->ready);
		id = ctx->job->readyId;
		ctx->job->ready.reset();
	}

	ctx->appliedFrameId = id;

	// Size is known here, on the tick, so a source that is not being rendered
	// still reports it.
	ctx->width = ready->width;
	ctx->height = ready->height;

	{
		std::lock_guard<std::mutex> lock(ctx->editMutex);

		ctx->editMap = ready->editMap;
	}

	ctx->pendingUpload = std::move(ready);
	ctx->pendingUploadId = id;
}

void tickSource(void *data, float seconds)
{
	auto *ctx = static_cast<SluggedSource *>(data);

	ctx->uptime += seconds;

	// Wrapped at an hour, which a whole number of cycles of any common speed
	// divides, so the float keeps the resolution an effect needs.
	ctx->loopTime = std::fmod(ctx->loopTime + seconds, 3600.0f);

	if (ctx->timerResetRequested.exchange(false))
		ctx->timer = 0.0f;

	if (ctx->timerRunning)
		ctx->timer += seconds;

	ctx->timerSeconds = ctx->timer;

	// ---- configuration ---------------------------------------------------
	bool needBuild = false;

	{
		std::lock_guard<std::mutex> lock(ctx->configMutex);

		if (ctx->configVersion != ctx->seenConfigVersion) {
			// Touching the motion settings replays what they configure.
			if (ctx->current && ctx->config &&
			    ctx->current->document.motion != ctx->config->document.motion)
				ctx->replayRequested = true;

			ctx->current = ctx->config;
			ctx->seenConfigVersion = ctx->configVersion;
			ctx->authoredValid = false;
			ctx->feed.invalidate();
			ctx->variableFeed.invalidate();
			needBuild = true;
		}
	}

	if (!ctx->current)
		return;

	const SourceConfig &config = *ctx->current;

	if (config.mode == settings::SourceMode::File && ctx->feed.tick(config.file, seconds))
		ctx->authoredValid = false;

	if (ctx->variableFeed.tick(config.variablesFile, seconds)) {
		const VariableMap values = settings::parseVariableFile(ctx->variableFeed.text());

		std::lock_guard<std::mutex> lock(ctx->tokenMutex);

		ctx->tokens.setFileVariables(values);
	}

	const uint64_t hostGeneration = host::generation();

	if (hostGeneration != ctx->hostGeneration) {
		ctx->hostGeneration = hostGeneration;

		VariableMap values = host::values();

		const char *name = obs_source_get_name(ctx->source);
		values["source"] = name ? name : "";

		std::lock_guard<std::mutex> lock(ctx->tokenMutex);

		ctx->tokens.setHostVariables(values);
	}

	if (!ctx->authoredValid) {
		refreshAuthored(ctx, config);
		needBuild = true;
	}

	// ---- lines taking turns ------------------------------------------------
	if (wantsRotation(ctx) != ctx->rotating) {
		selectShown(ctx);
		needBuild = true;
	}

	if (ctx->rotating) {
		ctx->rotateClock += seconds;

		if (ctx->nextMessageRequested.exchange(false) || ctx->rotateClock >= ctx->full.rotate.interval) {
			advanceMessage(ctx);
			needBuild = true;
		}
	} else {
		ctx->nextMessageRequested = false;
	}

	// ---- another source's picture ------------------------------------------
	bindFillSource(ctx, usesSourceFill(ctx->authored) ? ctx->authored.fillSource : std::string());

	// ---- tokens ----------------------------------------------------------
	std::string expanded;
	std::string content;

	{
		std::lock_guard<std::mutex> lock(ctx->tokenMutex);

		ctx->tokens.setUptime(ctx->uptime);
		ctx->tokens.setTimer(ctx->timer);

		if (TokenContext::hasTokens(ctx->authoredPlain)) {
			expanded = ctx->tokens.expand(ctx->authoredPlain);
			content = ctx->tokens.expandContent(ctx->authoredPlain);
		} else {
			expanded = ctx->authoredPlain;
			content = ctx->authoredPlain;
		}
	}

	// ---- counting numbers up ----------------------------------------------
	const MotionSpec &counted = ctx->authored.motion;

	if (counted.countUp && !ctx->editorAttached) {
		// Only a real change counts; a clock ticking over does not.
		if (content != ctx->countContent) {
			std::string probe;

			ctx->counting = ctx->built && interpolateNumbers(ctx->shownText, expanded, 0.0, probe);
			ctx->countFrom = ctx->shownText;
			ctx->countClock = 0.0f;
			ctx->countContent = content;
		}

		if (ctx->counting) {
			ctx->countClock += seconds;

			const float t = std::clamp(ctx->countClock / counted.countDuration, 0.0f, 1.0f);
			const float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);

			std::string shown;

			if (t < 1.0f && interpolateNumbers(ctx->countFrom, expanded, eased, shown))
				expanded = shown;
			else
				ctx->counting = false;
		}
	} else {
		ctx->counting = false;
		ctx->countContent = content;
	}

	ctx->shownText = expanded;

	// A clock token changes once a minute, not sixty times a second, so a
	// rebuild only follows a real difference.
	if (expanded != ctx->lastExpanded)
		needBuild = true;

	if (ctx->isFilter) {
		obs_source_t *target = obs_filter_get_target(ctx->source);

		const uint32_t w = target ? obs_source_get_width(target) : 0;
		const uint32_t h = target ? obs_source_get_height(target) : 0;

		if (w != ctx->targetWidth || h != ctx->targetHeight) {
			ctx->targetWidth = w;
			ctx->targetHeight = h;
			needBuild = true;
		}
	}

	// An editor opening or closing changes what a frame has to carry.
	if (ctx->editorAttached != ctx->builtForEditor)
		needBuild = true;

	if (needBuild || !ctx->built)
		scheduleBuild(ctx, expanded, content);

	// A turn that changed nothing on screen (two identical lines) is not
	// carried over to whatever changes next.
	ctx->rotateAdvanced = false;

	applyReadyFrame(ctx);

	// ---- animation clock -------------------------------------------------
	const MotionSpec &motion = ctx->authored.motion;

	if (ctx->replayRequested.exchange(false)) {
		ctx->animTime = 0.0f;
		ctx->scopeChanged = false;
		ctx->animEnd = passDuration(motion, ctx->metrics.units);
		ctx->travelX = 0.0f;
		ctx->travelY = 0.0f;
	}

	if (ctx->exitActive) {
		ctx->exitTime += seconds;

		if (ctx->exitTime >= ctx->exitEnd) {
			ctx->exitActive = false;
			ctx->releaseOutgoing = true;
		}
	} else if (motion.motion != Motion::None) {
		if (!motionIsTransient(motion.motion)) {
			// Wave never settles; its clock wraps at a whole number of
			// cycles so the float stays small enough to resolve a frame.
			ctx->animTime += seconds;

			const float period = motion.speed > 0.0f ? 1.0f / motion.speed : 0.0f;

			if (period > 0.0f && ctx->animTime > period * 1024.0f)
				ctx->animTime = std::fmod(ctx->animTime, period);
		} else if (motion.trigger == MotionTrigger::Loop) {
			ctx->animTime += seconds;

			if (ctx->animTime >= std::max(motion.loopInterval, ctx->animEnd - 0.25f)) {
				ctx->animTime = 0.0f;
				ctx->scopeChanged = false;
			}
		} else if (ctx->animTime < ctx->animEnd) {
			// A one-shot runs to the end of its pass and stops there, so a
			// source left up for hours is not adding 1/60 to a huge float.
			ctx->animTime += seconds;
		}
	}

	// ---- scrolling -------------------------------------------------------
	const ScrollSpec &scroll = ctx->authored.scroll;

	if (scroll.enabled) {
		const Metrics &m = ctx->metrics;

		auto advance = [&](float &travel, float speed, float box, float extent) {
			if (speed == 0.0f) {
				travel = 0.0f;
				return;
			}

			travel += std::fabs(speed) * seconds;

			const float period = std::max(extent + scroll.gap, 1.0f);

			if (scroll.loop) {
				// Once the first copy and its gap have left, the second copy
				// is exactly where the first started.
				while (travel >= box + period)
					travel -= period;
			} else {
				travel = std::min(travel, box + extent + 1.0f);
			}
		};

		advance(ctx->travelX, scroll.speedX, m.width, m.contentX1 - m.contentX0);
		advance(ctx->travelY, scroll.speedY, m.height, m.contentY1 - m.contentY0);
	} else {
		ctx->travelX = 0.0f;
		ctx->travelY = 0.0f;
	}
}

// ---- show / activate ---------------------------------------------------------

MotionTrigger triggerOf(SluggedSource *ctx)
{
	const auto config = configOf(ctx);

	return config ? config->document.motion.trigger : MotionTrigger::Never;
}

// Visible anywhere: the program, the Studio Mode preview, a projector.
void showSource(void *data)
{
	auto *ctx = static_cast<SluggedSource *>(data);

	if (triggerOf(ctx) == MotionTrigger::Show)
		ctx->replayRequested = true;
}

// Live on the program output.
void activateSource(void *data)
{
	auto *ctx = static_cast<SluggedSource *>(data);

	if (triggerOf(ctx) == MotionTrigger::Activate)
		ctx->replayRequested = true;
}

// ---- render ------------------------------------------------------------------

void uploadPending(SluggedSource *ctx)
{
	if (!ctx->pendingUpload)
		return;

	const FrameOutput &frame = *ctx->pendingUpload;
	const MotionSpec &motion = ctx->authored.motion;

	const bool replay = ctx->replayOnFrame != 0 && ctx->pendingUploadId >= ctx->replayOnFrame;

	// Old text animates out first when asked, and only when there is old text
	// on screen to animate.
	const bool exit = replay && ctx->replayIsTextChange && motion.exit && motionIsTransient(motion.motion) &&
			  ctx->renderer.hasGeometry();

	const uint32_t previousUnits = ctx->metrics.units;

	if (ctx->renderer.upload(frame, exit)) {
		ctx->metrics.width = float(frame.width);
		ctx->metrics.height = float(frame.height);
		ctx->metrics.contentX0 = frame.contentX0;
		ctx->metrics.contentY0 = frame.contentY0;
		ctx->metrics.contentX1 = frame.contentX1;
		ctx->metrics.contentY1 = frame.contentY1;
		ctx->metrics.clip = frame.clip;
		ctx->metrics.units = frame.units;
		ctx->metrics.changedUnits = frame.changedUnits;
	}

	if (replay) {
		ctx->replayOnFrame = 0;
		ctx->scopeChanged = ctx->replayChangedOnly;
		ctx->animTime = 0.0f;
		ctx->animEnd = passDuration(motion, ctx->scopeChanged ? frame.changedUnits : frame.units);

		if (exit) {
			ctx->exitActive = true;
			ctx->exitTime = 0.0f;
			ctx->exitEnd = passDuration(motion, previousUnits) - 0.25f;
		}
	}

	ctx->pendingUpload.reset();
}

// Draws `source` into `render` at its own size, and returns the result.
gs_texture_t *renderInto(gs_texrender_t *&render, uint32_t width, uint32_t height, const std::function<void()> &draw)
{
	if (width == 0 || height == 0)
		return nullptr;

	if (!render)
		render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

	gs_texrender_reset(render);

	if (!gs_texrender_begin(render, width, height))
		return nullptr;

	vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, float(width), 0.0f, float(height), -100.0f, 100.0f);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	draw();

	gs_blend_state_pop();
	gs_texrender_end(render);

	return gs_texrender_get_texture(render);
}

// Renders the source the text is filled with, and places its picture over the
// text so it covers the letters without being stretched out of shape.
void prepareSourceFill(SluggedSource *ctx, Renderer::DrawParams &params)
{
	if (!ctx->fillSource || ctx->renderingFill)
		return;

	obs_source_t *source = obs_weak_source_get_source(ctx->fillSource);

	if (!source)
		return;

	const uint32_t width = obs_source_get_width(source);
	const uint32_t height = obs_source_get_height(source);

	// A source that ends up drawing this one again stops here.
	ctx->renderingFill = true;

	gs_texture_t *texture =
		renderInto(ctx->fillRender, width, height, [source]() { obs_source_video_render(source); });

	ctx->renderingFill = false;

	obs_source_release(source);

	if (!texture)
		return;

	const Metrics &m = ctx->metrics;

	const float x0 = m.contentX0;
	const float y0 = m.contentY0;
	const float boxWidth = std::max(m.contentX1 - m.contentX0, 1.0f);
	const float boxHeight = std::max(m.contentY1 - m.contentY0, 1.0f);

	const float scale = std::max(boxWidth / float(width), boxHeight / float(height));
	const float w = float(width) * scale;
	const float h = float(height) * scale;

	params.sourceTexture = texture;
	params.sourceX0 = x0 + (boxWidth - w) * 0.5f;
	params.sourceY0 = y0 + (boxHeight - h) * 0.5f;
	params.sourceX1 = params.sourceX0 + w;
	params.sourceY1 = params.sourceY0 + h;
}

// `extra` adds to the draw what the caller already knows: the overlay
// filter's picture to fill with, or that the text cuts it out.
void renderText(SluggedSource *ctx, const Renderer::DrawParams *extra = nullptr)
{
	if (!ctx->renderer.loadEffect())
		return;

	uploadPending(ctx);

	if (ctx->releaseOutgoing) {
		ctx->renderer.releaseOutgoing();
		ctx->releaseOutgoing = false;
	}

	const Metrics &m = ctx->metrics;
	const Document &doc = ctx->authored;

	Renderer::DrawParams params;

	if (extra)
		params = *extra;
	else
		prepareSourceFill(ctx, params);

	params.elapsed = ctx->animTime;
	params.motion = doc.motion;
	params.opacity = doc.opacity;
	params.scopeChanged = ctx->scopeChanged;
	params.clip = m.clip;
	params.clipWidth = m.width;
	params.clipHeight = m.height;
	params.loopTime = ctx->loopTime;
	params.shimmerX0 = m.contentX0;
	params.shimmerX1 = m.contentX1;

	if (!params.erase)
		ctx->renderer.drawBackground(params);

	if (ctx->exitActive && ctx->renderer.hasOutgoing()) {
		Renderer::DrawParams exit = params;

		exit.elapsed = ctx->exitTime;
		exit.reverse = true;
		exit.scopeChanged = false;

		ctx->renderer.drawOutgoing(exit);
		return;
	}

	const ScrollSpec &scroll = doc.scroll;

	if (!scroll.enabled || (scroll.speedX == 0.0f && scroll.speedY == 0.0f)) {
		ctx->renderer.draw(params);
		return;
	}

	// Each axis: where the first copy sits, and how far apart the repeats are.
	// Text moving left enters from the right edge and repeats to its right;
	// text moving right enters from the left edge and repeats to its left.
	struct Axis {
		float first = 0.0f;
		float step = 0.0f;
		int count = 1;
	};

	auto axis = [&](float speed, float travel, float box, float c0, float c1) {
		Axis a;

		if (speed == 0.0f)
			return a;

		const float extent = c1 - c0;
		const float period = std::max(extent + scroll.gap, 1.0f);

		if (speed < 0.0f) {
			a.first = box - travel - c0;
			a.step = period;
		} else {
			a.first = travel - extent - c0;
			a.step = -period;
		}

		a.count = scroll.loop ? std::min(64, int(std::ceil(box / period)) + 2) : 1;

		return a;
	};

	const Axis ax = axis(scroll.speedX, ctx->travelX, m.width, m.contentX0, m.contentX1);
	const Axis ay = axis(scroll.speedY, ctx->travelY, m.height, m.contentY0, m.contentY1);

	for (int i = 0; i < ax.count; i++) {
		for (int j = 0; j < ay.count; j++) {
			params.offsetX = ax.first + float(i) * ax.step;
			params.offsetY = ay.first + float(j) * ay.step;

			ctx->renderer.draw(params);
		}
	}
}

void renderSource(void *data, gs_effect_t *)
{
	renderText(static_cast<SluggedSource *>(data));
}

// Draws a premultiplied texture at (0, 0).
void drawTexture(gs_texture_t *texture, uint32_t width, uint32_t height)
{
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);

	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), texture);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(texture, 0, width, height);

	gs_blend_state_pop();
}

void renderFilter(void *data, gs_effect_t *)
{
	auto *ctx = static_cast<SluggedSource *>(data);

	const auto config = configOf(ctx);
	const FilterMode mode = config ? config->filterMode : FilterMode::Overlay;

	if (mode == FilterMode::Overlay) {
		// Draw what the filter is attached to unchanged, then lay text over it.
		obs_source_skip_video_filter(ctx->source);

		renderText(ctx);
		return;
	}

	obs_source_t *target = obs_filter_get_target(ctx->source);
	const uint32_t width = target ? obs_source_get_width(target) : 0;
	const uint32_t height = target ? obs_source_get_height(target) : 0;

	if (width == 0 || height == 0) {
		obs_source_skip_video_filter(ctx->source);
		return;
	}

	Renderer::DrawParams extra;

	if (mode == FilterMode::Mask) {
		// The picture shows only inside the letters: render it aside, and
		// fill the text with it.
		gs_texture_t *picture = renderInto(ctx->filterRender, width, height,
						   [ctx]() { obs_source_skip_video_filter(ctx->source); });

		extra.sourceTexture = picture;
		extra.sourceX1 = float(width);
		extra.sourceY1 = float(height);
		extra.fillAllWithSource = true;

		renderText(ctx, &extra);
		return;
	}

	// Knockout: the picture with the letters cut out of it. The cutting has to
	// happen in a texture of its own, or it would cut through whatever the
	// scene drew underneath too.
	extra.erase = true;

	gs_texture_t *cut = renderInto(ctx->filterRender, width, height, [ctx, &extra]() {
		obs_source_skip_video_filter(ctx->source);
		renderText(ctx, &extra);
	});

	if (cut)
		drawTexture(cut, width, height);
}

const char *sourceName(void *)
{
	return obs_module_text("Slugged");
}

const char *filterName(void *)
{
	return obs_module_text("Slugged.Filter");
}

obs_properties_t *sourceProperties(void *data)
{
	auto *ctx = static_cast<SluggedSource *>(data);

	return settings::properties(ctx ? ctx->source : nullptr);
}

void sourceDefaults(obs_data_t *settingsData)
{
	settings::defaults(settingsData);
}

} // namespace

namespace {

SluggedSource *contextOf(obs_source_t *source)
{
	if (!isSluggedSource(source))
		return nullptr;

	return static_cast<SluggedSource *>(obs_obj_get_data(source));
}

} // namespace

bool isSluggedSource(obs_source_t *source)
{
	if (!source)
		return false;

	const char *id = obs_source_get_unversioned_id(source);

	return id && (std::strcmp(id, kSourceId) == 0 || std::strcmp(id, kFilterId) == 0);
}

void setEditorAttached(obs_source_t *source, bool attached)
{
	if (SluggedSource *ctx = contextOf(source))
		ctx->editorAttached = attached;
}

std::shared_ptr<const EditMap> editMapOf(obs_source_t *source)
{
	SluggedSource *ctx = contextOf(source);

	if (!ctx)
		return nullptr;

	std::lock_guard<std::mutex> lock(ctx->editMutex);

	return ctx->editMap;
}

TimerState timerStateOf(obs_source_t *source)
{
	TimerState state;

	if (SluggedSource *ctx = contextOf(source)) {
		state.seconds = ctx->timerSeconds;
		state.running = ctx->timerRunning;
	}

	return state;
}

std::string expandFor(obs_source_t *source, const std::string &text)
{
	SluggedSource *ctx = contextOf(source);

	if (!ctx)
		return text;

	std::lock_guard<std::mutex> lock(ctx->tokenMutex);

	return ctx->tokens.expand(text);
}

void registerSluggedSource()
{
	static obs_source_info sourceInfo = {};

	sourceInfo.id = kSourceId;
	sourceInfo.type = OBS_SOURCE_TYPE_INPUT;
	sourceInfo.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	sourceInfo.get_name = sourceName;
	sourceInfo.create = createTextSource;
	sourceInfo.destroy = destroySource;
	sourceInfo.update = updateSource;
	sourceInfo.get_defaults = sourceDefaults;
	sourceInfo.get_properties = sourceProperties;
	sourceInfo.get_width = getWidth;
	sourceInfo.get_height = getHeight;
	sourceInfo.video_tick = tickSource;
	sourceInfo.video_render = renderSource;
	sourceInfo.show = showSource;
	sourceInfo.activate = activateSource;
	sourceInfo.icon_type = OBS_ICON_TYPE_TEXT;

	obs_register_source(&sourceInfo);

	static obs_source_info filterInfo = {};

	filterInfo.id = kFilterId;
	filterInfo.type = OBS_SOURCE_TYPE_FILTER;
	filterInfo.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	filterInfo.get_name = filterName;
	filterInfo.create = createFilterSource;
	filterInfo.destroy = destroySource;
	filterInfo.update = updateSource;
	filterInfo.get_defaults = sourceDefaults;
	filterInfo.get_properties = sourceProperties;
	filterInfo.video_tick = tickSource;
	filterInfo.video_render = renderFilter;
	filterInfo.show = showSource;
	filterInfo.activate = activateSource;

	obs_register_source(&filterInfo);
}

} // namespace slugged
