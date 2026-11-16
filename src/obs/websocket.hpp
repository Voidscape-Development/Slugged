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

namespace slugged {
namespace websocket {

// Registers Slugged as an obs-websocket vendor, so a client can reach what the
// procs do without a script in between:
//
//   CallVendorRequest { vendorName: "slugged", requestType: "SetVariable",
//                       requestData: { sourceName: "Score", name: "home", value: "3" } }
//
// Requests: SetVariable, GetVariable, IncrementVariable, DecrementVariable,
// ResetVariable, ClearVariables, PlayMotion, NextMessage, TimerStart,
// TimerPause, TimerToggle, TimerReset. Each takes `sourceName` (and
// `filterName` for an overlay filter on that source); the variable requests
// without one act on the table shared by every Slugged source.
//
// Does nothing when obs-websocket is not installed. Must be called from
// obs_module_post_load, once obs-websocket has loaded.
void registerVendor();

} // namespace websocket
} // namespace slugged
