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

#include "shape_cache.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "slughorn/slughorn.hpp"

namespace slugged {

// One shape a source needs in its atlas, under a key that names it uniquely
// (font instance, glyph, and which variant: fill, stroke width, colour layer).
struct ShapeRequest {
	std::string key;
	const slughorn::Atlas::ShapeInfo *info = nullptr;
};

// The slughorn Atlas for one source.
//
// A slughorn Atlas is immutable once built, so this rebuilds it whenever the
// set of shapes the text needs grows. Shapes that fall out of use are carried
// along for a while, so deleting a character and retyping it does not rebuild,
// but only while they are a minority: once the atlas holds more stale shapes
// than live ones it is rebuilt from what is needed now. That bound is what keeps
// dragging the outline-width slider, or a chat log scrolling through a lot of
// different text, from growing the atlas forever.
class AtlasCache {
public:
	// Makes every requested shape available. Returns true when the atlas was
	// rebuilt, meaning the GPU copy is stale.
	bool ensure(const std::vector<ShapeRequest> &required);

	const slughorn::Atlas *atlas() const { return _atlas.get(); }

	// The built atlas, shared so a finished frame can hand it to the graphics
	// thread while the next rebuild starts a new one.
	std::shared_ptr<const slughorn::Atlas> shared() const { return _atlas; }

	std::optional<slughorn::Atlas::Shape> shape(const std::string &key) const;

	// Bumped on every rebuild.
	uint64_t generation() const { return _generation; }

	size_t size() const { return _registered.size(); }

private:
	bool rebuild();

	std::shared_ptr<const slughorn::Atlas> _atlas;
	std::map<std::string, slughorn::Atlas::ShapeInfo> _registered;

	uint64_t _generation = 0;
};

// Shelf-packed RGBA texture holding a source's colour bitmap glyphs.
class ImageAtlas {
public:
	struct Slot {
		float u0 = 0.0f;
		float v0 = 0.0f;
		float u1 = 0.0f;
		float v1 = 0.0f;
	};

	struct Request {
		std::string key;
		const BitmapGlyph *bitmap = nullptr;
	};

	// Returns true when the texture contents changed.
	bool ensure(const std::vector<Request> &required);

	std::optional<Slot> slot(const std::string &key) const;

	std::shared_ptr<const std::vector<uint8_t>> pixels() const { return _pixels; }
	uint32_t width() const { return _width; }
	uint32_t height() const { return _height; }
	uint64_t generation() const { return _generation; }

private:
	std::map<std::string, Slot> _slots;
	std::shared_ptr<const std::vector<uint8_t>> _pixels;
	uint32_t _width = 0;
	uint32_t _height = 0;
	uint64_t _generation = 0;
};

} // namespace slugged
