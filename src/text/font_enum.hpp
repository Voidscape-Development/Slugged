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

#include <cstdint>
#include <string>
#include <vector>

namespace slugged {

struct FontFileRef {
	std::string path;
	int faceIndex = 0;
};

// System font enumeration and matching.
//
// Three backends, one per platform: DirectWrite on Windows, CoreText on macOS,
// fontconfig on Linux. Each returns real file paths, because slughorn's
// FreeType backend needs a file (or memory blob) rather than a platform font
// handle -- the platform APIs are used for *finding* fonts, never for shaping
// or rasterising, so text lays out identically on all three.
namespace fontenum {

// Every installed family name, sorted, deduplicated. Drives the editor's font
// picker.
std::vector<std::string> families();

// Best file for a family at a given weight/slant. Returns false when the family
// is not installed at all.
bool match(const std::string &family, int weight, bool italic, FontFileRef &out);

// Ordered fallback candidates for a codepoint, best first. The platform APIs
// answer this far better than a hardcoded list can, since they know which fonts
// are actually installed and which script the codepoint belongs to.
std::vector<FontFileRef> fallbacks(uint32_t codepoint, const std::string &preferredFamily);

// Family that should stand in when the requested one is missing.
std::string defaultFamily();

} // namespace fontenum

} // namespace slugged
