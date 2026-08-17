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

namespace slugged {
namespace migrate {

// Import from OBS's built-in text sources.
//
// Switching overlay to a new text source normally means rebuilding every text
// element by hand. This reads an existing text_gdiplus or text_ft2_source and
// reproduces its font, colour, outline, alignment, background, wrapping and
// file/chatlog configuration as a Slugged document, so switching is one click.

// True when `sourceId` is a text source Slugged knows how to import.
bool isLegacyTextSource(const char *sourceId);

// Fills `list` with the names of every importable text source in the current
// scene collection.
void fillImportList(obs_property_t *list);

// Reads the named legacy source and writes the equivalent configuration into
// `target`'s settings. Returns false when the source is missing or not a text
// source.
bool importInto(obs_data_t *targetSettings, const char *legacySourceName);

} // namespace migrate
} // namespace slugged
