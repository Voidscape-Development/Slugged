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

#include "../text/edit_map.hpp"

#include <memory>
#include <string>

#include <obs.h>

namespace slugged {

extern const char *const kSourceId;
extern const char *const kFilterId;

// Registers the source and the overlay filter.
void registerSluggedSource();

// ---- for the editor ----------------------------------------------------------
//
// Each of these accepts any source and does nothing for one that is not a
// Slugged source or filter. All are safe from the UI thread.

bool isSluggedSource(obs_source_t *source);

// While an editor is attached, every frame the source builds carries an edit
// map: where its text sits, for typing on the preview.
void setEditorAttached(obs_source_t *source, bool attached);

// The edit map of the frame currently shown, or null.
std::shared_ptr<const EditMap> editMapOf(obs_source_t *source);

// The source's stopwatch.
struct TimerState {
	float seconds = 0.0f;
	bool running = false;
};

TimerState timerStateOf(obs_source_t *source);

// `text` with every token replaced by what this source would show for it now.
std::string expandFor(obs_source_t *source, const std::string &text);

} // namespace slugged
