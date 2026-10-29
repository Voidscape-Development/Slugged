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

#include "markup.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace slugged {

namespace {

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
	return s;
}

std::optional<float> parseNumber(const std::string &text)
{
	if (text.empty())
		return std::nullopt;

	char *end = nullptr;
	const float v = std::strtof(text.c_str(), &end);

	if (!end || *end != '\0')
		return std::nullopt;

	return v;
}

struct OpenTag {
	std::string name;
	StyleOverride delta;
};

// Turns a tag body ("b", "color=#f00") into the override it applies. Returns
// nullopt for anything that is not a tag Slugged knows.
std::optional<OpenTag> parseTag(const std::string &body)
{
	const size_t eq = body.find('=');
	const std::string name = lower(body.substr(0, eq));
	const std::string value = eq == std::string::npos ? std::string() : body.substr(eq + 1);

	OpenTag tag;
	tag.name = name;

	if (name == "b" && value.empty()) {
		tag.delta.weight = 700;
	} else if (name == "i" && value.empty()) {
		tag.delta.italic = true;
	} else if (name == "u" && value.empty()) {
		tag.delta.underline = true;
	} else if (name == "s" && value.empty()) {
		tag.delta.strikeout = true;
	} else if (name == "color") {
		const auto color = parseColor(value);

		if (!color)
			return std::nullopt;

		tag.delta.fillType = FillType::Solid;
		tag.delta.color = *color;
	} else if (name == "size") {
		const auto size = parseNumber(value);

		if (!size || *size <= 0.0f)
			return std::nullopt;

		tag.delta.size = *size;
	} else if (name == "weight") {
		const auto weight = parseNumber(value);

		if (!weight || *weight < 1.0f || *weight > 1000.0f)
			return std::nullopt;

		tag.delta.weight = int(*weight);
	} else if (name == "font") {
		if (value.empty())
			return std::nullopt;

		tag.delta.family = value;
	} else if (name == "outline") {
		tag.delta.outline = true;

		if (!value.empty()) {
			const auto color = parseColor(value);

			if (!color)
				return std::nullopt;

			tag.delta.outlineColor = *color;
		}
	} else {
		return std::nullopt;
	}

	return tag;
}

} // namespace

std::optional<Rgba> parseColor(const std::string &text)
{
	std::string s = lower(text);

	s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); }), s.end());

	if (s.empty())
		return std::nullopt;

	if (s[0] == '#') {
		std::string hex = s.substr(1);

		if (!std::all_of(hex.begin(), hex.end(), [](unsigned char c) { return std::isxdigit(c); }))
			return std::nullopt;

		if (hex.size() == 3 || hex.size() == 4) {
			std::string expanded;

			for (char c : hex) {
				expanded += c;
				expanded += c;
			}

			hex = expanded;
		}

		if (hex.size() != 6 && hex.size() != 8)
			return std::nullopt;

		auto channel = [&](size_t i) {
			return float(std::strtoul(hex.substr(i * 2, 2).c_str(), nullptr, 16)) / 255.0f;
		};

		return Rgba{channel(0), channel(1), channel(2), hex.size() == 8 ? channel(3) : 1.0f};
	}

	struct Named {
		const char *name;
		uint32_t rgb;
	};

	static const Named kNames[] = {
		{"black", 0x000000},  {"white", 0xFFFFFF},   {"red", 0xFF0000},    {"green", 0x008000},
		{"lime", 0x00FF00},   {"blue", 0x0000FF},    {"yellow", 0xFFFF00}, {"cyan", 0x00FFFF},
		{"aqua", 0x00FFFF},   {"magenta", 0xFF00FF}, {"fuchsia", 0xFF00FF}, {"orange", 0xFFA500},
		{"purple", 0x800080}, {"pink", 0xFFC0CB},    {"gray", 0x808080},   {"grey", 0x808080},
		{"silver", 0xC0C0C0}, {"gold", 0xFFD700},    {"navy", 0x000080},   {"teal", 0x008080},
		{"maroon", 0x800000}, {"olive", 0x808000},   {"brown", 0xA52A2A},  {"violet", 0xEE82EE},
	};

	for (const Named &n : kNames) {
		if (s == n.name)
			return Rgba{float((n.rgb >> 16) & 0xFF) / 255.0f, float((n.rgb >> 8) & 0xFF) / 255.0f,
				    float(n.rgb & 0xFF) / 255.0f, 1.0f};
	}

	return std::nullopt;
}

std::vector<Block> parseMarkup(const std::string &markup)
{
	std::vector<Block> blocks(1);
	std::vector<OpenTag> stack;

	auto currentStyle = [&]() {
		StyleOverride style;

		for (const OpenTag &tag : stack)
			style.merge(tag.delta);

		return style;
	};

	auto emit = [&](const std::string &text) {
		if (text.empty())
			return;

		Block &block = blocks.back();
		const StyleOverride style = currentStyle();

		if (!block.runs.empty() && block.runs.back().style == style)
			block.runs.back().text += text;
		else
			block.runs.push_back(Run{text, style});
	};

	std::string pending;
	size_t i = 0;

	while (i < markup.size()) {
		const char c = markup[i];

		if (c == '\r') {
			i++;
			continue;
		}

		if (c == '\n') {
			emit(pending);
			pending.clear();

			// An empty line still carries the style that was open, so it has the
			// same height as its neighbours.
			if (blocks.back().runs.empty())
				blocks.back().runs.push_back(Run{std::string(), currentStyle()});

			blocks.emplace_back();
			i++;
			continue;
		}

		if (c != '[') {
			pending += c;
			i++;
			continue;
		}

		if (i + 1 < markup.size() && markup[i + 1] == '[') {
			pending += '[';
			i += 2;
			continue;
		}

		const size_t close = markup.find(']', i + 1);
		const size_t newline = markup.find('\n', i + 1);

		if (close == std::string::npos || (newline != std::string::npos && newline < close)) {
			pending += c;
			i++;
			continue;
		}

		const std::string body = markup.substr(i + 1, close - i - 1);

		if (!body.empty() && body[0] == '/') {
			const std::string name = lower(body.substr(1));

			auto it = stack.end();

			if (name.empty()) {
				if (!stack.empty())
					it = stack.end() - 1;
			} else {
				for (auto s = stack.rbegin(); s != stack.rend(); ++s) {
					if (s->name == name) {
						it = std::next(s).base();
						break;
					}
				}
			}

			if (it == stack.end()) {
				// A closing tag with nothing to close is shown as typed.
				pending += markup.substr(i, close - i + 1);
			} else {
				emit(pending);
				pending.clear();
				stack.erase(it);
			}

			i = close + 1;
			continue;
		}

		const auto tag = parseTag(body);

		if (!tag) {
			pending += markup.substr(i, close - i + 1);
			i = close + 1;
			continue;
		}

		emit(pending);
		pending.clear();
		stack.push_back(*tag);

		i = close + 1;
	}

	emit(pending);

	if (blocks.back().runs.empty())
		blocks.back().runs.push_back(Run{std::string(), currentStyle()});

	return blocks;
}

} // namespace slugged
