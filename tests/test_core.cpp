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

TEST(tokens_report_where_they_were_replaced)
{
	TokenContext tokens;
	tokens.set("name", "Ada");

	std::vector<TokenSpan> spans;

	CHECK_EQ(tokens.expand("hi {name}, {{x} {nope}", spans), std::string("hi Ada, {x} {nope}"));
	CHECK_EQ(spans.size(), size_t(2));

	CHECK(spans[0] == (TokenSpan{3, 9, 3, 6}));

	// The "{{" escape shrinks too; an unknown token does not.
	CHECK(spans[1] == (TokenSpan{11, 13, 8, 9}));
}

TEST(document_token_spans_are_in_document_offsets)
{
	TokenContext tokens;
	tokens.set("a", "long value");

	Document doc;
	doc.setPlainText("x{a}\n{a}y");

	std::vector<TokenSpan> spans;

	CHECK(doc.expandTokens(tokens, spans));
	CHECK_EQ(doc.plainText(), std::string("xlong value\nlong valuey"));
	CHECK_EQ(spans.size(), size_t(2));

	CHECK(spans[0] == (TokenSpan{1, 4, 1, 11}));
	CHECK(spans[1] == (TokenSpan{5, 8, 12, 22}));
}

// ---- token filters ------------------------------------------------------------

TEST(tokens_filters_reshape_values)
{
	TokenContext tokens;
	tokens.set("followers", "1234567");
	tokens.set("name", "  ada lovelace ");
	tokens.set("score", "7");
	tokens.set("secs", "125");

	CHECK_EQ(tokens.expand("{followers|number}"), std::string("1,234,567"));
	CHECK_EQ(tokens.expand("{followers|number:.}"), std::string("1.234.567"));
	CHECK_EQ(tokens.expand("{followers|compact}"), std::string("1.2M"));
	CHECK_EQ(tokens.expand("{name|trim|title}"), std::string("Ada Lovelace"));
	CHECK_EQ(tokens.expand("{name|trim|upper}"), std::string("ADA LOVELACE"));
	CHECK_EQ(tokens.expand("{score|pad:3}"), std::string("007"));
	CHECK_EQ(tokens.expand("{score|add:1}"), std::string("8"));
	CHECK_EQ(tokens.expand("{score|mul:1.5}"), std::string("10.5"));
	CHECK_EQ(tokens.expand("{score|div:2|round:1}"), std::string("3.5"));
	CHECK_EQ(tokens.expand("{secs|mm:ss}"), std::string("02:05"));
	CHECK_EQ(tokens.expand("{secs|h:mm:ss}"), std::string("0:02:05"));
	CHECK_EQ(tokens.expand("{name|trim|truncate:5}"), std::string("ada \xE2\x80\xA6"));
	CHECK_EQ(tokens.expand("{score} {score|plural:life,lives}"), std::string("7 lives"));

	tokens.set("score", "1");
	CHECK_EQ(tokens.expand("{score|plural:life,lives}"), std::string("life"));
}

TEST(tokens_default_fills_in_a_missing_value)
{
	TokenContext tokens;

	CHECK_EQ(tokens.expand("{song|default:Nothing playing}"), std::string("Nothing playing"));
	CHECK_EQ(tokens.expand("{song}"), std::string("{song}"));

	tokens.set("song", "");
	CHECK_EQ(tokens.expand("{song|default:Quiet}"), std::string("Quiet"));

	tokens.set("song", "Hello");
	CHECK_EQ(tokens.expand("{song|default:Quiet|upper}"), std::string("HELLO"));

	// A default is something written, so it counts as content.
	CHECK_EQ(tokens.expandContent("{missing|default:x}"), std::string("x"));
}

TEST(tokens_filters_on_live_values_stay_live)
{
	TokenContext tokens;
	tokens.setTimer(75.0f);

	CHECK_EQ(tokens.expand("{timer|mm:ss}"), std::string("01:15"));
	CHECK_EQ(tokens.expandContent("{timer|mm:ss}"), std::string("{timer|mm:ss}"));
}

TEST(tokens_find_reports_the_layer)
{
	TokenContext tokens;
	std::string value;

	tokens.setSourceVariables({{"count", "4"}});
	CHECK(tokens.find("count", value) == TokenContext::Layer::Source);
	CHECK_EQ(value, std::string("4"));

	tokens.set("count", "9");
	CHECK(tokens.find("count", value) == TokenContext::Layer::Runtime);
	CHECK_EQ(value, std::string("9"));

	CHECK(tokens.find("nothing", value) == TokenContext::Layer::None);
}

TEST(numbers_interpolate_between_two_texts)
{
	std::string out;

	CHECK(interpolateNumbers("Followers: 100", "Followers: 200", 0.5, out));
	CHECK_EQ(out, std::string("Followers: 150"));

	CHECK(interpolateNumbers("Followers: 100", "Followers: 200", 0.0, out));
	CHECK_EQ(out, std::string("Followers: 100"));

	CHECK(interpolateNumbers("1,000 subs", "2,000 subs", 0.5, out));
	CHECK_EQ(out, std::string("1,500 subs"));

	CHECK(interpolateNumbers("$1.50", "$2.50", 0.5, out));
	CHECK_EQ(out, std::string("$2.00"));

	// Counting down rounds towards where it started, never past the end.
	CHECK(interpolateNumbers("10", "0", 0.95, out));
	CHECK_EQ(out, std::string("1"));

	// The words around the numbers must match.
	CHECK(!interpolateNumbers("Score 1", "Lives 2", 0.5, out));
	CHECK(!interpolateNumbers("no numbers", "no numbers", 0.5, out));
}

TEST(numbers_parse_like_an_overlay_writes_them)
{
	double v = 0.0;

	CHECK(parseNumber("1,234", v));
	CHECK_EQ(v, 1234.0);
	CHECK(parseNumber(" -3.5 ", v));
	CHECK_EQ(v, -3.5);
	CHECK(!parseNumber("12 apples", v));
	CHECK(!parseNumber("", v));
}

// ---- markup ------------------------------------------------------------------

TEST(markup_effect_tags)
{
	const auto blocks = parseMarkup("calm [shake]angry[/shake] [wave=2]wavy[/wave] [effect=rainbow]hue[/effect]");

	Style base;
	std::vector<Effect> seen;
	float waveStrength = 0.0f;

	for (const Run &run : blocks[0].runs) {
		const Style s = run.style.resolve(base);

		if (s.effect.kind != Effect::None)
			seen.push_back(s.effect.kind);

		if (s.effect.kind == Effect::Wave)
			waveStrength = s.effect.strength;
	}

	CHECK_EQ(seen.size(), size_t(3));
	CHECK(seen[0] == Effect::Shake);
	CHECK(seen[1] == Effect::Wave);
	CHECK(seen[2] == Effect::Rainbow);
	CHECK_EQ(waveStrength, 2.0f);
}

TEST(markup_gradient_and_glow_tags)
{
	const auto blocks = parseMarkup("[gradient=#f00,#0f0,#00f]rgb[/gradient] [glow=#ff0]lit[/glow]");

	Style base;
	const Style gradient = blocks[0].runs[0].style.resolve(base);

	CHECK(gradient.fill.type == FillType::Linear);
	CHECK_EQ(gradient.fill.stops, std::string("#0f0"));
	CHECK_EQ(gradient.fill.color.r, 1.0f);
	CHECK_EQ(gradient.fill.color2.b, 1.0f);

	const Style glow = blocks[0].runs.back().style.resolve(base);

	CHECK(glow.glow.enabled);
	CHECK_EQ(glow.glow.color.g, 1.0f);
}

// ---- document ----------------------------------------------------------------

TEST(document_scale_sizes_scales_runs_and_base)
{
	Document doc;
	doc.base.sizePx = 40.0f;
	doc.base.shadow.offsetX = 4.0f;
	doc.setPlainText("ab");

	StyleOverride big;
	big.size = 80.0f;
	doc.applyStyle(1, 2, big);

	doc.scaleSizes(0.5f);

	CHECK_EQ(doc.base.sizePx, 20.0f);
	CHECK_EQ(doc.base.shadow.offsetX, 2.0f);
	CHECK_EQ(doc.styleAt(2).sizePx, 40.0f);
}
