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
#include <memory>
#include <mutex>
#include <optional>
#include <string>

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

	std::atomic<uint32_t> width{0};
	std::atomic<uint32_t> height{0};

	obs_hotkey_id replayHotkey = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id timerToggleHotkey = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id timerResetHotkey = OBS_INVALID_HOTKEY_ID;

	// ---- graphics thread -------------------------------------------------

	std::shared_ptr<const SourceConfig> current;
	uint64_t seenConfigVersion = 0;

	TextFeed feed;
	TextFeed variableFeed;
	uint64_t hostGeneration = 0;

	// The document as authored -- file contents or markup applied, tokens not
	// yet expanded -- and its plain text.
	Document authored;
	std::string authoredPlain;
	bool authoredValid = false;

	std::string lastExpanded;
	std::string lastContent;
	bool built = false;

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
	}

	ctx->replayHotkey = obs_hotkey_register_source(source, "Slugged.Replay", obs_module_text("Hotkey.Replay"),
						       replayHotkey, ctx);
	ctx->timerToggleHotkey = obs_hotkey_register_source(
		source, "Slugged.TimerToggle", obs_module_text("Hotkey.TimerToggle"), timerToggleHotkey, ctx);
	ctx->timerResetHotkey = obs_hotkey_register_source(source, "Slugged.TimerReset",
							   obs_module_text("Hotkey.TimerReset"), timerResetHotkey, ctx);

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

	for (obs_hotkey_id id : {ctx->replayHotkey, ctx->timerToggleHotkey, ctx->timerResetHotkey})
		if (id != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(id);

	{
		std::lock_guard<std::mutex> lock(ctx->job->mutex);

		ctx->job->cancelled = true;
		ctx->job->pending.reset();
		ctx->job->ready.reset();
	}

	obs_enter_graphics();
	ctx->renderer.release();
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

// Rebuilds `authored` from the config and, in file mode, the file's contents.
void refreshAuthored(SluggedSource *ctx, const SourceConfig &config)
{
	ctx->authored = config.document;

	if (config.mode == settings::SourceMode::File) {
		const std::string &text = ctx->feed.text();

		// The file's text replaces the configured text through the same
		// minimal edit a script's would, so styling the first line of a
		// template keeps applying to whatever the file says there.
		if (config.markup)
			ctx->authored.blocks = parseMarkup(text);
		else
			ctx->authored.setPlainText(text);
	}

	ctx->authoredPlain = ctx->authored.plainText();
	ctx->authoredValid = true;
}

void scheduleBuild(SluggedSource *ctx, const std::string &expanded, const std::string &content)
{
	const MotionSpec &motion = ctx->authored.motion;

	const uint64_t id = ctx->nextFrameId++;

	if (!ctx->built) {
		// The first text a source shows plays its intro.
		ctx->replayOnFrame = id;
		ctx->replayChangedOnly = false;
		ctx->replayIsTextChange = false;
		ctx->animRange = Range{};
	} else if (content != ctx->lastContent) {
		ctx->animRange = diffRange(ctx->lastExpanded, expanded);

		if (motion.trigger == MotionTrigger::TextChange && motionIsTransient(motion.motion)) {
			ctx->replayOnFrame = id;
			ctx->replayChangedOnly = motion.scope == MotionScope::Changed;
			ctx->replayIsTextChange = true;
		}
	} else {
		ctx->animRange = shiftRange(ctx->animRange, ctx->lastExpanded, expanded);
	}

	FrameInput input;

	input.document = ctx->authored;

	{
		std::lock_guard<std::mutex> lock(ctx->tokenMutex);

		input.document.expandTokens([ctx](const std::string &text) { return ctx->tokens.expand(text); });
	}

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

	ctx->pendingUpload = std::move(ready);
	ctx->pendingUploadId = id;
}

void tickSource(void *data, float seconds)
{
	auto *ctx = static_cast<SluggedSource *>(data);

	ctx->uptime += seconds;

	if (ctx->timerResetRequested.exchange(false))
		ctx->timer = 0.0f;

	if (ctx->timerRunning)
		ctx->timer += seconds;

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

	if (needBuild || !ctx->built)
		scheduleBuild(ctx, expanded, content);

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

void renderText(SluggedSource *ctx)
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

	params.elapsed = ctx->animTime;
	params.opacity = doc.opacity;
	params.scopeChanged = ctx->scopeChanged;
	params.clip = m.clip;
	params.clipWidth = m.width;
	params.clipHeight = m.height;

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

void renderFilter(void *data, gs_effect_t *)
{
	auto *ctx = static_cast<SluggedSource *>(data);

	// Draw what the filter is attached to unchanged, then lay text over it.
	obs_source_skip_video_filter(ctx->source);

	renderText(ctx);
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
