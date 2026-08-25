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

#include "layout.hpp"
#include "font_manager.hpp"

#include <algorithm>
#include <cmath>

namespace slugged {

namespace {

// A glyph with its advance already resolved to pixels, before line breaking.
struct Measured {
	ShapedGlyph glyph;
	const Style *style = nullptr;
	float advancePx = 0.0f;
};

// One line's worth of measured glyphs.
struct PendingLine {
	std::vector<Measured> glyphs;
	float width = 0.0f;
	float ascent = 0.0f;
	float descent = 0.0f;
	float lineHeight = 0.0f;
};

void accumulateMetrics(PendingLine &line, const Measured &m)
{
	if (!m.glyph.face || !m.style)
		return;

	const float size = m.style->sizePx;

	line.ascent = std::max(line.ascent, m.glyph.face->ascender * size);
	line.descent = std::max(line.descent, m.glyph.face->descender * size);
	line.lineHeight = std::max(line.lineHeight, m.glyph.face->defaultLineHeight() * size);
}

// Recomputes a line's metrics from scratch. Needed after a break moves glyphs
// between lines, since metrics are maxima and cannot simply be subtracted.
void recomputeMetrics(PendingLine &line)
{
	line.width = 0.0f;
	line.ascent = 0.0f;
	line.descent = 0.0f;
	line.lineHeight = 0.0f;

	for (const Measured &m : line.glyphs) {
		line.width += m.advancePx;
		accumulateMetrics(line, m);
	}
}

// One block's lines, held until every block has been measured. Alignment cannot
// be resolved during measuring because in auto-size mode it is relative to the
// widest line in the whole document, which is not known until the last block.
struct BlockLines {
	const Block *block = nullptr;
	std::vector<PendingLine> lines;
};

// An underline or strikeout span being accumulated as the pen moves along a
// line. It stays open across the spaces inside a run so the bar comes out
// continuous rather than one rectangle per word.
struct PendingDecoration {
	bool active = false;
	const Style *style = nullptr;
	FontFace *face = nullptr;
	float startX = 0.0f;
	uint32_t ordinal = 0;
	uint32_t wordIndex = 0;
};

// An empty line still occupies vertical space, so fall back to the block's
// default style metrics when it has no glyphs of its own.
void ensureMetrics(PendingLine &line, const Style &fallbackStyle)
{
	if (line.lineHeight > 0.0f)
		return;

	FontFace *face = FontManager::instance().acquire(fallbackStyle.font);

	if (face) {
		line.ascent = face->ascender * fallbackStyle.sizePx;
		line.descent = face->descender * fallbackStyle.sizePx;
		line.lineHeight = face->defaultLineHeight() * fallbackStyle.sizePx;
	} else {
		line.ascent = fallbackStyle.sizePx * 0.8f;
		line.descent = fallbackStyle.sizePx * 0.2f;
		line.lineHeight = fallbackStyle.sizePx * 1.2f;
	}
}

} // namespace

LayoutResult Layout::run(const Document &doc, float availableWidth)
{
	LayoutResult result;

	const bool wraps = doc.wrap != WrapMode::None && availableWidth > 0.0f;

	// ---- phase 1: shape, measure and break every block ---------------------
	std::vector<BlockLines> measuredBlocks;
	measuredBlocks.reserve(doc.blocks.size());

	float widestLine = 0.0f;

	for (const Block &block : doc.blocks) {
		BlockLines entry;
		entry.block = &block;

		// ---- shape the whole block in one pass ---------------------------
		std::vector<ShapeRun> shapeRuns;
		shapeRuns.reserve(block.runs.size());

		for (uint32_t i = 0; i < block.runs.size(); i++)
			shapeRuns.push_back(ShapeRun{block.runs[i].text, &block.runs[i].style, i});

		const std::vector<ShapedGlyph> shaped = Shaper::shape(shapeRuns, block.direction);

		// ---- resolve advances to pixels ----------------------------------
		std::vector<Measured> measured;
		measured.reserve(shaped.size());

		for (const ShapedGlyph &g : shaped) {
			if (g.runIndex >= block.runs.size())
				continue;

			const Style &style = block.runs[g.runIndex].style;

			Measured m;
			m.glyph = g;
			m.style = &style;

			// Letter and word spacing are applied after shaping so they never
			// disturb kerning or prevent ligatures from forming.
			m.advancePx = g.advance * style.sizePx + style.letterSpacing +
				      (g.whitespace ? style.wordSpacing : 0.0f);

			measured.push_back(std::move(m));
		}

		// ---- break into lines --------------------------------------------
		std::vector<PendingLine> &lines = entry.lines;
		PendingLine current;

		// Index within `current` of the most recent point we could break at.
		size_t lastBreak = SIZE_MAX;

		for (const Measured &m : measured) {
			const bool fits = !wraps || current.width + m.advancePx <= availableWidth;

			if (!fits && !current.glyphs.empty()) {
				if (doc.wrap == WrapMode::Word && lastBreak != SIZE_MAX) {
					// Move everything after the break onto the next line,
					// dropping the breaking space itself so it does not
					// show up as leading whitespace.
					PendingLine next;

					for (size_t i = lastBreak + 1; i < current.glyphs.size(); i++)
						next.glyphs.push_back(current.glyphs[i]);

					current.glyphs.resize(lastBreak);

					recomputeMetrics(current);
					recomputeMetrics(next);

					lines.push_back(std::move(current));
					current = std::move(next);
				} else {
					// Character wrapping, or a single word longer than the
					// whole line: break right here.
					lines.push_back(std::move(current));
					current = PendingLine{};
				}

				lastBreak = SIZE_MAX;
			}

			if (m.glyph.whitespace)
				lastBreak = current.glyphs.size();

			current.width += m.advancePx;
			accumulateMetrics(current, m);
			current.glyphs.push_back(m);
		}

		lines.push_back(std::move(current));

		const Style &blockStyle = block.runs.empty() ? doc.defaultStyle : block.runs.front().style;

		for (PendingLine &line : lines) {
			ensureMetrics(line, blockStyle);

			widestLine = std::max(widestLine, line.width);
		}

		measuredBlocks.push_back(std::move(entry));
	}

	// Alignment needs an extent to align against. With a fixed box that is the
	// box's inner width; in auto-size mode the source grows to its own content,
	// so lines align against the widest line in the document. Falling back to
	// each line's own width instead -- which is what the single-pass version
	// did -- silently turns every alignment except Left into a no-op for an
	// auto-sized source.
	const float limit = availableWidth > 0.0f ? availableWidth : widestLine;

	// ---- phase 2: place every line -----------------------------------------
	float penY = 0.0f;
	uint32_t lineIndex = 0;
	uint32_t ordinal = 0;
	uint32_t wordCounter = 0;

	for (BlockLines &entry : measuredBlocks) {
		const Block &block = *entry.block;
		std::vector<PendingLine> &lines = entry.lines;

		penY += block.spaceBefore;

		for (PendingLine &line : lines) {
			const float lineHeight = line.lineHeight * block.lineHeight;

			// Distribute the leading evenly above and below, so changing line
			// height does not shift the first baseline oddly.
			const float leading = std::max(0.0f, lineHeight - (line.ascent + line.descent));
			const float baselineY = penY + leading * 0.5f + line.ascent;

			float startX = 0.0f;
			float justifyExtra = 0.0f;

			switch (block.align) {
			case HAlign::Left:
				startX = 0.0f;
				break;
			case HAlign::Center:
				startX = (limit - line.width) * 0.5f;
				break;
			case HAlign::Right:
				startX = limit - line.width;
				break;
			case HAlign::Justify: {
				startX = 0.0f;

				// Justification stretches the spaces, not the letters, and
				// never the last line of a block.
				const bool isLast = (&line == &lines.back());
				size_t spaces = 0;

				for (const Measured &m : line.glyphs)
					if (m.glyph.whitespace)
						spaces++;

				if (!isLast && spaces > 0 && limit > line.width)
					justifyExtra = (limit - line.width) / float(spaces);

				break;
			}
			}

			LineInfo info;
			info.baselineY = baselineY;
			info.width = line.width;
			info.ascent = line.ascent;
			info.descent = line.descent;
			info.firstGlyph = result.glyphs.size();

			float penX = startX;
			bool inWord = false;

			PendingDecoration underline;
			PendingDecoration strikeout;

			// Closes an open span at `endX` and emits its bar. `strike` picks
			// which set of face metrics to measure against.
			auto closeSpan = [&](PendingDecoration &span, bool strike, float endX) {
				if (!span.active)
					return;

				span.active = false;

				if (!span.style || !span.face || endX <= span.startX)
					return;

				const float size = span.style->sizePx;

				const float thickness = std::max(1.0f, (strike ? span.face->strikeoutThickness
									       : span.face->underlineThickness) *
									       size);

				// Both metrics are y-up about the baseline while layout is
				// y-down, hence the negation. FreeType reports the underline
				// position as the centre of the stem, but OS/2 reports the
				// strikeout position as its top edge, so only the underline
				// needs re-centring.
				const float pos = strike ? span.face->strikeoutPos : span.face->underlinePos;

				DecorationRect rect;

				rect.x = span.startX;
				rect.y = baselineY - pos * size - (strike ? 0.0f : thickness * 0.5f);
				rect.width = endX - span.startX;
				rect.height = thickness;
				rect.style = span.style;
				rect.lineIndex = lineIndex;
				rect.wordIndex = span.wordIndex;
				rect.ordinal = span.ordinal;

				result.decorations.push_back(rect);
			};

			for (const Measured &m : line.glyphs) {
				if (m.glyph.whitespace) {
					inWord = false;
				} else if (!inWord) {
					inWord = true;
					wordCounter++;
				}

				// A span breaks when the decoration turns off or the style
				// changes, so two differently coloured underlined runs produce
				// two bars rather than one in the first run's colour.
				const bool wantUnderline = m.style && m.style->underline;
				const bool wantStrikeout = m.style && m.style->strikeout;

				if (underline.active && (!wantUnderline || underline.style != m.style))
					closeSpan(underline, false, penX);

				if (strikeout.active && (!wantStrikeout || strikeout.style != m.style))
					closeSpan(strikeout, true, penX);

				for (int pass = 0; pass < 2; pass++) {
					PendingDecoration &span = pass == 0 ? underline : strikeout;

					if (!(pass == 0 ? wantUnderline : wantStrikeout))
						continue;

					if (!span.active) {
						span.active = true;
						span.style = m.style;
						span.face = m.glyph.face;
						span.startX = penX;
						span.ordinal = ordinal;
						span.wordIndex = wordCounter ? wordCounter - 1 : 0;
					} else if (!span.face) {
						// The span opened on a space, which may carry no face.
						span.face = m.glyph.face;
					}
				}

				PositionedGlyph pg;

				pg.face = m.glyph.face;
				pg.glyphIndex = m.glyph.glyphIndex;
				pg.sizePx = m.style ? m.style->sizePx : 0.0f;
				pg.style = m.style;
				pg.lineIndex = lineIndex;
				pg.wordIndex = wordCounter ? wordCounter - 1 : 0;
				pg.rtl = m.glyph.rtl;

				// Shaping offsets position marks relative to their base glyph;
				// y is negated because shaping is y-up and layout is y-down.
				pg.x = penX + m.glyph.offsetX * pg.sizePx;
				pg.y = baselineY - m.glyph.offsetY * pg.sizePx;

				// Whitespace carries no outline, so it is measured but never
				// emitted; skipping it here keeps animation ordinals counting
				// only glyphs the viewer can actually see.
				if (!m.glyph.whitespace) {
					pg.ordinal = ordinal++;
					result.glyphs.push_back(pg);
				}

				penX += m.advancePx + (m.glyph.whitespace ? justifyExtra : 0.0f);
			}

			closeSpan(underline, false, penX);
			closeSpan(strikeout, true, penX);

			info.glyphCount = result.glyphs.size() - info.firstGlyph;
			result.lines.push_back(info);

			// A justified line is stretched out to `limit` without its measured
			// width changing, so it contributes the full limit to the extent.
			result.width = std::max(result.width, justifyExtra > 0.0f ? limit : startX + line.width);

			penY += lineHeight;
			lineIndex++;
		}

		penY += block.spaceAfter;
	}

	result.height = penY;
	result.wordCount = wordCounter;

	// Vertical alignment shifts every baseline at once. Only meaningful when the
	// box is taller than the text.
	if (doc.sizeMode == SizeMode::Fixed && doc.valign != VAlign::Top) {
		const float slack = doc.boxHeight - 2.0f * doc.padding - result.height;

		if (slack > 0.0f) {
			const float shift = doc.valign == VAlign::Middle ? slack * 0.5f : slack;

			for (PositionedGlyph &g : result.glyphs)
				g.y += shift;

			for (DecorationRect &d : result.decorations)
				d.y += shift;

			for (LineInfo &l : result.lines)
				l.baselineY += shift;
		}
	}

	return result;
}

} // namespace slugged
