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
#include "core/markup.hpp"
#include "core/tokens.hpp"
#include "core/variables.hpp"

using namespace slugged;

namespace {

StyleOverride red()
{
	StyleOverride o;
	o.color = Rgba{1.0f, 0.0f, 0.0f, 1.0f};
	return o;
}

Document makeDoc(const std::string &text)
{
	Document doc;
	doc.setPlainText(text);
	return doc;
}

} // namespace

// ---- document editing ------------------------------------------------------

TEST(document_plain_text_round_trip)
{
	Document doc = makeDoc("hello\nworld");

	CHECK_EQ(doc.plainText(), std::string("hello\nworld"));
	CHECK_EQ(doc.blocks.size(), size_t(2));
	CHECK_EQ(doc.size(), size_t(11));
}

TEST(document_typing_keeps_styling)
{
	Document doc = makeDoc("Live now");

	doc.applyStyle(0, 4, red());

	// Typing at the end of the styled word extends it; typing elsewhere does
	// not disturb it. This is the edit that used to flatten everything.
	doc.replaceText(4, 4, "!");
	doc.replaceText(doc.size(), doc.size(), " today");

	CHECK_EQ(doc.plainText(), std::string("Live! now today"));
	CHECK(doc.styleAt(1).fill.color == (Rgba{1.0f, 0.0f, 0.0f, 1.0f}));
	CHECK(doc.styleAt(5).fill.color == (Rgba{1.0f, 0.0f, 0.0f, 1.0f}));
	CHECK(doc.styleAt(8).fill.color == Rgba{});
}

TEST(document_set_plain_text_is_a_minimal_edit)
{
	Document doc = makeDoc("Score: 10 points");

	doc.applyStyle(0, 6, red());
	doc.setPlainText("Score: 11 points");

	CHECK_EQ(doc.plainText(), std::string("Score: 11 points"));
	CHECK(doc.styleAt(3).fill.color == (Rgba{1.0f, 0.0f, 0.0f, 1.0f}));
	CHECK(doc.styleAt(12).fill.color == Rgba{});
}

TEST(document_set_plain_text_respects_utf8_boundaries)
{
	Document doc = makeDoc("caf\xC3\xA9");

	// é (C3 A9) -> è (C3 A8): shares a lead byte, which must not be split.
	doc.setPlainText("caf\xC3\xA8");

	CHECK_EQ(doc.plainText(), std::string("caf\xC3\xA8"));

	for (const Run &run : doc.blocks[0].runs)
		CHECK(run.text.empty() || (uint8_t(run.text[0]) & 0xC0) != 0x80);
}

TEST(document_newline_splits_and_merges_paragraphs)
{
	Document doc = makeDoc("abcdef");

	ParagraphOverride centre;
	centre.align = HAlign::Center;
	doc.applyParagraph(0, 0, centre);

	doc.replaceText(3, 3, "\n");

	CHECK_EQ(doc.blocks.size(), size_t(2));
	CHECK(doc.paragraphAt(5).align == HAlign::Center);

	doc.replaceText(3, 4, "");

	CHECK_EQ(doc.blocks.size(), size_t(1));
	CHECK_EQ(doc.plainText(), std::string("abcdef"));
}

TEST(document_base_style_cascades)
{
	Document doc = makeDoc("one two");

	doc.applyStyle(4, 7, red());

	StyleOverride bigger;
	bigger.size = 100.0f;
	doc.applyBaseStyle(bigger);

	// The base change reaches every character...
	CHECK_EQ(doc.styleAt(1).sizePx, 100.0f);
	CHECK_EQ(doc.styleAt(6).sizePx, 100.0f);

	// ...without disturbing what the run set for itself.
	CHECK(doc.styleAt(6).fill.color == (Rgba{1.0f, 0.0f, 0.0f, 1.0f}));

	// A base edit of the same field wins everywhere.
	StyleOverride blue;
	blue.color = Rgba{0.0f, 0.0f, 1.0f, 1.0f};
	doc.applyBaseStyle(blue);

	CHECK(doc.styleAt(6).fill.color == (Rgba{0.0f, 0.0f, 1.0f, 1.0f}));
	CHECK_EQ(doc.blocks[0].runs.size(), size_t(1));
}

TEST(document_apply_style_only_touches_the_changed_field)
{
	Document doc = makeDoc("ab");

	StyleOverride big;
	big.size = 40.0f;
	doc.applyStyle(0, 1, big);

	StyleOverride italic;
	italic.italic = true;
	doc.applyStyle(0, 2, italic);

	CHECK_EQ(doc.styleAt(1).sizePx, 40.0f);
	CHECK_EQ(doc.styleAt(2).sizePx, 64.0f);
	CHECK(doc.styleAt(1).font.italic);
	CHECK(doc.styleAt(2).font.italic);
}

TEST(document_clear_style)
{
	Document doc = makeDoc("abc");

	doc.applyStyle(0, 3, red());
	doc.clearStyle(1, 2);

	CHECK(doc.styleAt(1).fill.color == (Rgba{1.0f, 0.0f, 0.0f, 1.0f}));
	CHECK(doc.styleAt(2).fill.color == Rgba{});
	CHECK_EQ(doc.blocks[0].runs.size(), size_t(3));
}

TEST(document_empty_paragraph_keeps_style)
{
	Document doc = makeDoc("abc");

	doc.applyStyle(0, 3, red());
	doc.replaceText(3, 3, "\n");

	// The cursor now sits in an empty paragraph; typing continues in red.
	doc.replaceText(4, 4, "x");

	CHECK(doc.styleAt(5).fill.color == (Rgba{1.0f, 0.0f, 0.0f, 1.0f}));
}

TEST(style_override_diff_round_trips)
{
	Style base;
	Style styled = base;

	styled.sizePx = 12.0f;
	styled.outline.enabled = true;

	const StyleOverride diff = StyleOverride::diff(base, styled);

	CHECK(diff.size.has_value());
	CHECK(diff.outline.has_value());
	CHECK(!diff.color.has_value());
	CHECK(diff.resolve(base) == styled);
}

TEST(enum_strings)
{
	CHECK_EQ(std::string(enumToString(Motion::Spin)), std::string("spin"));
	CHECK(enumFromString("bounce", Motion::None) == Motion::Bounce);
	CHECK(enumFromString("nonsense", Motion::Fade) == Motion::Fade);
}

// ---- markup ------------------------------------------------------------------

TEST(markup_basic_tags)
{
	const std::vector<Block> blocks = parseMarkup("Hi [b]there[/b] [color=#f00]red[/color]");

	CHECK_EQ(blocks.size(), size_t(1));
	CHECK_EQ(blocks[0].plainText(), std::string("Hi there red"));
	CHECK_EQ(blocks[0].runs.size(), size_t(4));
	CHECK(blocks[0].runs[1].style.weight == 700);
	CHECK(blocks[0].runs[3].style.color == (Rgba{1.0f, 0.0f, 0.0f, 1.0f}));
}

TEST(markup_nesting_and_lines)
{
	const std::vector<Block> blocks = parseMarkup("[size=10]a[i]b\nc[/i]d[/]e");

	CHECK_EQ(blocks.size(), size_t(2));
	CHECK_EQ(blocks[0].plainText(), std::string("ab"));
	CHECK_EQ(blocks[1].plainText(), std::string("cde"));

	// "c" is still inside both tags on the second line.
	CHECK(blocks[1].runs[0].style.italic == true);
	CHECK(blocks[1].runs[0].style.size == 10.0f);

	// "e" follows [/], which closed [size].
	CHECK(!blocks[1].runs.back().style.size.has_value());
}

TEST(markup_unknown_and_escaped_tags_stay_visible)
{
	const std::vector<Block> blocks = parseMarkup("[[b] [nope] [/i] [color=wat]x");

	CHECK_EQ(blocks[0].plainText(), std::string("[b] [nope] [/i] [color=wat]x"));
}

TEST(markup_colours)
{
	CHECK(parseColor("#fff") == (Rgba{1, 1, 1, 1}));
	CHECK(parseColor("#00000080").value().a > 0.49f);
	CHECK(parseColor("orange").has_value());
	CHECK(!parseColor("#12345").has_value());
}

// ---- tokens ------------------------------------------------------------------

TEST(tokens_unknown_tokens_stay_visible)
{
	TokenContext tokens;

	CHECK_EQ(tokens.expand("{nope} {{x}"), std::string("{nope} {x}"));
}

TEST(tokens_variables_shadow_builtins)
{
	TokenContext tokens;

	tokens.set("time", "later");

	CHECK_EQ(tokens.expand("{time}"), std::string("later"));
}

TEST(tokens_content_expansion_ignores_live_values)
{
	TokenContext tokens;

	tokens.set("name", "Ana");
	tokens.setHostVariables({{"fps", "60"}});

	CHECK_EQ(tokens.expand("{name} {fps} {time}").substr(0, 7), std::string("Ana 60 "));
	CHECK_EQ(tokens.expandContent("{name} {fps} {time}"), std::string("Ana {fps} {time}"));
}

TEST(tokens_timer_and_countdown)
{
	TokenContext tokens;

	tokens.setTimer(65.4f);

	CHECK_EQ(tokens.expand("{timer}"), std::string("1:05"));
	CHECK_EQ(tokens.expand("{countdown:5:00}"), std::string("3:55"));

	tokens.setTimer(4000.0f);

	CHECK_EQ(tokens.expand("{countdown:5:00}"), std::string("0:00"));
	CHECK_EQ(tokens.expand("{countdown:junk}"), std::string("{countdown:junk}"));
}

TEST(tokens_until)
{
	std::tm noon{};
	noon.tm_year = 2026 - 1900;
	noon.tm_mon = 0;
	noon.tm_mday = 10;
	noon.tm_hour = 12;
	noon.tm_isdst = -1;

	TokenContext tokens;
	tokens.setClock(std::mktime(&noon));

	CHECK_EQ(tokens.expand("{until:13:30}"), std::string("1:30:00"));
	CHECK_EQ(tokens.expand("{until:2026-01-10 12:01}"), std::string("1:00"));
}

TEST(durations)
{
	CHECK_EQ(parseDuration("90"), 90.0);
	CHECK_EQ(parseDuration("1:30"), 90.0);
	CHECK_EQ(parseDuration("1:00:00"), 3600.0);
	CHECK(parseDuration("a:b") < 0.0);
	CHECK_EQ(formatDuration(3725.0), std::string("1:02:05"));
}

// ---- variables ---------------------------------------------------------------

TEST(variables_parse_assignments)
{
	const VariableMap values = parseAssignments("# comment\nname = Ana\nempty=\n=bad\nx=a=b\n");

	CHECK_EQ(values.at("name"), std::string("Ana"));
	CHECK_EQ(values.at("empty"), std::string(""));
	CHECK_EQ(values.at("x"), std::string("a=b"));
	CHECK(values.find("") == values.end());
}
