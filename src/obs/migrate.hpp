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

#include <obs-module.h>

#include <string>
#include <vector>

namespace slugged {
namespace migrate {

// Conversion from OBS's built-in text sources (GDI+ and FreeType 2).

// True when `sourceId` is a text source Slugged can convert.
bool isLegacyTextSource(const char *sourceId);

// The names of every convertible text source in the current scene collection.
std::vector<std::string> legacySourceNames();

// Writes the Slugged equivalent of `legacy`'s configuration into `target`:
// font, colours (gradients included), outline, shadow, alignment, background,
// size and wrapping, and file or chat-log reading.
bool importSettings(obs_source_t *legacy, obs_data_t *target);

struct ConvertOptions {
	// Remove the original source once replaced; otherwise it is renamed with
	// an " (old)" suffix and taken out of every scene.
	bool deleteOriginal = false;
};

// Replaces `legacy` with a Slugged source of the same name in every scene and
// group that uses it, keeping each item's position, transform, crop,
// visibility, order and filters. Returns the new source, or nullptr.
obs_source_t *convert(obs_source_t *legacy, const ConvertOptions &options);

} // namespace migrate
} // namespace slugged
