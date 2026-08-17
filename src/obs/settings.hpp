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

#include <obs-module.h>

#include <string>

namespace slugged {

// Bridges the document model to OBS's settings store.
//
// The rich document is the canonical form and lives under the "document" key.
// Alongside it sit flat, GDI+-shaped properties (text, font, colour, outline,
// alignment). Those exist so that everything already built around OBS text
// sources keeps working: obs-websocket, Lua and Python scripts, and Streamer.bot
// all set a plain `text` string, and the properties dialog stays usable without
// opening the editor.
//
// The two are kept coherent by only writing a flat property back into the
// document when that property actually changed, which is what SettingsSnapshot
// tracks. Without it, every update() would flatten per-run styling the editor
// had applied.
namespace settings {

// Values of the flat properties as of the last update, so the next update can
// tell which of them the user actually touched.
struct SettingsSnapshot {
	std::string text;
	std::string fontFace;
	std::string fontStyle;
	int fontSize = 64;
	uint32_t color = 0xFFFFFFFF;
	int opacity = 100;
	bool outline = false;
	int outlineSize = 2;
	uint32_t outlineColor = 0xFF000000;
	bool shadow = false;
	int align = 0;
	int valign = 0;
	bool background = false;
	uint32_t backgroundColor = 0x80000000;
	bool extents = false;
	int extentsWidth = 640;
	int extentsHeight = 200;
	int wrap = 1;

	bool valid = false;
};

void defaults(obs_data_t *data);

obs_properties_t *properties(void *sourceData);

// Reads the full document. Applies any flat property the user changed since
// `snapshot` on top of it, then refreshes `snapshot`.
void load(obs_data_t *data, Document &doc, SettingsSnapshot &snapshot);

// Writes `doc` back, including refreshing the flat mirror properties so scripts
// and the properties dialog see what the editor produced.
void save(obs_data_t *data, const Document &doc);

// Serialises a document to and from a nested obs_data object, for the editor's
// preset save/load.
obs_data_t *documentToData(const Document &doc);
void documentFromData(obs_data_t *data, Document &doc);

} // namespace settings

} // namespace slugged
