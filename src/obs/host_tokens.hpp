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

#include "../core/variables.hpp"

namespace slugged {
namespace host {

// Token values read out of the running OBS instance:
//
//   {stream_time}     how long the current stream has been live
//   {record_time}     how long the current recording has been running, paused
//                     time excluded
//   {scene}           the program scene's name
//   {preview_scene}   the preview scene's name in studio mode
//   {fps}             the canvas frame rate
//   {dropped_frames}  frames the streaming output has dropped
//   {dropped_percent} the same as a percentage of frames sent
//   {cpu}             this OBS process's CPU usage
//   {bitrate}         the streaming output's current outgoing bitrate in kb/s
//
// These are collected here rather than resolved inside TokenContext for two
// reasons: several of them are only reachable through the frontend API, which a
// -DENABLE_FRONTEND_API=OFF build does not link; and the ones that are not
// still cost a lock or a syscall that has no business running once per token
// per frame. Everything is sampled on a timer and handed over as a plain table.
//
// In a build without the frontend API the values libobs alone can answer
// ({fps}, {cpu}) still resolve; the rest resolve to their idle readings -- the
// timers to 0:00, the scene names to nothing, the output stats to zero -- since
// nothing is there to report otherwise.

// Registers the frontend hooks. Called once from the module's load.
void startup();

// Releases them. Called once from the module's unload.
void shutdown();

// Advances the sampling timer. Safe to call from every source every frame; the
// work behind it happens a few times a second.
void tick(float seconds);

// The current values. Cheap: a copy of an already-built table.
VariableMap values();

// Bumped whenever `values()` would return something different, so a source can
// skip re-expanding text that none of these appear in.
uint64_t generation();

} // namespace host
} // namespace slugged
