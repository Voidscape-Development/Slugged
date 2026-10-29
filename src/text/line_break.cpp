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

#include "line_break.hpp"
#include "shaper.hpp"

namespace slugged {

namespace {

bool isSpace(uint32_t cp)
{
	return cp == ' ' || cp == '\t' || cp == 0x3000 || (cp >= 0x2000 && cp <= 0x2006) || (cp >= 0x2008 && cp <= 0x200A);
}

bool isDash(uint32_t cp)
{
	return cp == '-' || cp == 0x2010 || cp == 0x2012 || cp == 0x2013 || cp == 0x2014;
}

// Scripts written without spaces between words, where a line may break between
// any two characters.
bool isIdeographic(uint32_t cp)
{
	return (cp >= 0x2E80 && cp <= 0x2FFF) || (cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x3100 && cp <= 0x31FF) ||
	       (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) ||
	       (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF01 && cp <= 0xFF60) || (cp >= 0x20000 && cp <= 0x3FFFF) ||
	       (cp >= 0x1F300 && cp <= 0x1FAFF);
}

// Must not begin a line.
bool isClosing(uint32_t cp)
{
	switch (cp) {
	case 0x3001: // 、
	case 0x3002: // 。
	case 0xFF0C: // ，
	case 0xFF0E: // ．
	case 0xFF1A: // ：
	case 0xFF1B: // ；
	case 0xFF01: // ！
	case 0xFF1F: // ？
	case 0xFF09: // ）
	case 0x300D: // 」
	case 0x300F: // 』
	case 0x3011: // 】
	case 0x3015: // 〕
	case 0x3009: // 〉
	case 0x300B: // 》
	case 0x30FC: // ー
	case 0x2026: // …
	case 0x3005: // 々
	case 0x309D: // ゝ
	case 0x30FD: // ヽ
	case ')':
	case ']':
	case '}':
	case ',':
	case '.':
	case ':':
	case ';':
	case '!':
	case '?':
		return true;
	default:
		break;
	}

	// Small kana.
	static const uint32_t smallKana[] = {0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x3063, 0x3083, 0x3085,
					     0x3087, 0x308E, 0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9, 0x30C3,
					     0x30E3, 0x30E5, 0x30E7, 0x30EE, 0x30F5, 0x30F6};

	for (uint32_t k : smallKana)
		if (cp == k)
			return true;

	return false;
}

// Must not end a line.
bool isOpening(uint32_t cp)
{
	switch (cp) {
	case 0xFF08: // （
	case 0x300C: // 「
	case 0x300E: // 『
	case 0x3010: // 【
	case 0x3014: // 〔
	case 0x3008: // 〈
	case 0x300A: // 《
	case '(':
	case '[':
	case '{':
		return true;
	default:
		return false;
	}
}

bool isWordChar(uint32_t cp)
{
	return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
	       (cp >= 0xC0 && !isSpace(cp) && !isIdeographic(cp) && !isDash(cp) && cp != 0x200B);
}

} // namespace

std::vector<bool> lineBreakOpportunities(const std::string &text)
{
	std::vector<bool> out(text.size() + 1, false);

	struct Char {
		uint32_t cp;
		size_t start;
	};

	std::vector<Char> chars;

	for (size_t pos = 0; pos < text.size();) {
		const size_t start = pos;
		chars.push_back({utf8Next(text, pos), start});
	}

	for (size_t i = 1; i < chars.size(); i++) {
		const uint32_t prev = chars[i - 1].cp;
		const uint32_t cur = chars[i].cp;

		bool allowed = false;

		if (isSpace(prev) && !isSpace(cur))
			allowed = true;
		else if (prev == 0x200B)
			allowed = true;
		else if (isDash(prev) && isWordChar(cur) && i >= 2 && isWordChar(chars[i - 2].cp))
			allowed = true;
		else if ((isIdeographic(prev) || isIdeographic(cur)) && !isSpace(cur) && !isSpace(prev))
			allowed = true;

		if (allowed && (isClosing(cur) || isOpening(prev)))
			allowed = false;

		out[chars[i].start] = allowed;
	}

	return out;
}

} // namespace slugged
