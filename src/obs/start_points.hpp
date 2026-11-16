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

#include <string>
#include <vector>

#include <obs.h>

namespace slugged {

// A ready-made beginning for a new source: the text, the look and the motion
// of something people commonly make, so the first thing a new user sees is a
// working clock or ticker rather than an empty box and forty settings.
struct StartPoint {
	const char *id;

	// Locale keys.
	const char *name;
	const char *description;
};

const std::vector<StartPoint> &startPoints();

// Rewrites `settings` into the starting point `id`, keeping the font family
// the source already had. Returns false for an id that does not exist.
bool applyStartPoint(obs_data_t *settings, const std::string &id);

// True for a source nobody has put anything into yet, which is when the editor
// offers the starting points unasked.
bool isUntouched(obs_data_t *settings);

} // namespace slugged
