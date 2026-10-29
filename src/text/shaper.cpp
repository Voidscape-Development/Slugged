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

#include <hb.h>
#include <SheenBidi/SheenBidi.h>

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

void reorderLine(const std::vector<uint8_t> &levels, std::vector<size_t> &order)
{
	if (levels.empty())
		return;

	uint8_t highest = 0;
	uint8_t lowestOdd = 0xFF;

	for (uint8_t level : levels) {
		highest = std::max(highest, level);

		if (level & 1)
			lowestOdd = std::min(lowestOdd, level);
	}

	// From the highest level down to the lowest odd one, reverse every maximal
	// span at or above that level.
	for (int level = highest; level >= int(lowestOdd) && level > 0; level--) {
		size_t i = 0;

		while (i < order.size()) {
			if (levels[order[i]] < level) {
				i++;
				continue;
			}

			size_t j = i;

			while (j < order.size() && levels[order[j]] >= level)
				j++;

			std::reverse(order.begin() + long(i), order.begin() + long(j));

			i = j;
		}
	}
}

namespace {

// One homogeneous piece of text: same run, level, script and font. This is the
// unit HarfBuzz can shape in one call.
struct Item {
	size_t start = 0;
	size_t end = 0;
	size_t run = 0; // position in the runs vector
	FontInstance *font = nullptr;
	hb_script_t script = HB_SCRIPT_COMMON;
	SBLevel level = 0;
};

bool isWhitespace(uint32_t cp)
{
	return cp == ' ' || cp == '\t' || cp == 0x00A0 || cp == 0x3000 || cp == 0x200B || (cp >= 0x2000 && cp <= 0x200A);
}

// Characters that only make sense attached to their neighbour: joiners,
// variation selectors, emoji modifiers and tags. They stay in the font of what
// precedes them, so an emoji sequence shapes as one unit in the emoji font.
bool isAttaching(uint32_t cp)
{
	return cp == 0x200D || cp == 0x200C || (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0x1F3FB && cp <= 0x1F3FF) ||
	       (cp >= 0xE0020 && cp <= 0xE007F) || cp == 0x20E3;
}

bool inheritsScript(hb_script_t s)
{
	return s == HB_SCRIPT_COMMON || s == HB_SCRIPT_INHERITED || s == HB_SCRIPT_UNKNOWN;
}

bool covers(const FontInstance *font, uint32_t cp)
{
	return font && FT_Get_Char_Index(font->file->ft, cp) != 0;
}

std::vector<hb_feature_t> featuresFor(const Style &style)
{
	std::vector<hb_feature_t> out;

	auto add = [&out](const std::string &text) {
		hb_feature_t feature;

		if (hb_feature_from_string(text.c_str(), int(text.size()), &feature))
			out.push_back(feature);
	};

	if (style.tabularDigits)
		add("tnum");

	size_t start = 0;

	while (start < style.features.size()) {
		size_t comma = style.features.find(',', start);

		if (comma == std::string::npos)
			comma = style.features.size();

		std::string part = style.features.substr(start, comma - start);

		part.erase(0, part.find_first_not_of(" \t"));
		part.erase(part.find_last_not_of(" \t") + 1);

		if (!part.empty())
			add(part);

		start = comma + 1;
	}

	return out;
}

} // namespace

ShapedParagraph Shaper::shape(const std::vector<ShapeRun> &runs, Direction baseDirection)
{
	ShapedParagraph out;

	std::string &text = out.text;
	std::vector<uint32_t> byteRun;

	for (size_t i = 0; i < runs.size(); i++) {
		if (!runs[i].style)
			continue;

		text += runs[i].text;
		byteRun.insert(byteRun.end(), runs[i].text.size(), uint32_t(i));
	}

	if (text.empty())
		return out;

	// ---- bidi levels, one per UTF-8 code unit
	SBLevel requested = SBLevelDefaultLTR;

	if (baseDirection == Direction::LTR)
		requested = 0;
	else if (baseDirection == Direction::RTL)
		requested = 1;

	std::vector<SBLevel> levels(text.size(), requested == 1 ? 1 : 0);

	SBCodepointSequence sequence;

	sequence.stringEncoding = SBStringEncodingUTF8;
	sequence.stringBuffer = const_cast<char *>(text.data());
	sequence.stringLength = text.size();

	if (SBAlgorithmRef algorithm = SBAlgorithmCreate(&sequence)) {
		if (SBParagraphRef paragraph = SBAlgorithmCreateParagraph(algorithm, 0, text.size(), requested)) {
			const SBLevel *resolved = SBParagraphGetLevelsPtr(paragraph);
			const SBUInteger length = SBParagraphGetLength(paragraph);

			if (resolved)
				for (SBUInteger i = 0; i < length && i < levels.size(); i++)
					levels[i] = resolved[i];

			out.baseLevel = uint8_t(SBParagraphGetBaseLevel(paragraph) & 1);

			SBParagraphRelease(paragraph);
		}

		SBAlgorithmRelease(algorithm);
	}

	FontManager &fonts = FontManager::instance();
	auto guard = fonts.lock();

	std::vector<FontInstance *> runFonts(runs.size(), nullptr);

	for (size_t i = 0; i < runs.size(); i++)
		if (runs[i].style)
			runFonts[i] = fonts.acquire(runs[i].style->font);

	// ---- itemize
	std::vector<Item> items;
	size_t byte = 0;

	while (byte < text.size()) {
		size_t next = byte;
		const uint32_t cp = utf8Next(text, next);

		const size_t run = byteRun[byte];
		const SBLevel level = levels[byte];
		FontInstance *preferred = runFonts[run];

		FontInstance *font = preferred;

		// An emoji, or a character asking for emoji presentation with U+FE0F,
		// goes to a colour font even when the run's own font has a plain
		// black-and-white version of it.
		size_t peek = next;
		const bool wantsColor = FontManager::isEmoji(cp) || (next < text.size() && utf8Next(text, peek) == 0xFE0F);
		const bool preferredIsColor =
			preferred && (preferred->file->hasColorLayers || preferred->file->hasColorBitmaps);

		if (!items.empty() && items.back().run == run && items.back().level == level && isAttaching(cp)) {
			font = items.back().font;
		} else if (!isWhitespace(cp) && (!covers(preferred, cp) || (wantsColor && !preferredIsColor))) {
			if (FontInstance *fallback = fonts.fallbackFor(runs[run].style->font, cp, wantsColor))
				if (covers(fallback, cp))
					font = fallback;
		}

		const hb_script_t script = hb_unicode_script(hb_unicode_funcs_get_default(), cp);

		bool startNew = items.empty();

		if (!startNew) {
			const Item &last = items.back();

			startNew = last.run != run || last.font != font || last.level != level ||
				   (!inheritsScript(script) && !inheritsScript(last.script) && last.script != script);
		}

		if (startNew) {
			Item item;

			item.start = byte;
			item.end = next;
			item.run = run;
			item.font = font;
			item.script = script;
			item.level = level;

			items.push_back(item);
		} else {
			Item &last = items.back();

			last.end = next;

			if (inheritsScript(last.script) && !inheritsScript(script))
				last.script = script;
		}

		byte = next;
	}

	// ---- shape each item, in logical order
	hb_buffer_t *buffer = hb_buffer_create();

	std::vector<ShapedGlyph> itemGlyphs;

	for (const Item &item : items) {
		if (!item.font || !item.font->hb)
			continue;

		const ShapeRun &run = runs[item.run];
		const bool rtl = (item.level & 1) != 0;

		hb_buffer_clear_contents(buffer);

		// The whole paragraph goes in with an item range, so HarfBuzz sees the
		// surrounding context that joining scripts need.
		hb_buffer_add_utf8(buffer, text.c_str(), int(text.size()), unsigned(item.start),
				   int(item.end - item.start));

		hb_buffer_set_direction(buffer, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
		hb_buffer_set_script(buffer, item.script);

		if (!run.style->language.empty())
			hb_buffer_set_language(buffer, hb_language_from_string(run.style->language.c_str(),
									       int(run.style->language.size())));
		else
			hb_buffer_set_language(buffer, hb_language_get_default());

		hb_buffer_set_cluster_level(buffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);

		const std::vector<hb_feature_t> features = featuresFor(*run.style);

		hb_shape(item.font->hb, buffer, features.data(), unsigned(features.size()));

		unsigned count = 0;
		const hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buffer, &count);
		const hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buffer, &count);

		if (!info || !pos)
			continue;

		const float inv = 1.0f / item.font->file->unitsPerEm;

		itemGlyphs.clear();

		for (unsigned g = 0; g < count; g++) {
			ShapedGlyph sg;

			sg.font = item.font;
			sg.glyphIndex = info[g].codepoint;
			sg.advance = float(pos[g].x_advance) * inv;
			sg.offsetX = float(pos[g].x_offset) * inv;
			sg.offsetY = float(pos[g].y_offset) * inv;
			sg.cluster = info[g].cluster;
			sg.runIndex = run.runIndex;
			sg.level = uint8_t(item.level);

			size_t at = sg.cluster;
			sg.whitespace = at < text.size() && isWhitespace(utf8Next(text, at));

			itemGlyphs.push_back(sg);
		}

		// HarfBuzz returns right-to-left items in visual order. Lines are
		// broken in logical order, so put them back; reorderLine() reverses
		// them again once the line is known.
		if (rtl)
			std::reverse(itemGlyphs.begin(), itemGlyphs.end());

		out.glyphs.insert(out.glyphs.end(), itemGlyphs.begin(), itemGlyphs.end());
	}

	hb_buffer_destroy(buffer);

	for (size_t i = 0; i < out.glyphs.size(); i++)
		out.glyphs[i].clusterStart = i == 0 || out.glyphs[i].cluster != out.glyphs[i - 1].cluster;

	return out;
}

} // namespace slugged
