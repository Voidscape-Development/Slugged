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

#include "shaper.hpp"
#include "font_manager.hpp"

#include <algorithm>
#include <cstring>

#include <hb.h>
#include <SheenBidi/SheenBidi.h>

#include <ft2build.h>
#include FT_FREETYPE_H

namespace slugged {

uint32_t utf8Next(const std::string &text, size_t &pos)
{
	if (pos >= text.size())
		return 0;

	const unsigned char c = uint8_t(text[pos]);

	auto cont = [&](size_t i) -> bool {
		return pos + i < text.size() && (uint8_t(text[pos + i]) & 0xC0) == 0x80;
	};

	if (c < 0x80) {
		pos += 1;
		return c;
	}

	if ((c & 0xE0) == 0xC0 && cont(1)) {
		const uint32_t v = (uint32_t(c & 0x1F) << 6) | uint32_t(uint8_t(text[pos + 1]) & 0x3F);
		pos += 2;
		return v < 0x80 ? 0xFFFD : v;
	}

	if ((c & 0xF0) == 0xE0 && cont(1) && cont(2)) {
		const uint32_t v = (uint32_t(c & 0x0F) << 12) | (uint32_t(uint8_t(text[pos + 1]) & 0x3F) << 6) |
				   uint32_t(uint8_t(text[pos + 2]) & 0x3F);
		pos += 3;
		return v < 0x800 ? 0xFFFD : v;
	}

	if ((c & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
		const uint32_t v = (uint32_t(c & 0x07) << 18) | (uint32_t(uint8_t(text[pos + 1]) & 0x3F) << 12) |
				   (uint32_t(uint8_t(text[pos + 2]) & 0x3F) << 6) |
				   uint32_t(uint8_t(text[pos + 3]) & 0x3F);
		pos += 4;
		return (v < 0x10000 || v > 0x10FFFF) ? 0xFFFD : v;
	}

	pos += 1;

	return 0xFFFD;
}

size_t utf8Length(const std::string &text)
{
	size_t pos = 0;
	size_t count = 0;

	while (pos < text.size()) {
		utf8Next(text, pos);
		count++;
	}

	return count;
}

namespace {

// One homogeneous piece of text: same run, same direction, same script, same
// face. This is the unit HarfBuzz can actually shape in a single call.
struct Item {
	size_t start = 0; // byte offset into the flattened paragraph text
	size_t end = 0;
	uint32_t runIndex = 0;
	FontFace *face = nullptr;
	hb_script_t script = HB_SCRIPT_COMMON;
	bool rtl = false;
	SBLevel level = 0;
};

bool isWhitespace(uint32_t cp)
{
	return cp == ' ' || cp == '\t' || cp == 0x00A0 || cp == 0x3000 || cp == 0x200B;
}

hb_script_t scriptOf(uint32_t cp)
{
	return hb_unicode_script(hb_unicode_funcs_get_default(), cp);
}

// Scripts that carry no identity of their own (spaces, punctuation, digits)
// should join whatever surrounds them rather than splitting an item.
bool inheritsScript(hb_script_t s)
{
	return s == HB_SCRIPT_COMMON || s == HB_SCRIPT_INHERITED || s == HB_SCRIPT_UNKNOWN;
}

hb_direction_t hbDirection(bool rtl)
{
	return rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR;
}

// Picks the face that should render `cp` for `style`: the requested one when it
// has the glyph, otherwise the platform's fallback for that codepoint.
FontFace *faceFor(const Style &style, uint32_t cp, FontFace *preferred)
{
	if (!preferred)
		return nullptr;

	// Whitespace never justifies a font switch; doing so splits items for no
	// visual gain and breaks shaping across the space.
	if (isWhitespace(cp) || cp == '\n')
		return preferred;

	if (FT_Get_Char_Index(preferred->ft, cp))
		return preferred;

	FontFace *fallback = FontManager::instance().fallbackFor(style.font, cp);

	return fallback ? fallback : preferred;
}

} // namespace

std::vector<ShapedGlyph> Shaper::shape(const std::vector<ShapeRun> &runs, Direction baseDirection)
{
	std::vector<ShapedGlyph> out;

	// ---- flatten to one paragraph, remembering which run each byte came from
	std::string text;
	std::vector<uint32_t> byteRun; // run index per byte

	for (const ShapeRun &r : runs) {
		if (!r.style)
			continue;

		text += r.text;
		byteRun.insert(byteRun.end(), r.text.size(), r.runIndex);
	}

	if (text.empty())
		return out;

	// ---- resolve the bidi levels
	//
	// SheenBidi reports one level per code *unit*, and the sequence is UTF-8
	// here, so levels can be indexed directly by byte offset -- no parallel
	// UTF-32 buffer and no offset mapping to keep in sync.
	SBLevel baseLevel = SBLevelDefaultLTR;

	if (baseDirection == Direction::LTR)
		baseLevel = 0;
	else if (baseDirection == Direction::RTL)
		baseLevel = 1;

	std::vector<SBLevel> levels(text.size(), 0);

	SBCodepointSequence sequence;

	sequence.stringEncoding = SBStringEncodingUTF8;
	sequence.stringBuffer = const_cast<char *>(text.data());
	sequence.stringLength = text.size();

	if (SBAlgorithmRef algorithm = SBAlgorithmCreate(&sequence)) {
		SBParagraphRef paragraph = SBAlgorithmCreateParagraph(algorithm, 0, text.size(), baseLevel);

		if (paragraph) {
			const SBLevel *resolved = SBParagraphGetLevelsPtr(paragraph);
			const SBUInteger length = SBParagraphGetLength(paragraph);

			if (resolved)
				for (SBUInteger i = 0; i < length && i < levels.size(); i++)
					levels[i] = resolved[i];

			SBParagraphRelease(paragraph);
		}

		SBAlgorithmRelease(algorithm);
	}

	// ---- itemize: split wherever run, level, script or face changes
	std::vector<Item> items;

	FontFace *runFace = nullptr;
	uint32_t lastRun = UINT32_MAX;

	size_t byte = 0;

	while (byte < text.size()) {
		size_t next = byte;
		const uint32_t cp = utf8Next(text, next);

		const uint32_t runIndex = byte < byteRun.size() ? byteRun[byte] : 0;
		const SBLevel level = byte < levels.size() ? levels[byte] : 0;

		const ShapeRun *run = nullptr;

		for (const ShapeRun &r : runs)
			if (r.runIndex == runIndex)
				run = &r;

		if (!run || !run->style) {
			byte = next;
			continue;
		}

		if (runIndex != lastRun) {
			runFace = FontManager::instance().acquire(run->style->font);
			lastRun = runIndex;
		}

		FontFace *face = faceFor(*run->style, cp, runFace);
		const hb_script_t script = scriptOf(cp);
		const bool rtl = (level & 1) != 0;

		const size_t byteEnd = next;

		bool startNew = items.empty();

		if (!startNew) {
			Item &last = items.back();

			startNew = last.runIndex != runIndex || last.face != face || last.rtl != rtl ||
				   last.level != level ||
				   (!inheritsScript(script) && !inheritsScript(last.script) && last.script != script);
		}

		if (startNew) {
			Item it;

			it.start = byte;
			it.end = byteEnd;
			it.runIndex = runIndex;
			it.face = face;
			it.script = script;
			it.rtl = rtl;
			it.level = level;

			items.push_back(it);
		} else {
			Item &last = items.back();

			last.end = byteEnd;

			// An item that began on inherited characters adopts the first
			// real script it meets.
			if (inheritsScript(last.script) && !inheritsScript(script))
				last.script = script;
		}

		byte = next;
	}

	if (items.empty())
		return out;

	// ---- reorder items into visual order
	//
	// Reordering is applied to whole items rather than to individual
	// characters. That is equivalent here because itemization already split on
	// every level change, so each item carries one uniform level -- and it keeps
	// the mapping back to shaped glyphs trivial. The rule is the standard one
	// from the Unicode Bidirectional Algorithm (L2): reverse each maximal span
	// at or above every level, from the highest level down to the lowest odd
	// level.
	std::vector<size_t> order(items.size());

	for (size_t i = 0; i < items.size(); i++)
		order[i] = i;

	SBLevel highest = 0;
	SBLevel lowestOdd = SBLevel(SBLevelMax + 1);

	for (const Item &it : items) {
		highest = std::max(highest, it.level);

		if (it.level % 2)
			lowestOdd = std::min(lowestOdd, it.level);
	}

	for (SBLevel level = highest; level >= lowestOdd && level > 0; level--) {
		size_t i = 0;

		while (i < order.size()) {
			if (items[order[i]].level < level) {
				i++;
				continue;
			}

			size_t j = i;

			while (j < order.size() && items[order[j]].level >= level)
				j++;

			std::reverse(order.begin() + long(i), order.begin() + long(j));

			i = j;
		}
	}

	// ---- shape each item
	hb_buffer_t *buffer = hb_buffer_create();

	if (!buffer)
		return out;

	for (size_t oi = 0; oi < order.size(); oi++) {
		const Item &item = items[order[oi]];

		if (!item.face || !item.face->hb)
			continue;

		const ShapeRun *run = nullptr;

		for (const ShapeRun &r : runs)
			if (r.runIndex == item.runIndex)
				run = &r;

		if (!run || !run->style)
			continue;

		hb_buffer_clear_contents(buffer);

		// Feed the whole paragraph with an explicit item range so HarfBuzz can
		// see the surrounding context; joining scripts need the neighbouring
		// characters to choose initial/medial/final forms correctly.
		hb_buffer_add_utf8(buffer, text.c_str(), int(text.size()), unsigned(item.start),
				   int(item.end - item.start));

		hb_buffer_set_direction(buffer, hbDirection(item.rtl));
		hb_buffer_set_script(buffer, item.script);

		if (!run->style->language.empty())
			hb_buffer_set_language(
				buffer, hb_language_from_string(run->style->language.c_str(),
								int(run->style->language.size())));
		else
			hb_buffer_set_language(buffer, hb_language_get_default());

		hb_buffer_set_cluster_level(buffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);

		hb_shape(item.face->hb, buffer, nullptr, 0);

		unsigned count = 0;
		const hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buffer, &count);
		const hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buffer, &count);

		if (!info || !pos)
			continue;

		// Normalise to em space using HarfBuzz's own scale, so everything
		// downstream is resolution-independent.
		const float invX = item.face->hbScaleX != 0.0f ? 1.0f / item.face->hbScaleX : 0.0f;
		const float invY = item.face->hbScaleY != 0.0f ? 1.0f / item.face->hbScaleY : 0.0f;

		for (unsigned g = 0; g < count; g++) {
			ShapedGlyph sg;

			sg.face = item.face;
			sg.glyphIndex = info[g].codepoint; // post-shaping this is a glyph id
			sg.advance = float(pos[g].x_advance) * invX;
			sg.offsetX = float(pos[g].x_offset) * invX;
			sg.offsetY = float(pos[g].y_offset) * invY;
			sg.cluster = info[g].cluster;
			sg.runIndex = item.runIndex;
			sg.rtl = item.rtl;

			// Whitespace is identified from the source text rather than from
			// the glyph, because a space's glyph id is font-specific.
			size_t byte = size_t(info[g].cluster);

			if (byte < text.size()) {
				size_t tmp = byte;
				sg.whitespace = isWhitespace(utf8Next(text, tmp));
			}

			out.push_back(sg);
		}
	}

	hb_buffer_destroy(buffer);

	return out;
}

} // namespace slugged
