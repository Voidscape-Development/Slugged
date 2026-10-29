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
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <utility>

// Shared value types for the document model and the text pipeline.
//
// Nothing in core/ or text/ includes libobs or Qt: the whole pipeline from
// document to positioned glyphs is plain C++ so it can be unit-tested without an
// OBS instance. Conversion to and from obs_data_t lives in obs/settings.cpp, and
// GPU resources live in render/.

namespace slugged {

// Straight (non-premultiplied) RGBA in 0..1.
struct Rgba {
	float r = 1.0f;
	float g = 1.0f;
	float b = 1.0f;
	float a = 1.0f;

	// OBS stores colours as 0xAABBGGRR (obs_property_add_color_alpha).
	static Rgba fromObsColor(uint32_t abgr)
	{
		return Rgba{
			float(abgr & 0xFF) / 255.0f,
			float((abgr >> 8) & 0xFF) / 255.0f,
			float((abgr >> 16) & 0xFF) / 255.0f,
			float((abgr >> 24) & 0xFF) / 255.0f,
		};
	}

	uint32_t toObsColor() const
	{
		auto q = [](float v) -> uint32_t {
			const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
			return uint32_t(c * 255.0f + 0.5f);
		};

		return q(r) | (q(g) << 8) | (q(b) << 16) | (q(a) << 24);
	}

	bool operator==(const Rgba &) const = default;
};

// ---------------------------------------------------------------------------
// Enumerations
//
// Every enum that reaches the settings store is stored as a string, so a script
// or obs-websocket client writes "fade" rather than a number that silently means
// something else after the list is reordered. EnumTraits holds the one table
// both directions are derived from.
// ---------------------------------------------------------------------------

template<typename E> struct EnumTraits;

#define SLUGGED_ENUM_TRAITS(Type, ...)                                              \
	template<> struct EnumTraits<Type> {                                        \
		static constexpr std::pair<Type, const char *> values[] = {__VA_ARGS__}; \
	};

template<typename E> const char *enumToString(E value)
{
	for (const auto &[v, name] : EnumTraits<E>::values)
		if (v == value)
			return name;

	return EnumTraits<E>::values[0].second;
}

// Unknown or empty strings yield `fallback`, so a typo in a script degrades to the
// default rather than to an out-of-range enum value.
template<typename E> E enumFromString(const char *name, E fallback)
{
	if (!name || !*name)
		return fallback;

	for (const auto &[v, n] : EnumTraits<E>::values)
		if (std::strcmp(n, name) == 0)
			return v;

	return fallback;
}

enum class FillType { Solid, Linear, Radial };
SLUGGED_ENUM_TRAITS(FillType, {FillType::Solid, "solid"}, {FillType::Linear, "linear"}, {FillType::Radial, "radial"})

// What a gradient is stretched across.
enum class GradientSpan {
	// The whole text block, so one ramp runs through every character.
	Text,
	// Each character's own box, so every letter carries the full ramp.
	Character,
};
SLUGGED_ENUM_TRAITS(GradientSpan, {GradientSpan::Text, "text"}, {GradientSpan::Character, "character"})

enum class LineJoin { Round, Miter, Bevel };
SLUGGED_ENUM_TRAITS(LineJoin, {LineJoin::Round, "round"}, {LineJoin::Miter, "miter"}, {LineJoin::Bevel, "bevel"})

// Start and End follow the paragraph's direction: Start is the left edge for
// left-to-right text and the right edge for right-to-left text.
enum class HAlign { Start, Center, End, Justify };
SLUGGED_ENUM_TRAITS(HAlign, {HAlign::Start, "start"}, {HAlign::Center, "center"}, {HAlign::End, "end"},
		    {HAlign::Justify, "justify"})

enum class VAlign { Top, Middle, Bottom };
SLUGGED_ENUM_TRAITS(VAlign, {VAlign::Top, "top"}, {VAlign::Middle, "middle"}, {VAlign::Bottom, "bottom"})

enum class WrapMode { None, Word, Character };
SLUGGED_ENUM_TRAITS(WrapMode, {WrapMode::None, "none"}, {WrapMode::Word, "word"}, {WrapMode::Character, "character"})

enum class Direction { Auto, LTR, RTL };
SLUGGED_ENUM_TRAITS(Direction, {Direction::Auto, "auto"}, {Direction::LTR, "ltr"}, {Direction::RTL, "rtl"})

// ---------------------------------------------------------------------------
// Style
// ---------------------------------------------------------------------------

using AxisMap = std::map<std::string, float>;

// A font request. `file` wins over `family` when both are set, so a scene
// collection can ship a .ttf next to itself and render identically on a machine
// where that font was never installed.
struct FontSpec {
	std::string family;
	std::string file;
	int faceIndex = 0;

	int weight = 400; // CSS-style 100..900
	bool italic = false;

	// Variable-font axis settings keyed by 4-character tag ("wght", "wdth",
	// "slnt", ...). Applied per shaping call, so many settings of one variable
	// font share a single opened file.
	AxisMap axes;

	bool operator==(const FontSpec &) const = default;
};

// A two-stop fill. The ramp is evaluated per pixel on the GPU.
struct Fill {
	FillType type = FillType::Solid;

	Rgba color;
	Rgba color2{0.35f, 0.6f, 1.0f, 1.0f};

	// Linear only: direction in degrees clockwise from +x.
	float angleDeg = 90.0f;

	GradientSpan span = GradientSpan::Text;

	bool operator==(const Fill &) const = default;
};

struct Outline {
	bool enabled = false;

	// Visible width in source pixels, outside the glyph's edge.
	float widthPx = 2.0f;

	Rgba color{0.0f, 0.0f, 0.0f, 1.0f};
	LineJoin join = LineJoin::Round;

	bool operator==(const Outline &) const = default;
};

struct Shadow {
	bool enabled = false;
	float offsetX = 2.0f;
	float offsetY = 2.0f;
	Rgba color{0.0f, 0.0f, 0.0f, 0.5f};

	bool operator==(const Shadow &) const = default;
};

// Everything that can vary between two adjacent characters.
struct Style {
	FontSpec font;
	float sizePx = 64.0f;

	Fill fill;
	Outline outline;
	Shadow shadow;

	// Extra advance in source pixels, added after shaping so it never disturbs
	// kerning or ligature formation.
	float letterSpacing = 0.0f;
	float wordSpacing = 0.0f;

	// BCP-47 tag handed to HarfBuzz. Empty means "guess from the script".
	std::string language;

	// Fixed-width digits (the OpenType `tnum` feature), so a clock or counter
	// does not jitter sideways as its digits change.
	bool tabularDigits = true;

	// Further OpenType features in HarfBuzz syntax, comma separated, e.g.
	// "smcp,-liga,ss01".
	std::string features;

	bool underline = false;
	bool strikeout = false;

	bool operator==(const Style &) const = default;
};

// Every Style field that a run can override, as (name, type, member, settings key).
// One list drives the override struct, merging, diffing and serialisation, so a
// new style field is added in exactly one place.
#define SLUGGED_STYLE_FIELDS(X)                                          \
	X(family, std::string, font.family, "font_family")               \
	X(fontFile, std::string, font.file, "font_file")                 \
	X(faceIndex, int, font.faceIndex, "font_face_index")             \
	X(weight, int, font.weight, "font_weight")                       \
	X(italic, bool, font.italic, "italic")                           \
	X(axes, AxisMap, font.axes, "font_axes")                         \
	X(size, float, sizePx, "size")                                   \
	X(fillType, FillType, fill.type, "fill")                         \
	X(color, Rgba, fill.color, "color")                              \
	X(color2, Rgba, fill.color2, "gradient_color")                   \
	X(gradientAngle, float, fill.angleDeg, "gradient_angle")         \
	X(gradientSpan, GradientSpan, fill.span, "gradient_span")        \
	X(outline, bool, outline.enabled, "outline")                     \
	X(outlineWidth, float, outline.widthPx, "outline_width")         \
	X(outlineColor, Rgba, outline.color, "outline_color")            \
	X(outlineJoin, LineJoin, outline.join, "outline_join")           \
	X(shadow, bool, shadow.enabled, "shadow")                        \
	X(shadowX, float, shadow.offsetX, "shadow_x")                    \
	X(shadowY, float, shadow.offsetY, "shadow_y")                    \
	X(shadowColor, Rgba, shadow.color, "shadow_color")               \
	X(letterSpacing, float, letterSpacing, "letter_spacing")         \
	X(wordSpacing, float, wordSpacing, "word_spacing")               \
	X(language, std::string, language, "language")                   \
	X(tabularDigits, bool, tabularDigits, "tabular_digits")          \
	X(features, std::string, features, "font_features")              \
	X(underline, bool, underline, "underline")                       \
	X(strikeout, bool, strikeout, "strikeout")

// The parts of a Style a run sets for itself. Anything left unset follows the
// source's base style, so changing the base (from the properties dialog, a
// script, or the editor with nothing selected) restyles all text except what was
// deliberately styled differently -- the same cascade CSS uses.
struct StyleOverride {
#define SLUGGED_X(name, type, member, key) std::optional<type> name;
	SLUGGED_STYLE_FIELDS(SLUGGED_X)
#undef SLUGGED_X

	// Writes every set field into `style`.
	void applyTo(Style &style) const
	{
#define SLUGGED_X(name, type, member, key) \
	if (name)                          \
		style.member = *name;
		SLUGGED_STYLE_FIELDS(SLUGGED_X)
#undef SLUGGED_X
	}

	// Fields set in `other` replace this override's own.
	void merge(const StyleOverride &other)
	{
#define SLUGGED_X(name, type, member, key) \
	if (other.name)                    \
		name = other.name;
		SLUGGED_STYLE_FIELDS(SLUGGED_X)
#undef SLUGGED_X
	}

	// Unsets every field that is set in `mask`, so the run follows the base for
	// those fields again.
	void clear(const StyleOverride &mask)
	{
#define SLUGGED_X(name, type, member, key) \
	if (mask.name)                     \
		name.reset();
		SLUGGED_STYLE_FIELDS(SLUGGED_X)
#undef SLUGGED_X
	}

	bool empty() const
	{
#define SLUGGED_X(name, type, member, key) \
	if (name)                          \
		return false;
		SLUGGED_STYLE_FIELDS(SLUGGED_X)
#undef SLUGGED_X
		return true;
	}

	Style resolve(const Style &base) const
	{
		Style out = base;
		applyTo(out);
		return out;
	}

	// The override that turns `base` into `styled`.
	static StyleOverride diff(const Style &base, const Style &styled)
	{
		StyleOverride out;

#define SLUGGED_X(name, type, member, key) \
	if (!(base.member == styled.member))  \
		out.name = styled.member;
		SLUGGED_STYLE_FIELDS(SLUGGED_X)
#undef SLUGGED_X

		return out;
	}

	bool operator==(const StyleOverride &) const = default;
};

// ---------------------------------------------------------------------------
// Paragraph style
// ---------------------------------------------------------------------------

struct ParagraphStyle {
	HAlign align = HAlign::Start;
	Direction direction = Direction::Auto;

	// Multiple of the font's own line height.
	float lineHeight = 1.0f;

	float spaceBefore = 0.0f;
	float spaceAfter = 0.0f;

	bool operator==(const ParagraphStyle &) const = default;
};

#define SLUGGED_PARAGRAPH_FIELDS(X)                         \
	X(align, HAlign, align, "align")                    \
	X(direction, Direction, direction, "direction")     \
	X(lineHeight, float, lineHeight, "line_height")     \
	X(spaceBefore, float, spaceBefore, "space_before") \
	X(spaceAfter, float, spaceAfter, "space_after")

struct ParagraphOverride {
#define SLUGGED_X(name, type, member, key) std::optional<type> name;
	SLUGGED_PARAGRAPH_FIELDS(SLUGGED_X)
#undef SLUGGED_X

	void applyTo(ParagraphStyle &style) const
	{
#define SLUGGED_X(name, type, member, key) \
	if (name)                          \
		style.member = *name;
		SLUGGED_PARAGRAPH_FIELDS(SLUGGED_X)
#undef SLUGGED_X
	}

	void merge(const ParagraphOverride &other)
	{
#define SLUGGED_X(name, type, member, key) \
	if (other.name)                    \
		name = other.name;
		SLUGGED_PARAGRAPH_FIELDS(SLUGGED_X)
#undef SLUGGED_X
	}

	void clear(const ParagraphOverride &mask){
#define SLUGGED_X(name, type, member, key) \
	if (mask.name)                     \
		name.reset();
		SLUGGED_PARAGRAPH_FIELDS(SLUGGED_X)
#undef SLUGGED_X
	}

	ParagraphStyle resolve(const ParagraphStyle &base) const
	{
		ParagraphStyle out = base;
		applyTo(out);
		return out;
	}

	bool operator==(const ParagraphOverride &) const = default;
};

// ---------------------------------------------------------------------------
// Motion
// ---------------------------------------------------------------------------

// Must stay in sync with the MOTION_* defines in data/effects/slugged.effect.
enum class Motion {
	None = 0,
	Fade = 1,
	Slide = 2,
	Pop = 3,
	Typewriter = 4,
	Wave = 5,
	Blur = 6,
	Spin = 7,
	Bounce = 8,
};
SLUGGED_ENUM_TRAITS(Motion, {Motion::None, "none"}, {Motion::Fade, "fade"}, {Motion::Slide, "slide"},
		    {Motion::Pop, "pop"}, {Motion::Typewriter, "typewriter"}, {Motion::Wave, "wave"},
		    {Motion::Blur, "blur"}, {Motion::Spin, "spin"}, {Motion::Bounce, "bounce"})

// How a motion preset is distributed across the text.
enum class MotionOrder { Together, PerCharacter, PerWord, PerLine };
SLUGGED_ENUM_TRAITS(MotionOrder, {MotionOrder::Together, "together"}, {MotionOrder::PerCharacter, "character"},
		    {MotionOrder::PerWord, "word"}, {MotionOrder::PerLine, "line"})

// Which way a displacing preset travels before it settles. Slide and Bounce come
// *from* this direction; Wave oscillates along it.
enum class MotionDirection { Up, Down, Left, Right };
SLUGGED_ENUM_TRAITS(MotionDirection, {MotionDirection::Up, "up"}, {MotionDirection::Down, "down"},
		    {MotionDirection::Left, "left"}, {MotionDirection::Right, "right"})

// What makes a one-shot preset play again. Every trigger also plays when the
// source is created and when the motion settings themselves change.
enum class MotionTrigger {
	// Only on creation and settings changes (and the button, hotkey or proc).
	Never,
	// Whenever the text's content changes. Live tokens ticking over ({time},
	// {fps}, ...) are not content changes; a variable being set is.
	TextChange,
	// Whenever the source becomes visible anywhere, including the Studio Mode
	// preview and projectors.
	Show,
	// Whenever the source goes live on the program output. The right choice for
	// an intro in Studio Mode, where "shown" happens before the transition.
	Activate,
	// On a fixed cycle.
	Loop,
};
SLUGGED_ENUM_TRAITS(MotionTrigger, {MotionTrigger::Never, "never"}, {MotionTrigger::TextChange, "text"},
		    {MotionTrigger::Show, "show"}, {MotionTrigger::Activate, "activate"}, {MotionTrigger::Loop, "loop"})

// Which characters a text-change replay animates.
enum class MotionScope {
	// Only the characters that changed, so a counter ticking 99 -> 100 animates
	// just its new digits. Show, loop and manual replays always animate all.
	Changed,
	All,
};
SLUGGED_ENUM_TRAITS(MotionScope, {MotionScope::Changed, "changed"}, {MotionScope::All, "all"})

// True for presets that settle into a final state and then hold it. Wave never
// stops, so it has no transition to time, stagger or replay.
inline bool motionIsTransient(Motion motion)
{
	return motion != Motion::None && motion != Motion::Wave;
}

inline bool motionHasDirection(Motion motion)
{
	return motion == Motion::Slide || motion == Motion::Bounce || motion == Motion::Wave;
}

// MotionSpec::param means something different per preset (a distance, a scale,
// an angle, a blur width), so each preset carries its own range and default.
struct MotionParamSpec {
	float min = 0.0f;
	float max = 0.0f;
	float step = 1.0f;
	float defaultValue = 0.0f;

	// Locale key for the control's label, or nullptr when the preset has none.
	const char *label = nullptr;
};

inline MotionParamSpec motionParamSpec(Motion motion)
{
	switch (motion) {
	case Motion::Slide:
		return {-1000.0f, 1000.0f, 1.0f, 48.0f, "Motion.Amount.Distance"};
	case Motion::Bounce:
		return {-1000.0f, 1000.0f, 1.0f, 64.0f, "Motion.Amount.Distance"};
	case Motion::Pop:
		return {0.0f, 8.0f, 0.05f, 0.4f, "Motion.Amount.Scale"};
	case Motion::Wave:
		return {-500.0f, 500.0f, 1.0f, 12.0f, "Motion.Amount.Height"};
	case Motion::Blur:
		return {0.0f, 200.0f, 0.5f, 16.0f, "Motion.Amount.Blur"};
	case Motion::Spin:
		return {-1440.0f, 1440.0f, 5.0f, 90.0f, "Motion.Amount.Angle"};
	case Motion::None:
	case Motion::Fade:
	case Motion::Typewriter:
		break;
	}

	return {};
}

struct MotionSpec {
	Motion motion = Motion::None;
	MotionOrder order = MotionOrder::PerCharacter;
	MotionDirection direction = MotionDirection::Up;

	// Seconds for one unit's transition, and the delay between units.
	float duration = 0.35f;
	float stagger = 0.03f;

	// Preset-specific; see motionParamSpec().
	float param = 48.0f;

	// Cycles per second for Wave.
	float speed = 0.5f;

	MotionTrigger trigger = MotionTrigger::TextChange;
	MotionScope scope = MotionScope::Changed;

	// Play the preset in reverse on the old text before a text change brings
	// the new text in.
	bool exit = false;

	// Seconds between replays for MotionTrigger::Loop.
	float loopInterval = 5.0f;

	// How long one pass takes once the stagger is spread over `units`.
	float passDuration(uint32_t units) const
	{
		const float spread = units > 0 ? stagger * float(units - 1) : 0.0f;

		return duration + (spread > 0.0f ? spread : 0.0f);
	}

	bool operator==(const MotionSpec &) const = default;
};

// Continuous movement of the whole text through its box.
struct ScrollSpec {
	bool enabled = false;
	float speedX = -120.0f; // px/second; negative moves left
	float speedY = 0.0f;

	// Repeat the text endlessly. Off, it crosses the box once per replay.
	bool loop = true;

	// Gap in px between one repeat's tail and the next one's head.
	float gap = 64.0f;

	bool operator==(const ScrollSpec &) const = default;
};

} // namespace slugged
