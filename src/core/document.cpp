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

#include "document.hpp"

namespace slugged {

std::vector<std::string> splitLines(const std::string &text)
{
	std::vector<std::string> out;
	std::string cur;

	for (char c : text) {
		if (c == '\n') {
			out.push_back(cur);
			cur.clear();
		} else if (c != '\r') {
			cur += c;
		}
	}

	out.push_back(cur);

	return out;
}

void Document::setPlainText(const std::string &text)
{
	const std::vector<std::string> lines = splitLines(text);

	std::vector<Block> next;
	next.reserve(lines.size());

	for (size_t i = 0; i < lines.size(); i++) {
		Block b;

		// Carry over the styling of the block that previously occupied this
		// slot, so a script rewriting the text does not flatten the look the
		// user built in the editor. Past the end, reuse the last block's
		// styling rather than the bare default.
		const Block *src = nullptr;

		if (i < blocks.size())
			src = &blocks[i];
		else if (!blocks.empty())
			src = &blocks.back();

		if (src) {
			b.align = src->align;
			b.direction = src->direction;
			b.lineHeight = src->lineHeight;
			b.spaceBefore = src->spaceBefore;
			b.spaceAfter = src->spaceAfter;
		}

		Run r;
		r.text = lines[i];
		r.style = (src && !src->runs.empty()) ? src->runs.front().style : defaultStyle;

		b.runs.push_back(std::move(r));
		next.push_back(std::move(b));
	}

	blocks = std::move(next);
}

} // namespace slugged
