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

#pragma once

#include "../core/document.hpp"
#include "../core/text_feed.hpp"
#include "../core/tokens.hpp"
#include "../render/atlas_cache.hpp"
#include "../render/geometry.hpp"
#include "../render/renderer.hpp"
#include "../text/layout.hpp"
#include "settings.hpp"

#include <atomic>
#include <mutex>
#include <string>

#include <obs-module.h>

namespace slugged {

extern const char *const kSourceId;
extern const char *const kFilterId;

// State for one Slugged source or filter instance.
//
// The editor mutates `document` from the Qt thread while the graphics thread
// reads it, so every access goes through `mutex`. The rebuild itself happens on
// the graphics thread inside render(), where a GPU context is guaranteed.
struct SluggedSource {
	obs_source_t *source = nullptr;
	bool isFilter = false;

	std::mutex mutex;

	Document document;
	settings::SettingsSnapshot snapshot;

	// Guards `tokens` on its own, because a script can push a variable in from
	// any thread at any time while the graphics thread is expanding text. Kept
	// separate from `mutex` so pushing a variable never contends with a rebuild
	// reading the document.
	std::mutex tokenMutex;
	TokenContext tokens;

	TextSourceMode mode = TextSourceMode::Document;
	TextFeed feed;
	TextFeed::Config feedConfig;

	// The source's own variables file, polled the same way the text file is.
	TextFeed variableFeed;
	TextFeed::Config variableFeedConfig;

	// Generation of the host token table this source last copied in, so the
	// OBS-derived values are only pulled across when one of them moved.
	uint64_t hostVariableGeneration = 0;

	// Text after token expansion and file reading, as last laid out. Compared
	// each tick so a re-layout only happens when the result actually differs.
	std::string resolvedText;

	AtlasCache atlas;
	Renderer renderer;
	LayoutResult layout;
	GeometryBuffers geometry;

	uint32_t width = 0;
	uint32_t height = 0;

	// The source's own running time. Never reset: {uptime} and {timer} are
	// built on it, and a timer that restarted every time a motion preset
	// replayed would sit at 0:00 forever.
	float uptime = 0.0f;

	// The animation clock, which is what the shader's motion phase is measured
	// from. Reset by every replay trigger.
	float animTime = 0.0f;

	// Seconds after which the current pass has finished for every glyph. The
	// clock stops there for a one-shot preset, so a source left on screen for
	// hours does not run its phase arithmetic out into the range where a float
	// can no longer resolve a 60 Hz step.
	float animEnd = 0.0f;

	float scrollX = 0.0f;
	float scrollY = 0.0f;

	obs_hotkey_id replayHotkey = OBS_INVALID_HOTKEY_ID;

	// Set by any replay trigger and consumed by the next tick. A flag rather
	// than a direct write to `animTime`, because the hotkey, the properties
	// button and a script's proc call all arrive on threads that are not the
	// graphics thread the clock is advanced on.
	std::atomic<bool> replayRequested{true};

	// Set when the document changed and geometry must be rebuilt.
	bool dirty = true;

	// Rebuilds layout, atlas and geometry. Must be called with a graphics
	// context current.
	void rebuild();

	// Restarts the animation clock. Every replay trigger -- a settings change,
	// the source being shown, the loop timer, the button, the hotkey and the
	// `replay_motion` proc -- funnels through here.
	void replayMotion();

	// Applies a document edited in the editor and persists it.
	void applyDocument(const Document &doc);

	// Snapshot of the document for the editor to edit.
	Document documentCopy();
};

// Registers both the source and the filter variants.
void registerSluggedSource();

} // namespace slugged
