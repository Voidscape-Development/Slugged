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

#include "harness.hpp"

#include "core/document.hpp"
#include "core/tokens.hpp"
#include "text/edit_map.hpp"
#include "text/font_enum.hpp"
#include "text/font_manager.hpp"
#include "text/layout.hpp"
#include "text/line_break.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace slugged;

// These tests need real fonts. They use whatever the platform resolves its
// default family to, and skip themselves on a machine with no fonts at all.

namespace {

bool haveFonts()
{
	FontFileRef ref;

	if (fontenum::match(fontenum::defaultFamily(), 400, false, ref))
		return true;

	static bool warned = false;

	if (!warned) {
		std::fprintf(stderr, "  (no default font installed; text tests skipped)\n");
		warned = true;
	}

	return false;
}

LayoutResult layOut(Document doc, float width = 0.0f)
{
	auto guard = FontManager::instance().lock();

	return Layout::run(doc, width, 0.0f);
}

Document docWith(const std::string &text)
{
	Document doc;
	doc.base.sizePx = 20.0f;
	doc.setPlainText(text);
	return doc;
}

std::vector<std::vector<uint32_t>> offsetsByLine(const LayoutResult &layout)
{
	std::vector<std::vector<uint32_t>> lines(layout.lines.size());

	for (const PositionedGlyph &g : layout.glyphs)
		lines[g.lineIndex].push_back(g.textOffset);

	return lines;
}

} // namespace

TEST(line_breaks_spaces_dashes_and_ideographs)
{
	const std::vector<bool> latin = lineBreakOpportunities("ab cd-ef");

	CHECK(latin[3]);  // after the space
	CHECK(!latin[2]); // not before it
	CHECK(latin[6]);  // after the hyphen between letters

	// 日本語。 -- between ideographs, but never before the full stop.
	const std::string cjk = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x80\x82";
	const std::vector<bool> ideo = lineBreakOpportunities(cjk);

	CHECK(ideo[3]);
	CHECK(ideo[6]);
	CHECK(!ideo[9]);
}

TEST(layout_basic_line)
{
	if (!haveFonts())
		return;

	const LayoutResult layout = layOut(docWith("Hello"));

	CHECK_EQ(layout.glyphs.size(), size_t(5));
	CHECK_EQ(layout.lines.size(), size_t(1));
	CHECK(layout.width > 20.0f);

	for (size_t i = 1; i < layout.glyphs.size(); i++)
		CHECK(layout.glyphs[i].x > layout.glyphs[i - 1].x);
}

TEST(layout_word_wrap)
{
	if (!haveFonts())
		return;

	const LayoutResult single = layOut(docWith("aaaa"));
	const float word = single.width;

	const LayoutResult layout = layOut(docWith("aaaa bbbb cccc"), word * 1.5f);

	CHECK_EQ(layout.lines.size(), size_t(3));

	// Every wrapped line starts flush at the left edge: the breaking space
	// went away rather than becoming leading whitespace.
	for (const PositionedGlyph &g : layout.glyphs)
		if (g.ordinal == 4 || g.ordinal == 8)
			CHECK(g.x < 3.0f);
}

TEST(layout_wraps_cjk_without_spaces)
{
	if (!haveFonts())
		return;

	std::string text;

	for (int i = 0; i < 12; i++)
		text += "\xE6\x97\xA5"; // 日

	const LayoutResult layout = layOut(docWith(text), 80.0f);

	CHECK(layout.lines.size() > 1);
}

TEST(layout_mixed_direction_breaks_in_logical_order)
{
	if (!haveFonts())
		return;

	// English with a Hebrew phrase in the middle, wrapped narrow enough to need
	// several lines. Every line must hold text that comes *after* the previous
	// line's text: breaking in visual order would scramble that.
	const std::string text =
		"one two \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D \xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D three four five";

	const LayoutResult layout = layOut(docWith(text), 90.0f);
	const auto lines = offsetsByLine(layout);

	CHECK(lines.size() > 2);

	for (size_t i = 1; i < lines.size(); i++) {
		if (lines[i - 1].empty() || lines[i].empty())
			continue;

		CHECK(*std::max_element(lines[i - 1].begin(), lines[i - 1].end()) <
		      *std::min_element(lines[i].begin(), lines[i].end()));
	}
}

TEST(layout_rtl_paragraph_starts_on_the_right)
{
	if (!haveFonts())
		return;

	Document doc = docWith("\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D\nxxxxxxxxxxxxxxxxxxxxxxxx");

	const LayoutResult layout = layOut(doc);

	float minX = 1e9f;

	for (const PositionedGlyph &g : layout.glyphs)
		if (g.lineIndex == 0)
			minX = std::min(minX, g.x);

	// Start alignment puts the short Hebrew line against the right edge.
	CHECK(minX > layout.width * 0.5f);
}

TEST(layout_alignment_in_auto_size)
{
	if (!haveFonts())
		return;

	Document doc = docWith("a\nlonger line");

	ParagraphOverride centre;
	centre.align = HAlign::Center;
	doc.applyBaseParagraph(centre);

	const LayoutResult layout = layOut(doc);

	CHECK(layout.glyphs.front().x > 5.0f);
}

TEST(layout_text_offsets_follow_the_document)
{
	if (!haveFonts())
		return;

	const LayoutResult layout = layOut(docWith("ab\ncd"));

	CHECK_EQ(layout.glyphs.size(), size_t(4));
	CHECK_EQ(layout.glyphs[2].textOffset, uint32_t(3));
	CHECK_EQ(layout.glyphs[3].textOffset, uint32_t(4));
}

TEST(font_instances_are_shared_and_stable)
{
	if (!haveFonts())
		return;

	FontManager &fonts = FontManager::instance();
	auto guard = fonts.lock();

	FontSpec spec;
	FontInstance *a = fonts.acquire(spec);
	FontInstance *b = fonts.acquire(spec);

	CHECK(a != nullptr);
	CHECK(a == b);

	FontSpec missing;
	missing.family = "Definitely Not A Real Font Family";

	// A missing family falls back to the default rather than failing.
	CHECK(fonts.acquire(missing) != nullptr);
}

// ---- edit map ----------------------------------------------------------------

namespace {

EditMap mapOf(const Document &written, const TokenContext &tokens, float width = 0.0f)
{
	Document expanded = written;
	std::vector<TokenSpan> spans;

	expanded.expandTokens(tokens, spans);

	auto guard = FontManager::instance().lock();

	return EditMap::build(Layout::run(expanded, width, 0.0f), spans, 0.0f, 0.0f, uint32_t(written.size()));
}

} // namespace

TEST(edit_map_caret_moves_through_plain_text)
{
	if (!haveFonts())
		return;

	const EditMap map = mapOf(docWith("abc"), TokenContext());

	CHECK_EQ(map.lines.size(), size_t(1));

	// Each caret stop is further right than the last.
	CHECK(map.caret(0).x < map.caret(1).x);
	CHECK(map.caret(1).x < map.caret(2).x);
	CHECK(map.caret(2).x < map.caret(3).x);
	CHECK(map.caret(0).height > 0.0f);
}

TEST(edit_map_hit_test_round_trips_a_caret)
{
	if (!haveFonts())
		return;

	const EditMap map = mapOf(docWith("hello world"), TokenContext());

	for (uint32_t offset = 0; offset <= 11; offset++) {
		const EditMap::Rect caret = map.caret(offset);

		CHECK_EQ(map.hitTest(caret.x, caret.y + caret.height * 0.5f), offset);
	}

	// Far outside the text resolves to its ends.
	CHECK_EQ(map.hitTest(-1000.0f, 0.0f), uint32_t(0));
	CHECK_EQ(map.hitTest(100000.0f, 0.0f), uint32_t(11));
}

TEST(edit_map_lines_and_empty_paragraphs)
{
	if (!haveFonts())
		return;

	const EditMap map = mapOf(docWith("ab\n\ncd"), TokenContext());

	CHECK_EQ(map.lines.size(), size_t(3));
	CHECK_EQ(map.lines[1].start, uint32_t(3));
	CHECK_EQ(map.lines[2].start, uint32_t(4));

	// The empty line has a caret of its own, on its own row.
	CHECK(map.caret(3).y > map.caret(0).y);
	CHECK(map.caret(4).y > map.caret(3).y);

	// A click on the empty line lands in it.
	CHECK_EQ(map.hitTest(50.0f, map.caret(3).y + 1.0f), uint32_t(3));

	// Up and down keep the column.
	CHECK_EQ(map.vertical(5, -2), uint32_t(1));
	CHECK_EQ(map.vertical(1, 1), uint32_t(3));

	// A selection across the break covers all three lines.
	CHECK_EQ(map.selection(0, 6).size(), size_t(3));
	CHECK(map.selection(2, 2).empty());
}

TEST(edit_map_treats_a_token_as_one_unit)
{
	if (!haveFonts())
		return;

	TokenContext tokens;
	tokens.set("n", "12345");

	// "a{n}b" shows as "a12345b".
	const EditMap map = mapOf(docWith("a{n}b"), tokens);

	CHECK_EQ(map.textSize, uint32_t(5));
	CHECK_EQ(map.tokens().size(), size_t(1));

	// Offsets are in the text as written: 1 is before the token, 4 after it.
	const float before = map.caret(1).x;
	const float after = map.caret(4).x;

	CHECK(before < after);
	CHECK(map.caret(4).x < map.caret(5).x);

	// The token is as wide as what it shows.
	CHECK(std::abs(map.tokens()[0].width - (after - before)) < 0.01f);

	// A click inside the value lands on whichever end is nearer.
	const float y = map.caret(0).y + 1.0f;

	CHECK_EQ(map.hitTest(before + (after - before) * 0.2f, y), uint32_t(1));
	CHECK_EQ(map.hitTest(before + (after - before) * 0.8f, y), uint32_t(4));

	uint32_t start = 0;
	uint32_t end = 0;

	CHECK(map.tokenAt(2, start, end));
	CHECK_EQ(start, uint32_t(1));
	CHECK_EQ(end, uint32_t(4));
	CHECK(!map.tokenAt(1, start, end));

	// Selecting the token selects all of what it shows.
	const std::vector<EditMap::Rect> selected = map.selection(1, 4);

	CHECK_EQ(selected.size(), size_t(1));
	CHECK(std::abs(selected[0].width - (after - before)) < 0.01f);
}

TEST(edit_map_follows_wrapped_lines)
{
	if (!haveFonts())
		return;

	Document doc = docWith("aaaa bbbb cccc");
	doc.wrap = WrapMode::Word;

	const float oneWord = layOut(docWith("aaaa b")).width;
	const EditMap map = mapOf(doc, TokenContext(), oneWord);

	CHECK(map.lines.size() >= size_t(2));
	CHECK_EQ(map.lines[0].start, uint32_t(0));
	CHECK_EQ(map.lines[1].start, uint32_t(5));

	// The start of the second line is on the second row, at its left.
	CHECK(map.caret(5).y > map.caret(4).y);
	CHECK(map.caret(5).x <= map.caret(6).x);
}
