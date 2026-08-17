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

#include "editor_bridge.hpp"
#include "../util/log.hpp"

// Built in place of ui/editor_window.cpp when the plugin is configured without
// Qt. The source itself is fully functional from the standard properties
// dialog, so a Qt-less build is a legitimate configuration rather than a broken
// one -- it simply has no editor window.

namespace slugged {
namespace editor {

void openFor(obs_source_t *source)
{
	UNUSED_PARAMETER(source);

	slugged_log(LOG_WARNING, "this build of Slugged was compiled without Qt, so the editor is unavailable; "
				 "use the properties below instead");
}

void closeFor(obs_source_t *source)
{
	UNUSED_PARAMETER(source);
}

void shutdown() {}

} // namespace editor
} // namespace slugged
