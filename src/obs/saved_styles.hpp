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

#include <string>
#include <vector>

namespace slugged {

// A look worth keeping: the base style of a text, its background, and
// optionally how it moves. It never carries the text itself.
//
// Saved styles live next to the module's own config rather than in a scene
// collection, so the same looks are there in every collection.
struct SavedStyle {
	std::string name;

	Style style;
	Background background;
	float padding = 0.0f;

	bool hasMotion = false;
	MotionSpec motion;
	ScrollSpec scroll;

	bool operator==(const SavedStyle &) const = default;
};

namespace styles {

// Reads the saved styles from disk. Called once at module load.
void load();

std::vector<SavedStyle> all();
bool find(const std::string &name, SavedStyle &out);

// Adds `style`, or replaces the one already saved under its name.
void put(const SavedStyle &style);
void remove(const std::string &name);

// False when `to` is empty or already taken.
bool rename(const std::string &from, const std::string &to);

// The look of `doc`, as a style called `name`.
SavedStyle capture(const Document &doc, const std::string &name, bool withMotion);

// Gives the whole text the style. Parts styled on their own keep what they
// override, exactly as when the base style is changed any other way.
void apply(Document &doc, const SavedStyle &style);

// Gives [start, end) the style's character styling, replacing whatever those
// characters had. Background and motion belong to the whole text and are left
// alone.
void apply(Document &doc, size_t start, size_t end, const SavedStyle &style);

} // namespace styles

} // namespace slugged
