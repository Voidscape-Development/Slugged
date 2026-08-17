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

#include "obs/editor_bridge.hpp"
#include "obs/slugged_source.hpp"
#include "text/font_manager.hpp"
#include "util/log.hpp"

#include <obs-module.h>
#include <plugin-support.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

// Bridges the plugin's internal logging to OBS's, so core/ and text/ never need
// to include libobs themselves.
void logToObs(int level, const char *format, va_list args)
{
	blogva(level, format, args);
}

} // namespace

bool obs_module_load(void)
{
	slugged::setLogSink(logToObs);

	slugged::registerSluggedSource();

	obs_log(LOG_INFO, "Slugged loaded (version %s)", PLUGIN_VERSION);

	return true;
}

void obs_module_unload(void)
{
	slugged::editor::shutdown();

	// Faces hold FreeType and HarfBuzz objects; drop them before the module's
	// copies of those libraries go away.
	slugged::FontManager::instance().reset();

	slugged::setLogSink(nullptr);

	obs_log(LOG_INFO, "Slugged unloaded");
}
