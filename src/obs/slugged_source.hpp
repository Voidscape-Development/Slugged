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

	TokenContext tokens;

	TextSourceMode mode = TextSourceMode::Document;
	TextFeed feed;
	TextFeed::Config feedConfig;

	// Text after token expansion and file reading, as last laid out. Compared
	// each tick so a re-layout only happens when the result actually differs.
	std::string resolvedText;

	AtlasCache atlas;
	Renderer renderer;
	LayoutResult layout;
	GeometryBuffers geometry;

	uint32_t width = 0;
	uint32_t height = 0;

	float elapsed = 0.0f;
	float scrollX = 0.0f;
	float scrollY = 0.0f;

	// Set when the document changed and geometry must be rebuilt.
	bool dirty = true;

	// Rebuilds layout, atlas and geometry. Must be called with a graphics
	// context current.
	void rebuild();

	// Applies a document edited in the editor and persists it.
	void applyDocument(const Document &doc);

	// Snapshot of the document for the editor to edit.
	Document documentCopy();
};

// Registers both the source and the filter variants.
void registerSluggedSource();

} // namespace slugged
