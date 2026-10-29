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

#include <cstdint>
#include <string>
#include <vector>

namespace slugged {

// Word-wrap opportunities for UTF-8 `text`: element i is true when a line may
// start at byte i.
//
// A practical subset of UAX #14 rather than the whole algorithm: breaks after
// spaces, after hyphens and dashes between letters, after a zero-width space,
// and between ideographs, kana and hangul -- which have no spaces to break at --
// while keeping closing punctuation off the start of a line and opening
// punctuation off the end of one.
std::vector<bool> lineBreakOpportunities(const std::string &text);

} // namespace slugged
