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
#include "migrate.hpp"
#include "../util/log.hpp"

#include <algorithm>
#include <cmath>

namespace slugged {

const char *const kSourceId = "slugged_text";
const char *const kFilterId = "slugged_text_filter";

namespace {

// Document text with tokens expanded, or the file's contents in file mode.
std::string resolveText(SluggedSource *ctx)
{
	if (ctx->mode == TextSourceMode::File)
		return ctx->tokens.expand(ctx->feed.text());

	const std::string plain = ctx->document.plainText();

	return TokenContext::hasTokens(plain) ? ctx->tokens.expand(plain) : plain;
}

} // namespace

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

	// In file mode, or when tokens are present, the visible text differs from
	// what the document stores; substitute it while preserving styling.
	const std::string resolved = resolvedText;

	if (mode == TextSourceMode::File || local.plainText() != resolved)
		local.setPlainText(resolved);

	const float available =
		local.sizeMode == SizeMode::Fixed ? std::max(0.0f, local.boxWidth - 2.0f * local.padding) : 0.0f;

	layout = Layout::run(local, available);

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

	std::lock_guard<std::mutex> lock(ctx->mutex);

	settings::load(settings, ctx->document, ctx->snapshot);

	ctx->mode = obs_data_get_int(settings, "mode") == 1 ? TextSourceMode::File : TextSourceMode::Document;

	ctx->feedConfig.path = obs_data_get_string(settings, "file");
	ctx->feedConfig.chatlog = obs_data_get_bool(settings, "chatlog");
	ctx->feedConfig.chatlogLines = int(obs_data_get_int(settings, "chatlog_lines"));

	ctx->feed.invalidate();
	ctx->dirty = true;
}

void *createSource(obs_data_t *settings, obs_source_t *source, bool isFilter)
{
	auto *ctx = new SluggedSource();

	ctx->source = source;
	ctx->isFilter = isFilter;

	updateSource(ctx, settings);

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

	ctx->elapsed += seconds;
	ctx->tokens.tick(ctx->elapsed);

	if (ctx->mode == TextSourceMode::File && ctx->feed.tick(ctx->feedConfig, seconds))
		ctx->dirty = true;

	// Tokens are re-expanded every tick, but a re-layout only happens when the
	// expansion actually produced different text -- a clock token changes once
	// a minute, not sixty times a second.
	const std::string resolved = resolveText(ctx);

	if (resolved != ctx->resolvedText) {
		ctx->resolvedText = resolved;
		ctx->dirty = true;

		std::lock_guard<std::mutex> lock(ctx->mutex);

		if (ctx->document.motion.replayOnChange)
			ctx->elapsed = 0.0f;
	}

	ScrollSpec scroll;

	{
		std::lock_guard<std::mutex> lock(ctx->mutex);

		scroll = ctx->document.scroll;
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

	ctx->renderer.draw(ctx->elapsed, local.opacity);

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

			ctx->renderer.draw(ctx->elapsed, local.opacity);

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

	obs_register_source(&filterInfo);
}

} // namespace slugged
