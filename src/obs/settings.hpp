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
#include "../core/variables.hpp"

#include <obs-module.h>

#include <string>

namespace slugged {

// Bridges the document model to OBS's settings store.
//
// There is exactly one place each value lives:
//
//   - The base style, paragraph defaults, layout, motion and scrolling are
//     top-level settings keys. They are what the properties dialog shows and
//     edits, what obs-websocket's SetInputSettings writes, and what the editor
//     writes when nothing is selected.
//   - Per-character styling lives under "content" as runs that record only the
//     fields they override. Changing the base never touches it, so styling one
//     word in the editor survives any change made from anywhere else.
//   - "text" is the plain text. The content's text is kept equal to it; when
//     something outside the editor changes "text", the content is updated with
//     a minimal edit so the unchanged characters keep their styling.
//
// Enumerations are stored as strings ("fade", "center"), never as numbers.
// "schema" records the layout version, so older settings are migrated once.
namespace settings {

constexpr int kSchemaVersion = 2;

// Where a source's text comes from.
enum class SourceMode { Text, File };

// Everything update() reads, resolved into one immutable value.
struct SourceConfig {
	Document document;

	// The raw `text` value: markup when `markup` is set.
	std::string text;
	bool markup = false;

	SourceMode mode = SourceMode::Text;
	TextFeed::Config file;
	TextFeed::Config variablesFile;

	VariableMap variables;

	// True when `text` had been changed from outside and the content was
	// brought in line with it, so the caller can store the updated content.
	bool contentAdjusted = false;
};

void defaults(obs_data_t *data);

obs_properties_t *properties(obs_source_t *source);

// Upgrades settings written by an older version in place. Safe to call on
// settings that are already current.
void migrate(obs_data_t *data);

SourceConfig load(obs_data_t *data);

// Writes `doc` back: base style, layout, motion, scroll, content and the plain
// text mirror. In markup mode the text and its inline styling belong to the
// `text` property, so only the base style and layout are written.
void save(obs_data_t *data, const Document &doc, bool markup);

// Writes just the per-character content and the plain text mirror.
void saveContent(obs_data_t *data, const Document &doc);

// Parses the stored content into `doc`'s blocks, or leaves `doc` with a single
// empty paragraph when there is none.
void contentFromData(obs_data_t *data, Document &doc);

// The per-source variable table: an editable list of "name=value" strings, so
// obs-websocket, Streamer.bot and a two-line script can all write it.
extern const char *const kVariablesKey;

VariableMap variablesFromData(obs_data_t *data, const char *key);
void variablesToData(obs_data_t *data, const char *key, const VariableMap &values);

// A watched variables file: a JSON object of name/value pairs, or "name=value"
// lines.
VariableMap parseVariableFile(const std::string &text);

// The process-wide variable table lives next to the module's own config rather
// than in a scene collection, because every collection shares it.
void loadGlobalVariables();
void saveGlobalVariables();

} // namespace settings

} // namespace slugged
