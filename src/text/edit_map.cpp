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

#include "edit_map.hpp"

#include <algorithm>
#include <cmath>

namespace slugged {

namespace {

// Maps an offset in the expanded text back to the text as written. An offset
// inside a replaced token has no single answer, so `inside` reports the token
// it fell in instead.
uint32_t toWritten(const std::vector<TokenSpan> &spans, uint32_t offset, const TokenSpan **inside)
{
	int64_t shift = 0;

	for (const TokenSpan &span : spans) {
		if (offset < span.dstStart)
			break;

		if (offset < span.dstEnd) {
			if (inside)
				*inside = &span;

			return span.srcStart;
		}

		shift += int64_t(span.srcEnd - span.srcStart) - int64_t(span.dstEnd - span.dstStart);
	}

	return uint32_t(int64_t(offset) + shift);
}

float leading(const EditMap::Cell &cell)
{
	return cell.rtl ? cell.x1 : cell.x0;
}

float trailing(const EditMap::Cell &cell)
{
	return cell.rtl ? cell.x0 : cell.x1;
}

} // namespace

EditMap EditMap::build(const LayoutResult &layout, const std::vector<TokenSpan> &spans, float dx, float dy,
		       uint32_t textSize)
{
	EditMap map;

	map.textSize = textSize;
	map.lines.reserve(layout.lines.size());

	for (const LineInfo &info : layout.lines) {
		Line line;

		line.top = info.baselineY - info.ascent + dy;
		line.bottom = info.baselineY + info.descent + dy;
		line.x0 = info.x + dx;
		line.x1 = info.x + info.width + dx;
		line.rtl = info.rtl;
		line.start = toWritten(spans, info.textStart, nullptr);

		// A line's end is where the next one starts, which is a boundary
		// between characters and so never inside a token.
		const TokenSpan *inside = nullptr;
		line.end = toWritten(spans, info.textEnd, &inside);

		if (inside && info.textEnd > inside->dstStart)
			line.end = inside->srcEnd;

		map.lines.push_back(std::move(line));
	}

	for (const CaretCell &source : layout.cells) {
		if (source.lineIndex >= map.lines.size())
			continue;

		Line &line = map.lines[source.lineIndex];

		Cell cell;

		cell.x0 = std::min(source.x0, source.x1) + dx;
		cell.x1 = std::max(source.x0, source.x1) + dx;
		cell.rtl = source.rtl;

		const TokenSpan *inside = nullptr;

		cell.start = toWritten(spans, source.start, &inside);

		if (inside) {
			cell.end = inside->srcEnd;
			cell.token = true;

			// Every cluster of one token is the same unit.
			if (!line.cells.empty() && line.cells.back().token && line.cells.back().start == cell.start) {
				Cell &previous = line.cells.back();

				previous.x0 = std::min(previous.x0, cell.x0);
				previous.x1 = std::max(previous.x1, cell.x1);

				continue;
			}
		} else {
			cell.end = toWritten(spans, source.end, nullptr);
		}

		line.cells.push_back(cell);
	}

	return map;
}

size_t EditMap::lineOf(uint32_t offset) const
{
	// The last line that starts at or before the offset: at a wrap, the caret
	// belongs to the start of the new line rather than the end of the old one.
	size_t found = 0;

	for (size_t i = 0; i < lines.size(); i++)
		if (lines[i].start <= offset)
			found = i;

	return found;
}

EditMap::Rect EditMap::caret(uint32_t offset) const
{
	if (lines.empty())
		return {};

	const Line &line = lines[lineOf(offset)];

	Rect rect;

	rect.y = line.top;
	rect.height = line.bottom - line.top;

	if (line.cells.empty()) {
		// An empty line: where its first character would go.
		rect.x = line.rtl ? line.x1 : line.x0;
		return rect;
	}

	const Cell *before = nullptr;

	for (const Cell &cell : line.cells) {
		if (cell.start == offset) {
			rect.x = leading(cell);
			return rect;
		}

		if (cell.end == offset)
			before = &cell;

		if (offset > cell.start && offset < cell.end) {
			// Inside a ligature, split the cell evenly; inside a token,
			// there is nowhere to stand but its ends.
			const float t = cell.token ? 1.0f : float(offset - cell.start) / float(cell.end - cell.start);

			rect.x = leading(cell) + (trailing(cell) - leading(cell)) * t;
			return rect;
		}
	}

	if (before) {
		rect.x = trailing(*before);
		return rect;
	}

	// Past everything drawn on this line -- trailing spaces do not draw -- or
	// before it: the line's own end.
	const bool atEnd = offset >= line.cells.front().start;

	rect.x = (atEnd != line.rtl) ? line.x1 : line.x0;

	return rect;
}

uint32_t EditMap::hitTest(float x, float y) const
{
	if (lines.empty())
		return 0;

	size_t index = 0;
	float best = 0.0f;

	for (size_t i = 0; i < lines.size(); i++) {
		const Line &line = lines[i];

		const float distance = y < line.top ? line.top - y : (y > line.bottom ? y - line.bottom : 0.0f);

		if (i == 0 || distance < best) {
			best = distance;
			index = i;
		}
	}

	const Line &line = lines[index];

	if (line.cells.empty())
		return line.start;

	const Cell *nearest = nullptr;
	float nearestDistance = 0.0f;

	for (const Cell &cell : line.cells) {
		const float distance = x < cell.x0 ? cell.x0 - x : (x > cell.x1 ? x - cell.x1 : 0.0f);

		if (!nearest || distance < nearestDistance) {
			nearest = &cell;
			nearestDistance = distance;
		}
	}

	const bool leftHalf = x < (nearest->x0 + nearest->x1) * 0.5f;

	// The left half of a left-to-right cell is its start; of a right-to-left
	// one, its end.
	return leftHalf != nearest->rtl ? nearest->start : nearest->end;
}

uint32_t EditMap::vertical(uint32_t offset, int delta) const
{
	if (lines.empty())
		return offset;

	const size_t current = lineOf(offset);
	const long target = long(current) + delta;

	if (target < 0)
		return lines.front().start;

	if (target >= long(lines.size()))
		return lines.back().end;

	const Line &line = lines[size_t(target)];
	const Rect at = caret(offset);

	return hitTest(at.x, (line.top + line.bottom) * 0.5f);
}

std::vector<EditMap::Rect> EditMap::selection(uint32_t start, uint32_t end) const
{
	std::vector<Rect> out;

	if (end <= start)
		return out;

	for (const Line &line : lines) {
		const float height = line.bottom - line.top;

		bool open = false;
		Rect run;

		auto close = [&]() {
			if (open)
				out.push_back(run);

			open = false;
		};

		for (const Cell &cell : line.cells) {
			if (cell.end <= start || cell.start >= end) {
				close();
				continue;
			}

			if (open && std::fabs(cell.x0 - (run.x + run.width)) < 0.5f) {
				run.width = cell.x1 - run.x;
				continue;
			}

			close();

			run = Rect{cell.x0, line.top, cell.x1 - cell.x0, height};
			open = true;
		}

		close();

		// A selected line break shows as a sliver, so an empty line inside a
		// selection is visibly part of it.
		if (line.cells.empty() && line.start >= start && line.start < end)
			out.push_back(
				Rect{line.rtl ? line.x1 - height * 0.25f : line.x0, line.top, height * 0.25f, height});
	}

	return out;
}

std::vector<EditMap::Rect> EditMap::tokens() const
{
	std::vector<Rect> out;

	for (const Line &line : lines)
		for (const Cell &cell : line.cells)
			if (cell.token)
				out.push_back(Rect{cell.x0, line.top, cell.x1 - cell.x0, line.bottom - line.top});

	return out;
}

bool EditMap::tokenAt(uint32_t offset, uint32_t &start, uint32_t &end) const
{
	for (const Line &line : lines) {
		for (const Cell &cell : line.cells) {
			if (cell.token && offset > cell.start && offset < cell.end) {
				start = cell.start;
				end = cell.end;
				return true;
			}
		}
	}

	return false;
}

} // namespace slugged
