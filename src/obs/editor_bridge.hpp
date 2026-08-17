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

#include <obs.h>

namespace slugged {
namespace editor {

// Seam between the source and the Qt editor.
//
// Everything Qt lives behind these three functions so the source, the settings
// bridge and the renderer never include a Qt header. When the plugin is built
// without Qt these become no-ops and the source still works fully from the
// standard properties dialog.

// Opens (or raises) the editor window for `source`.
void openFor(obs_source_t *source);

// Closes any editor window bound to `source`. Called when the source is being
// destroyed, so the window cannot outlive what it is editing.
void closeFor(obs_source_t *source);

// Tears down all editor state at module unload.
void shutdown();

} // namespace editor
} // namespace slugged
