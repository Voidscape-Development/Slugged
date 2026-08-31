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
#include "migrate.hpp"
#include "../util/log.hpp"

#include <algorithm>
#include <cmath>

namespace slugged {

const char *const kSourceId = "slugged_text";
const char *const kFilterId = "slugged_text_filter";

namespace {

// Document text with tokens expanded, or the file's contents in file mode.
//
// The document and the token tables are guarded separately and are never held
// at the same time: a script pushing a variable in should not have to wait
// behind a rebuild, and a rebuild should not have to wait behind a script.
std::string resolveText(SluggedSource *ctx)
{
	std::string plain;

	if (ctx->mode == TextSourceMode::File) {
		plain = ctx->feed.text();
	} else {
		std::lock_guard<std::mutex> lock(ctx->mutex);

		plain = ctx->document.plainText();
	}

	if (!TokenContext::hasTokens(plain))
		return plain;

	std::lock_guard<std::mutex> lock(ctx->tokenMutex);

	return ctx->tokens.expand(plain);
}

} // namespace

void SluggedSource::replayMotion()
{
	replayRequested = true;
}

Document SluggedSource::documentCopy()
{
	std::lock_guard<std::mutex> lock(mutex);

	return document;
}

void SluggedSource::applyDocument(const Document &doc)
{
	{
		std::lock_guard<std::mutex> lock(mutex);

		document = doc;
		dirty = true;
	}

	// Persisting through obs_source_update keeps undo, scene collection saving
	// and obs-websocket's view of the source all consistent.
	obs_data_t *data = obs_source_get_settings(source);

	settings::save(data, doc);
	obs_source_update(source, data);

	obs_data_release(data);
}

void SluggedSource::rebuild()
{
	Document local;

	{
		std::lock_guard<std::mutex> lock(mutex);

		local = document;
	}

	if (mode == TextSourceMode::File) {
		// The whole document is the file's contents, so there is no run
		// structure of its own to preserve.
		local.setPlainText(resolvedText);
	} else {
		// Expanded run by run rather than through setPlainText(), which
		// rebuilds the block list and would collapse every line back to a
		// single run -- silently flattening the per-character styling of any
		// source that contained so much as a {time}.
		std::lock_guard<std::mutex> lock(tokenMutex);

		local.expandTokens([this](const std::string &text) { return tokens.expand(text); });
	}

	// An overlay filter has no box of its own: it draws over whatever it is
	// attached to. Laying out against that source's dimensions is what makes
	// alignment mean anything here -- against the document's own auto size,
	// centring text over a 1920x1080 capture centred it within a box exactly as
	// wide as the text, which is to say not at all.
	if (isFilter && local.sizeMode != SizeMode::Fixed) {
		obs_source_t *target = obs_filter_get_target(source);

		const uint32_t targetWidth = target ? obs_source_get_width(target) : 0;
		const uint32_t targetHeight = target ? obs_source_get_height(target) : 0;

		if (targetWidth > 0 && targetHeight > 0) {
			local.sizeMode = SizeMode::Fixed;
			local.boxWidth = float(targetWidth);
			local.boxHeight = float(targetHeight);
		}
	}

	const float available =
		local.sizeMode == SizeMode::Fixed ? std::max(0.0f, local.boxWidth - 2.0f * local.padding) : 0.0f;

	layout = Layout::run(local, available);

	// How long one pass of the current preset takes, now that it is known how
	// many characters, words or lines the stagger is spread over. The clock
	// stops there, so a source left on screen for a day is not still adding
	// 1/60 to a float too large to resolve the step.
	{
		uint32_t units = 1;

		switch (local.motion.order) {
		case MotionOrder::Together:
			break;
		case MotionOrder::PerGlyph:
			units = uint32_t(layout.glyphs.size());
			break;
		case MotionOrder::PerWord:
			units = layout.wordCount;
			break;
		case MotionOrder::PerLine:
			units = uint32_t(layout.lines.size());
			break;
		}

		animEnd = local.motion.passDuration(units) + 0.25f;
	}

	if (local.sizeMode == SizeMode::Fixed) {
		width = uint32_t(std::max(1.0f, local.boxWidth));
		height = uint32_t(std::max(1.0f, local.boxHeight));
	} else {
		// Auto mode grows to the text. The padding matters here: outlines and
		// shadows extend past the glyph boxes and would otherwise be clipped
		// by the source's own bounds.
		width = uint32_t(std::max(1.0f, std::ceil(layout.width + 2.0f * local.padding)));
		height = uint32_t(std::max(1.0f, std::ceil(layout.height + 2.0f * local.padding)));
	}

	atlas.ensure(layout.glyphs);

	if (!renderer.syncTextures(atlas)) {
		renderer.releaseGeometry();
		return;
	}

	// Layout works from the box origin; the padding offset is applied here so
	// the rest of the pipeline stays in one coordinate space.
	if (local.padding != 0.0f) {
		for (PositionedGlyph &g : layout.glyphs) {
			g.x += local.padding;
			g.y += local.padding;
		}

		for (DecorationRect &d : layout.decorations) {
			d.x += local.padding;
			d.y += local.padding;
		}
	}

	if (GeometryBuilder::build(local, layout, atlas, geometry))
		renderer.setGeometry(geometry);
	else
		renderer.releaseGeometry();

	dirty = false;
}

// ---------------------------------------------------------------------------
// obs_source_info callbacks
// ---------------------------------------------------------------------------

namespace {

const char *sourceName(void *)
{
	return obs_module_text("Slugged");
}

const char *filterName(void *)
{
	return obs_module_text("Slugged.Filter");
}

void updateSource(void *data, obs_data_t *settings)
{
	auto *ctx = static_cast<SluggedSource *>(data);

	bool motionChanged = false;

	{
		std::lock_guard<std::mutex> lock(ctx->mutex);

		const MotionSpec before = ctx->document.motion;

		settings::load(settings, ctx->document, ctx->snapshot);

		motionChanged = ctx->document.motion != before;

		ctx->mode = obs_data_get_int(settings, "mode") == 1 ? TextSourceMode::File : TextSourceMode::Document;

		ctx->feedConfig.path = obs_data_get_string(settings, "file");
		ctx->feedConfig.chatlog = obs_data_get_bool(settings, "chatlog");
		ctx->feedConfig.chatlogLines = int(obs_data_get_int(settings, "chatlog_lines"));

		ctx->variableFeedConfig.path = obs_data_get_string(settings, "variables_file");
	}

	{
		std::lock_guard<std::mutex> lock(ctx->tokenMutex);

		ctx->tokens.setSourceVariables(settings::variablesFromData(settings, settings::kVariablesKey));
	}

	ctx->feed.invalidate();
	ctx->variableFeed.invalidate();
	ctx->dirty = true;

	// Touching the motion controls replays what they configure. Without this
	// the presets that settle were a single event at source creation: choosing
	// Fade on a source that had been on screen for a minute animated nothing at
	// all, because its window had closed fifty-nine seconds earlier.
	if (motionChanged)
		ctx->replayMotion();
}

// ---- scripting entry points -----------------------------------------------
//
// Everything a Slugged source can be driven with from outside is a proc on the
// source, so `obs.obs_source_get_proc_handler` in a Lua or Python script
// reaches all of it without the plugin having to be linked against.

void setVariableProc(void *data, calldata_t *call)
{
	auto *ctx = static_cast<SluggedSource *>(data);

	const char *name = nullptr;
	const char *value = nullptr;

	if (!calldata_get_string(call, "name", &name) || !name || !*name)
		return;

	if (!calldata_get_string(call, "value", &value))
		value = "";

	{
		std::lock_guard<std::mutex> lock(ctx->tokenMutex);

		ctx->tokens.set(name, value ? value : "");
	}

	// The next tick re-expands and notices the difference by itself, so there
	// is nothing to mark dirty here.
}

void clearVariablesProc(void *data, calldata_t *call)
{
	UNUSED_PARAMETER(call);

	auto *ctx = static_cast<SluggedSource *>(data);

	std::lock_guard<std::mutex> lock(ctx->tokenMutex);

	ctx->tokens.setSourceVariables({});
}

void replayProc(void *data, calldata_t *call)
{
	UNUSED_PARAMETER(call);

	static_cast<SluggedSource *>(data)->replayMotion();
}

void replayHotkeyPressed(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (pressed)
		static_cast<SluggedSource *>(data)->replayMotion();
}

void *createSource(obs_data_t *settings, obs_source_t *source, bool isFilter)
{
	auto *ctx = new SluggedSource();

	ctx->source = source;
	ctx->isFilter = isFilter;

	updateSource(ctx, settings);

	proc_handler_t *procs = obs_source_get_proc_handler(source);

	if (procs) {
		proc_handler_add(procs, "void set_variable(string name, string value)", setVariableProc, ctx);
		proc_handler_add(procs, "void clear_variables()", clearVariablesProc, ctx);
		proc_handler_add(procs, "void replay_motion()", replayProc, ctx);
	}

	ctx->replayHotkey = obs_hotkey_register_source(source, "Slugged.Replay", obs_module_text("Hotkey.Replay"),
						       replayHotkeyPressed, ctx);

	return ctx;
}

void *createTextSource(obs_data_t *settings, obs_source_t *source)
{
	return createSource(settings, source, false);
}

void *createFilterSource(obs_data_t *settings, obs_source_t *source)
{
	return createSource(settings, source, true);
}

void destroySource(void *data)
{
	auto *ctx = static_cast<SluggedSource *>(data);

	// The editor holds a pointer to this instance; close it first so it cannot
	// touch freed state.
	editor::closeFor(ctx->source);

	if (ctx->replayHotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(ctx->replayHotkey);

	obs_enter_graphics();
	ctx->renderer.releaseGeometry();
	ctx->renderer.releaseTextures();
	obs_leave_graphics();

	delete ctx;
}

uint32_t getWidth(void *data)
{
	auto *ctx = static_cast<SluggedSource *>(data);

	return ctx->width;
}

uint32_t getHeight(void *data)
{
	auto *ctx = static_cast<SluggedSource *>(data);

	return ctx->height;
}

void tickSource(void *data, float seconds)
{
	auto *ctx = static_cast<SluggedSource *>(data);

	// The source's own running time, which is what {uptime} and {timer} read.
	// Deliberately not the animation clock below: they used to be one value,
	// and replaying on a text change meant a timer reset itself every time it
	// ticked over, leaving it alternating between 0:00 and 0:01 for ever.
	ctx->uptime += seconds;

	host::tick(seconds);

	if (ctx->mode == TextSourceMode::File && ctx->feed.tick(ctx->feedConfig, seconds))
		ctx->dirty = true;

	if (ctx->variableFeed.tick(ctx->variableFeedConfig, seconds)) {
		std::lock_guard<std::mutex> lock(ctx->tokenMutex);

		ctx->tokens.setFileVariables(settings::parseVariableFile(ctx->variableFeed.text()));
	}

	const uint64_t hostGeneration = host::generation();

	if (hostGeneration != ctx->hostVariableGeneration) {
		ctx->hostVariableGeneration = hostGeneration;

		VariableMap values = host::values();

		// {source} is the one host value that differs per source, so it is
		// added here rather than in the shared table.
		const char *name = obs_source_get_name(ctx->source);

		values["source"] = name ? name : "";

		std::lock_guard<std::mutex> lock(ctx->tokenMutex);

		ctx->tokens.setHostVariables(values);
	}

	{
		std::lock_guard<std::mutex> lock(ctx->tokenMutex);

		ctx->tokens.tick(ctx->uptime);
	}

	MotionSpec motion;
	ScrollSpec scroll;

	{
		std::lock_guard<std::mutex> lock(ctx->mutex);

		motion = ctx->document.motion;
		scroll = ctx->document.scroll;
	}

	// Tokens are re-expanded every tick, but a re-layout only happens when the
	// expansion actually produced different text -- a clock token changes once
	// a minute, not sixty times a second.
	const std::string resolved = resolveText(ctx);

	if (resolved != ctx->resolvedText) {
		ctx->resolvedText = resolved;
		ctx->dirty = true;

		// Never is the only trigger that does not care; the rest are a
		// ladder, each adding an occasion to the ones below it.
		if (motion.trigger != MotionTrigger::Never)
			ctx->replayMotion();
	}

	// ---- animation clock -------------------------------------------------
	if (ctx->replayRequested.exchange(false))
		ctx->animTime = 0.0f;

	if (motion.motion != Motion::None) {
		if (!motionIsTransient(motion.motion)) {
			// A preset that never settles keeps its clock running, wrapped
			// at a whole number of cycles: the phase is continuous across
			// the wrap and the float stays small enough to resolve a frame.
			ctx->animTime += seconds;

			const float period = motion.speed > 0.0f ? 1.0f / motion.speed : 0.0f;

			if (period > 0.0f && ctx->animTime > period * 1024.0f)
				ctx->animTime = std::fmod(ctx->animTime, period);
		} else if (motion.trigger == MotionTrigger::Loop) {
			ctx->animTime += seconds;

			if (ctx->animTime >= motion.loopInterval)
				ctx->replayMotion();
		} else if (ctx->animTime < ctx->animEnd) {
			// One-shot: run to the end of the pass and stop there.
			ctx->animTime += seconds;
		}
	}

	if (scroll.enabled) {
		ctx->scrollX += scroll.speedX * seconds;
		ctx->scrollY += scroll.speedY * seconds;

		// Wrapping keeps the offsets bounded so a ticker left running for
		// hours never drifts into float imprecision.
		const float spanX = float(ctx->width) + scroll.gap;
		const float spanY = float(ctx->height) + scroll.gap;

		if (scroll.loop) {
			if (spanX > 1.0f)
				ctx->scrollX = std::fmod(ctx->scrollX, spanX);

			if (spanY > 1.0f)
				ctx->scrollY = std::fmod(ctx->scrollY, spanY);
		}
	} else {
		ctx->scrollX = 0.0f;
		ctx->scrollY = 0.0f;
	}
}

// Called when the source becomes visible, which for a source sitting in a scene
// means every time that scene comes up. That is the moment an intro animation is
// meant to play, so the triggers at or above Show restart on it.
void showSource(void *data)
{
	auto *ctx = static_cast<SluggedSource *>(data);

	MotionTrigger trigger;

	{
		std::lock_guard<std::mutex> lock(ctx->mutex);

		trigger = ctx->document.motion.trigger;
	}

	if (trigger == MotionTrigger::Show || trigger == MotionTrigger::Loop)
		ctx->replayMotion();
}

// Draws the text. Assumes a graphics context is current.
void renderText(SluggedSource *ctx)
{
	if (!ctx->renderer.loadEffect())
		return;

	if (ctx->dirty)
		ctx->rebuild();

	Document local;

	{
		std::lock_guard<std::mutex> lock(ctx->mutex);

		local = ctx->document;
	}

	if (local.backgroundEnabled)
		Renderer::drawBackground(ctx->width, ctx->height, local.backgroundColor);

	if (!ctx->renderer.hasGeometry())
		return;

	const bool scrolling = local.scroll.enabled;

	if (scrolling) {
		gs_matrix_push();
		gs_matrix_translate3f(ctx->scrollX, ctx->scrollY, 0.0f);
	}

	ctx->renderer.draw(ctx->animTime, local.opacity);

	if (scrolling) {
		gs_matrix_pop();

		// A looping ticker draws a second copy one span behind, so the tail
		// and head meet seamlessly instead of the text popping back.
		if (local.scroll.loop) {
			const float spanX = local.scroll.speedX != 0.0f ? float(ctx->width) + local.scroll.gap : 0.0f;
			const float spanY = local.scroll.speedY != 0.0f ? float(ctx->height) + local.scroll.gap : 0.0f;

			gs_matrix_push();
			gs_matrix_translate3f(ctx->scrollX - std::copysign(spanX, local.scroll.speedX),
					      ctx->scrollY - std::copysign(spanY, local.scroll.speedY), 0.0f);

			ctx->renderer.draw(ctx->animTime, local.opacity);

			gs_matrix_pop();
		}
	}
}

void renderSource(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);

	renderText(static_cast<SluggedSource *>(data));
}

void renderFilter(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);

	auto *ctx = static_cast<SluggedSource *>(data);

	// Draw whatever this filter is attached to, unchanged, then lay the text
	// over it. Slugged is an overlay filter rather than an image-processing
	// one, so it never needs to capture the target into a texture.
	obs_source_skip_video_filter(ctx->source);

	renderText(ctx);
}

obs_properties_t *sourceProperties(void *data)
{
	auto *ctx = static_cast<SluggedSource *>(data);

	return settings::properties(ctx ? ctx->source : nullptr);
}

void sourceDefaults(obs_data_t *settings)
{
	settings::defaults(settings);
}

} // namespace

void registerSluggedSource()
{
	static obs_source_info sourceInfo = {};

	sourceInfo.id = kSourceId;
	sourceInfo.type = OBS_SOURCE_TYPE_INPUT;
	sourceInfo.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
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
	sourceInfo.icon_type = OBS_ICON_TYPE_TEXT;

	obs_register_source(&sourceInfo);

	static obs_source_info filterInfo = {};

	filterInfo.id = kFilterId;
	filterInfo.type = OBS_SOURCE_TYPE_FILTER;
	filterInfo.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
	filterInfo.get_name = filterName;
	filterInfo.create = createFilterSource;
	filterInfo.destroy = destroySource;
	filterInfo.update = updateSource;
	filterInfo.get_defaults = sourceDefaults;
	filterInfo.get_properties = sourceProperties;
	filterInfo.video_tick = tickSource;
	filterInfo.video_render = renderFilter;
	filterInfo.show = showSource;

	obs_register_source(&filterInfo);
}

} // namespace slugged
