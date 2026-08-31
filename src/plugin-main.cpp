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

#include "core/variables.hpp"
#include "obs/editor_bridge.hpp"
#include "obs/host_tokens.hpp"
#include "obs/settings.hpp"
#include "obs/slugged_source.hpp"
#include "text/font_manager.hpp"
#include "util/log.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <string>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

// Bridges the plugin's internal logging to OBS's, so core/ and text/ never need
// to include libobs themselves.
void logToObs(int level, const char *format, va_list args)
{
	blogva(level, format, args);
}

// ---- global variable procs ------------------------------------------------
//
// These hang off OBS's own proc handler rather than a source's, because the
// table they write is shared by every Slugged source. A script reaches them
// with two lines and no reference to any particular source:
//
//   local cd = obs.calldata_create()
//   obs.calldata_set_string(cd, "name", "followers")
//   obs.calldata_set_string(cd, "value", "1234")
//   obs.proc_handler_call(obs.obs_get_proc_handler(), "slugged_set_variable", cd)
//
// Every Slugged source picks the new value up on its next frame.

void setGlobalVariable(void *, calldata_t *call)
{
	const char *name = nullptr;
	const char *value = nullptr;

	if (!calldata_get_string(call, "name", &name) || !name || !*name)
		return;

	if (!calldata_get_string(call, "value", &value))
		value = "";

	slugged::GlobalVariables::set(name, value ? value : "");
}

void getGlobalVariable(void *, calldata_t *call)
{
	const char *name = nullptr;

	if (!calldata_get_string(call, "name", &name) || !name)
		return;

	std::string value;

	slugged::GlobalVariables::lookup(name, value);

	calldata_set_string(call, "value", value.c_str());
}

void eraseGlobalVariable(void *, calldata_t *call)
{
	const char *name = nullptr;

	if (calldata_get_string(call, "name", &name) && name)
		slugged::GlobalVariables::erase(name);
}

void clearGlobalVariables(void *, calldata_t *)
{
	slugged::GlobalVariables::clear();
}

} // namespace

bool obs_module_load(void)
{
	slugged::setLogSink(logToObs);

	slugged::settings::loadGlobalVariables();
	slugged::host::startup();

	slugged::registerSluggedSource();

	if (proc_handler_t *procs = obs_get_proc_handler()) {
		proc_handler_add(procs, "void slugged_set_variable(string name, string value)", setGlobalVariable,
				 nullptr);
		proc_handler_add(procs, "void slugged_get_variable(in string name, out string value)",
				 getGlobalVariable, nullptr);
		proc_handler_add(procs, "void slugged_erase_variable(string name)", eraseGlobalVariable, nullptr);
		proc_handler_add(procs, "void slugged_clear_variables()", clearGlobalVariables, nullptr);
	}

	obs_log(LOG_INFO, "Slugged loaded (version %s)", PLUGIN_VERSION);

	return true;
}

void obs_module_unload(void)
{
	slugged::editor::shutdown();

	slugged::settings::saveGlobalVariables();
	slugged::host::shutdown();

	// Faces hold FreeType and HarfBuzz objects; drop them before the module's
	// copies of those libraries go away.
	slugged::FontManager::instance().reset();

	slugged::setLogSink(nullptr);

	obs_log(LOG_INFO, "Slugged unloaded");
}
