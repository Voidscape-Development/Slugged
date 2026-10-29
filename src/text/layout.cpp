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
#include "line_break.hpp"

#include <algorithm>
#include <cmath>

namespace slugged {

namespace {

// A glyph with its advance resolved to pixels, before line breaking.
struct Measured {
	ShapedGlyph glyph;
	const Style *style = nullptr;
	float advancePx = 0.0f;
	bool breakBefore = false;
	uint32_t textOffset = 0;
};

struct PendingLine {
	std::vector<Measured> glyphs;
	bool lastInParagraph = false;
};

struct BlockLines {
	ParagraphStyle paragraph;
	uint8_t baseLevel = 0;
	const Style *style = nullptr;
	std::vector<PendingLine> lines;
};

struct Metrics {
	float ascent = 0.0f;
	float descent = 0.0f;
	float lineHeight = 0.0f;
};

Metrics lineMetrics(const PendingLine &line, const Style &fallback)
{
	Metrics m;

	for (const Measured &g : line.glyphs) {
		if (!g.glyph.font || !g.style)
			continue;

		const FontFile &file = *g.glyph.font->file;
		const float size = g.style->sizePx;

		m.ascent = std::max(m.ascent, file.ascender * size);
		m.descent = std::max(m.descent, file.descender * size);
		m.lineHeight = std::max(m.lineHeight, file.defaultLineHeight() * size);
	}

	if (m.lineHeight > 0.0f)
		return m;

	// An empty line still occupies a line of its paragraph's style.
	if (FontInstance *font = FontManager::instance().acquire(fallback.font)) {
		m.ascent = font->file->ascender * fallback.sizePx;
		m.descent = font->file->descender * fallback.sizePx;
		m.lineHeight = font->file->defaultLineHeight() * fallback.sizePx;
	} else {
		m.ascent = fallback.sizePx * 0.8f;
		m.descent = fallback.sizePx * 0.2f;
		m.lineHeight = fallback.sizePx * 1.2f;
	}

	return m;
}

float widthOf(const std::vector<Measured> &glyphs)
{
	float w = 0.0f;

	for (const Measured &m : glyphs)
		w += m.advancePx;

	return w;
}

// Splits `line` before index `at`, returning the remainder with its leading
// whitespace removed (a wrapped line never starts with the space it broke at).
std::vector<Measured> splitAt(std::vector<Measured> &line, size_t at)
{
	std::vector<Measured> rest(line.begin() + long(at), line.end());

	line.resize(at);

	size_t lead = 0;

	while (lead < rest.size() && rest[lead].glyph.whitespace)
		lead++;

	rest.erase(rest.begin(), rest.begin() + long(lead));

	return rest;
}

size_t lastBreakIn(const std::vector<Measured> &line)
{
	for (size_t i = line.size(); i-- > 1;)
		if (line[i].breakBefore)
			return i;

	return 0;
}

// An underline or strikeout span accumulating as the pen moves along a line.
struct PendingDecoration {
	bool active = false;
	const Style *style = nullptr;
	const FontFile *file = nullptr;
	float startX = 0.0f;
	uint32_t ordinal = 0;
	uint32_t wordIndex = 0;
	uint32_t textOffset = 0;
};

} // namespace

LayoutResult Layout::run(const Document &doc, float availableWidth, float availableHeight)
{
	LayoutResult result;

	const bool wraps = doc.wrap != WrapMode::None && availableWidth > 0.0f;

	// ---- phase 1: shape, measure and break every paragraph in logical order --
	std::vector<BlockLines> paragraphs;
	paragraphs.reserve(doc.blocks.size());

	float widest = 0.0f;
	uint32_t blockOffset = 0;

	for (const Block &block : doc.blocks) {
		BlockLines entry;
		entry.paragraph = doc.resolve(block);

		std::vector<ShapeRun> shapeRuns;
		std::vector<const Style *> runStyles;

		for (uint32_t i = 0; i < block.runs.size(); i++) {
			result.styles.push_back(doc.resolve(block.runs[i]));
			runStyles.push_back(&result.styles.back());
			shapeRuns.push_back(ShapeRun{block.runs[i].text, runStyles.back(), i});
		}

		if (runStyles.empty()) {
			result.styles.push_back(doc.base);
			runStyles.push_back(&result.styles.back());
		}

		entry.style = runStyles.front();

		const ShapedParagraph shaped = Shaper::shape(shapeRuns, entry.paragraph.direction);
		const std::vector<bool> breaks = lineBreakOpportunities(shaped.text);

		entry.baseLevel = shaped.baseLevel;

		std::vector<Measured> current;
		float width = 0.0f;
		size_t lastBreak = 0;

		for (const ShapedGlyph &g : shaped.glyphs) {
			Measured m;

			m.glyph = g;
			m.style = g.runIndex < runStyles.size() ? runStyles[g.runIndex] : runStyles.front();
			m.advancePx = g.advance * m.style->sizePx + m.style->letterSpacing +
				      (g.whitespace ? m.style->wordSpacing : 0.0f);
			m.breakBefore = g.clusterStart && g.cluster < breaks.size() && breaks[g.cluster];
			m.textOffset = blockOffset + g.cluster;

			// Whitespace may hang past the edge; only ink has to fit.
			if (wraps && !current.empty() && !g.whitespace && width + m.advancePx > availableWidth) {
				size_t at = 0;

				if (doc.wrap == WrapMode::Word)
					at = lastBreak;

				// No word boundary on this line (or character wrapping): break
				// right here, but never inside a cluster.
				if (at == 0) {
					if (g.clusterStart) {
						at = current.size();
					} else {
						size_t start = current.size();

						while (start > 0 && !current[start - 1].glyph.clusterStart)
							start--;

						// `start - 1` opens the cluster `g` belongs to.
						at = start > 1 ? start - 1 : 0;
					}
				}

				if (at > 0) {
					std::vector<Measured> rest = splitAt(current, at);

					entry.lines.push_back(PendingLine{std::move(current), false});
					current = std::move(rest);
					width = widthOf(current);
					lastBreak = lastBreakIn(current);
				}
			}

			if (m.breakBefore && !current.empty())
				lastBreak = current.size();

			if (current.empty() && !entry.lines.empty() && g.whitespace)
				continue;

			width += m.advancePx;
			current.push_back(std::move(m));
		}

		entry.lines.push_back(PendingLine{std::move(current), true});

		for (PendingLine &line : entry.lines) {
			// Trailing whitespace neither counts toward the width nor draws.
			while (!line.glyphs.empty() && line.glyphs.back().glyph.whitespace)
				line.glyphs.pop_back();

			widest = std::max(widest, widthOf(line.glyphs));
		}

		blockOffset += uint32_t(block.size()) + 1;
		paragraphs.push_back(std::move(entry));
	}

	const float limit = availableWidth > 0.0f ? availableWidth : widest;

	// ---- phase 2: reorder each line for display and place it ---------------
	float penY = 0.0f;
	uint32_t lineIndex = 0;
	uint32_t ordinal = 0;
	uint32_t wordCounter = 0;

	for (BlockLines &entry : paragraphs) {
		const ParagraphStyle &paragraph = entry.paragraph;
		const bool rtl = entry.baseLevel & 1;

		penY += paragraph.spaceBefore;

		for (PendingLine &line : entry.lines) {
			const Metrics metrics = lineMetrics(line, *entry.style);
			const float lineHeight = metrics.lineHeight * paragraph.lineHeight;
			const float leading = std::max(0.0f, lineHeight - (metrics.ascent + metrics.descent));
			const float baselineY = penY + leading * 0.5f + metrics.ascent;

			const size_t count = line.glyphs.size();

			// Reading-order bookkeeping comes first, so staggering follows the
			// text rather than the screen.
			std::vector<uint32_t> ordinals(count, 0);
			std::vector<uint32_t> words(count, 0);
			bool inWord = false;

			for (size_t i = 0; i < count; i++) {
				const Measured &m = line.glyphs[i];

				if (m.glyph.whitespace) {
					inWord = false;
				} else {
					if (!inWord) {
						inWord = true;
						wordCounter++;
					}

					ordinals[i] = ordinal++;
				}

				words[i] = wordCounter ? wordCounter - 1 : 0;
			}

			std::vector<uint8_t> levels(count);
			std::vector<size_t> order(count);

			for (size_t i = 0; i < count; i++) {
				levels[i] = line.glyphs[i].glyph.level;
				order[i] = i;
			}

			reorderLine(levels, order);

			const float width = widthOf(line.glyphs);

			HAlign align = paragraph.align;

			// Justify never stretches a paragraph's last line.
			if (align == HAlign::Justify && line.lastInParagraph)
				align = HAlign::Start;

			float startX = 0.0f;
			float justifyExtra = 0.0f;

			switch (align) {
			case HAlign::Start:
				startX = rtl ? limit - width : 0.0f;
				break;
			case HAlign::End:
				startX = rtl ? 0.0f : limit - width;
				break;
			case HAlign::Center:
				startX = (limit - width) * 0.5f;
				break;
			case HAlign::Justify: {
				size_t spaces = 0;

				for (const Measured &m : line.glyphs)
					if (m.glyph.whitespace)
						spaces++;

				if (spaces > 0 && limit > width)
					justifyExtra = (limit - width) / float(spaces);
				else if (rtl)
					startX = limit - width;

				break;
			}
			}

			LineInfo info;
			info.baselineY = baselineY;
			info.x = startX;
			info.width = justifyExtra > 0.0f ? limit : width;
			info.ascent = metrics.ascent;
			info.descent = metrics.descent;

			float penX = startX;

			PendingDecoration underline;
			PendingDecoration strikeout;

			auto closeSpan = [&](PendingDecoration &span, bool strike, float endX) {
				if (!span.active)
					return;

				span.active = false;

				if (!span.style || !span.file || endX <= span.startX)
					return;

				const float size = span.style->sizePx;
				const float thickness = std::max(
					1.0f, (strike ? span.file->strikeoutThickness : span.file->underlineThickness) * size);

				// Both metrics are y-up about the baseline. FreeType gives the
				// underline's centre but OS/2 gives the strikeout's top edge.
				const float pos = strike ? span.file->strikeoutPos : span.file->underlinePos;

				DecorationRect rect;

				rect.x = span.startX;
				rect.y = baselineY - pos * size - (strike ? 0.0f : thickness * 0.5f);
				rect.width = endX - span.startX;
				rect.height = thickness;
				rect.style = span.style;
				rect.lineIndex = lineIndex;
				rect.wordIndex = span.wordIndex;
				rect.ordinal = span.ordinal;
				rect.textOffset = span.textOffset;

				result.decorations.push_back(rect);
			};

			for (size_t index : order) {
				const Measured &m = line.glyphs[index];

				const bool wantUnderline = m.style && m.style->underline;
				const bool wantStrikeout = m.style && m.style->strikeout;

				if (underline.active && (!wantUnderline || underline.style != m.style))
					closeSpan(underline, false, penX);

				if (strikeout.active && (!wantStrikeout || strikeout.style != m.style))
					closeSpan(strikeout, true, penX);

				for (PendingDecoration *span : {&underline, &strikeout}) {
					const bool want = span == &underline ? wantUnderline : wantStrikeout;

					if (!want)
						continue;

					if (!span->active) {
						span->active = true;
						span->style = m.style;
						span->file = m.glyph.font ? m.glyph.font->file : nullptr;
						span->startX = penX;
						span->ordinal = ordinals[index];
						span->wordIndex = words[index];
						span->textOffset = m.textOffset;
					} else if (!span->file && m.glyph.font) {
						span->file = m.glyph.font->file;
					}
				}

				if (!m.glyph.whitespace) {
					PositionedGlyph pg;

					pg.font = m.glyph.font;
					pg.glyphIndex = m.glyph.glyphIndex;
					pg.sizePx = m.style ? m.style->sizePx : 0.0f;
					pg.style = m.style;
					pg.lineIndex = lineIndex;
					pg.wordIndex = words[index];
					pg.ordinal = ordinals[index];
					pg.textOffset = m.textOffset;

					// Shaping is y-up and layout y-down, hence the negation.
					pg.x = penX + m.glyph.offsetX * pg.sizePx;
					pg.y = baselineY - m.glyph.offsetY * pg.sizePx;

					result.glyphs.push_back(pg);
				}

				penX += m.advancePx + (m.glyph.whitespace ? justifyExtra : 0.0f);
			}

			closeSpan(underline, false, penX);
			closeSpan(strikeout, true, penX);

			result.lines.push_back(info);

			penY += lineHeight;
			lineIndex++;
		}

		penY += paragraph.spaceAfter;
	}

	result.wordCount = wordCounter;
	result.width = limit;
	result.height = penY;

	if (availableHeight > 0.0f) {
		const float slack = availableHeight - penY;

		if (slack > 0.0f && doc.valign != VAlign::Top) {
			const float shift = doc.valign == VAlign::Middle ? slack * 0.5f : slack;

			for (PositionedGlyph &g : result.glyphs)
				g.y += shift;

			for (DecorationRect &d : result.decorations)
				d.y += shift;

			for (LineInfo &l : result.lines)
				l.baselineY += shift;
		}

		result.height = availableHeight;
	}

	return result;
}

} // namespace slugged
