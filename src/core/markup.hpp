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

#include "document.hpp"

#include <optional>
#include <string>
#include <vector>

namespace slugged {

// Inline markup for the plain `text` property, so a script, bot or
// obs-websocket client can send styled text without the editor:
//
//   [b]bold[/b]  [i]italic[/i]  [u]underline[/u]  [s]strikeout[/s]
//   [color=#ff8800]...[/color]   (#rgb, #rrggbb, #rrggbbaa or a CSS colour name)
//   [size=48]...[/size]          (pixels)
//   [font=Inter]...[/font]
//   [weight=600]...[/weight]
//   [outline=#000]...[/outline]  (turns the outline on in that colour)
//
// A closing tag ends the most recent open tag of the same name; [/] ends the most
// recent tag of any kind. "[[" is a literal "[". Anything that does not parse as
// a known tag is left in the text as written, so a typo is visible on screen.
//
// Tags nest and may span lines; each line becomes a paragraph.
std::vector<Block> parseMarkup(const std::string &markup);

// Parses "#rgb", "#rrggbb", "#rrggbbaa" or a CSS colour name.
std::optional<Rgba> parseColor(const std::string &text);

} // namespace slugged
