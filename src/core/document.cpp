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

#include <algorithm>

namespace slugged {

namespace {

struct Position {
	size_t block = 0;
	size_t offset = 0; // byte offset within the block
};

// Copies the part of `runs` covering bytes [from, to) of the block.
std::vector<Run> sliceRuns(const std::vector<Run> &runs, size_t from, size_t to)
{
	std::vector<Run> out;
	size_t pos = 0;

	for (const Run &run : runs) {
		const size_t runStart = pos;
		const size_t runEnd = pos + run.text.size();

		pos = runEnd;

		const size_t a = std::max(from, runStart);
		const size_t b = std::min(to, runEnd);

		if (a < b)
			out.push_back(Run{run.text.substr(a - runStart, b - a), run.style});
		else if (run.text.empty() && runStart >= from && runStart <= to)
			out.push_back(run);
	}

	return out;
}

// The overrides of the run holding byte `offset` of a block, preferring the
// character before the offset.
StyleOverride overridesAt(const Block &block, size_t offset)
{
	size_t pos = 0;
	const Run *before = nullptr;
	const Run *after = nullptr;

	for (const Run &run : block.runs) {
		const size_t runEnd = pos + run.text.size();

		if (offset > pos && offset <= runEnd)
			before = &run;

		if (!after && offset >= pos && offset < runEnd)
			after = &run;

		pos = runEnd;
	}

	if (before)
		return before->style;

	if (after)
		return after->style;

	return block.runs.empty() ? StyleOverride{} : block.runs.front().style;
}

void normalizeBlock(Block &block)
{
	std::vector<Run> out;

	for (Run &run : block.runs) {
		if (run.text.empty())
			continue;

		if (!out.empty() && out.back().style == run.style)
			out.back().text += run.text;
		else
			out.push_back(std::move(run));
	}

	// An empty paragraph keeps one empty run, so the style the cursor was in when
	// it was created carries on when typing into it.
	if (out.empty() && !block.runs.empty())
		out.push_back(Run{std::string(), block.runs.front().style});

	block.runs = std::move(out);
}

} // namespace

std::string Block::plainText() const
{
	std::string out;

	for (const Run &r : runs)
		out += r.text;

	return out;
}

size_t Block::size() const
{
	size_t n = 0;

	for (const Run &r : runs)
		n += r.text.size();

	return n;
}

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

size_t utf8Floor(const std::string &text, size_t offset)
{
	offset = std::min(offset, text.size());

	while (offset > 0 && offset < text.size() && (uint8_t(text[offset]) & 0xC0) == 0x80)
		offset--;

	return offset;
}

std::string Document::plainText() const
{
	std::string out;

	for (size_t i = 0; i < blocks.size(); i++) {
		if (i)
			out += '\n';

		out += blocks[i].plainText();
	}

	return out;
}

size_t Document::size() const
{
	size_t n = blocks.empty() ? 0 : blocks.size() - 1;

	for (const Block &b : blocks)
		n += b.size();

	return n;
}

bool Document::empty() const
{
	for (const Block &b : blocks)
		for (const Run &r : b.runs)
			if (!r.text.empty())
				return false;

	return true;
}

static Position locate(const std::vector<Block> &blocks, size_t offset)
{
	size_t pos = 0;

	for (size_t i = 0; i < blocks.size(); i++) {
		const size_t size = blocks[i].size();

		if (offset <= pos + size)
			return {i, offset - pos};

		pos += size + 1;
	}

	if (blocks.empty())
		return {0, 0};

	return {blocks.size() - 1, blocks.back().size()};
}

void Document::replaceText(size_t start, size_t end, const std::string &text)
{
	if (blocks.empty())
		blocks.push_back(Block{});

	if (end < start)
		std::swap(start, end);

	const Position a = locate(blocks, start);
	const Position b = locate(blocks, end);

	const Block &first = blocks[a.block];
	const Block &last = blocks[b.block];

	const StyleOverride inserted = overridesAt(first, a.offset);

	std::vector<Run> head = sliceRuns(first.runs, 0, a.offset);
	std::vector<Run> tail = sliceRuns(last.runs, b.offset, last.size());

	const ParagraphOverride firstParagraph = first.paragraph;
	const ParagraphOverride lastParagraph = last.paragraph;

	const std::vector<std::string> pieces = splitLines(text);

	std::vector<Block> replacement;

	for (size_t i = 0; i < pieces.size(); i++) {
		Block block;

		block.paragraph = i + 1 == pieces.size() && pieces.size() > 1 ? lastParagraph : firstParagraph;

		if (i == 0)
			block.runs = head;

		block.runs.push_back(Run{pieces[i], inserted});

		if (i + 1 == pieces.size())
			block.runs.insert(block.runs.end(), tail.begin(), tail.end());

		normalizeBlock(block);
		replacement.push_back(std::move(block));
	}

	blocks.erase(blocks.begin() + long(a.block), blocks.begin() + long(b.block) + 1);
	blocks.insert(blocks.begin() + long(a.block), replacement.begin(), replacement.end());
}

void Document::setPlainText(const std::string &text)
{
	const std::string current = plainText();

	if (current == text && !blocks.empty())
		return;

	// Common prefix and suffix, snapped to whole UTF-8 sequences so the edit
	// never splits a character.
	size_t prefix = 0;
	const size_t limit = std::min(current.size(), text.size());

	while (prefix < limit && current[prefix] == text[prefix])
		prefix++;

	prefix = utf8Floor(current, prefix);

	size_t suffix = 0;

	while (suffix < limit - prefix && current[current.size() - 1 - suffix] == text[text.size() - 1 - suffix])
		suffix++;

	// The suffix must also begin on a character boundary in both strings.
	while (suffix > 0 && (uint8_t(current[current.size() - suffix]) & 0xC0) == 0x80)
		suffix--;

	replaceText(prefix, current.size() - suffix, text.substr(prefix, text.size() - suffix - prefix));
}

bool Document::expandTokens(const std::function<std::string(const std::string &)> &expand)
{
	bool changed = false;

	for (Block &block : blocks) {
		for (Run &run : block.runs) {
			if (run.text.find('{') == std::string::npos)
				continue;

			std::string expanded = expand(run.text);

			if (expanded == run.text)
				continue;

			run.text = std::move(expanded);
			changed = true;
		}
	}

	return changed;
}

void Document::applyStyle(size_t start, size_t end, const StyleOverride &delta)
{
	if (end < start)
		std::swap(start, end);

	size_t pos = 0;

	for (Block &block : blocks) {
		const size_t blockStart = pos;
		const size_t blockEnd = pos + block.size();

		pos = blockEnd + 1;

		const size_t from = std::max(start, blockStart) - blockStart;
		const size_t to = std::min(end, blockEnd);

		// An empty paragraph inside the selection still takes the style, so
		// text typed into it later looks like its neighbours.
		if (block.size() == 0 && start <= blockStart && end > blockStart) {
			if (block.runs.empty())
				block.runs.push_back(Run{});

			block.runs.front().style.merge(delta);
			continue;
		}

		if (to <= blockStart || from >= block.size() || to - blockStart <= from)
			continue;

		std::vector<Run> rebuilt = sliceRuns(block.runs, 0, from);
		std::vector<Run> middle = sliceRuns(block.runs, from, to - blockStart);

		for (Run &run : middle)
			run.style.merge(delta);

		rebuilt.insert(rebuilt.end(), middle.begin(), middle.end());

		const std::vector<Run> rest = sliceRuns(block.runs, to - blockStart, block.size());
		rebuilt.insert(rebuilt.end(), rest.begin(), rest.end());

		block.runs = std::move(rebuilt);
		normalizeBlock(block);
	}
}

void Document::applyBaseStyle(const StyleOverride &delta)
{
	delta.applyTo(base);

	for (Block &block : blocks) {
		for (Run &run : block.runs)
			run.style.clear(delta);

		normalizeBlock(block);
	}
}

void Document::clearStyle(size_t start, size_t end)
{
	if (end < start)
		std::swap(start, end);

	size_t pos = 0;

	for (Block &block : blocks) {
		const size_t blockStart = pos;
		const size_t blockEnd = pos + block.size();

		pos = blockEnd + 1;

		if (end <= blockStart && !(block.size() == 0 && start == blockStart))
			continue;

		if (start > blockEnd)
			continue;

		const size_t from = std::max(start, blockStart) - blockStart;
		const size_t to = std::min(end, blockEnd) - blockStart;

		std::vector<Run> rebuilt = sliceRuns(block.runs, 0, from);

		for (Run run : sliceRuns(block.runs, from, to)) {
			run.style = StyleOverride{};
			rebuilt.push_back(std::move(run));
		}

		const std::vector<Run> rest = sliceRuns(block.runs, to, block.size());
		rebuilt.insert(rebuilt.end(), rest.begin(), rest.end());

		block.runs = std::move(rebuilt);
		normalizeBlock(block);
	}
}

Style Document::styleAt(size_t offset) const
{
	if (blocks.empty())
		return base;

	const Position p = locate(blocks, offset);

	return overridesAt(blocks[p.block], p.offset).resolve(base);
}

void Document::applyParagraph(size_t start, size_t end, const ParagraphOverride &delta)
{
	if (end < start)
		std::swap(start, end);

	const Position a = locate(blocks, start);
	const Position b = locate(blocks, end);

	for (size_t i = a.block; i <= b.block && i < blocks.size(); i++)
		blocks[i].paragraph.merge(delta);
}

void Document::applyBaseParagraph(const ParagraphOverride &delta)
{
	delta.applyTo(paragraph);

	for (Block &block : blocks)
		block.paragraph.clear(delta);
}

ParagraphStyle Document::paragraphAt(size_t offset) const
{
	if (blocks.empty())
		return paragraph;

	return resolve(blocks[locate(blocks, offset).block]);
}

void Document::normalize()
{
	for (Block &block : blocks)
		normalizeBlock(block);
}

} // namespace slugged
