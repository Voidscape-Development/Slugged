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

#include "atlas_cache.hpp"
#include "../util/log.hpp"

#include <algorithm>

namespace slugged {

namespace {

// Stale shapes are carried while there are fewer than this many beyond twice
// the live count.
constexpr size_t kStaleAllowance = 128;

// Bitmap atlas limits. Colour emoji strikes are ~128 px, so this holds several
// hundred distinct emoji before anything is dropped.
constexpr uint32_t kImageAtlasWidth = 2048;
constexpr uint32_t kImageAtlasMaxHeight = 4096;
constexpr uint32_t kImagePadding = 2;

} // namespace

bool AtlasCache::ensure(const std::vector<ShapeRequest> &required)
{
	bool missing = !_atlas && !required.empty();

	for (const ShapeRequest &req : required) {
		if (req.info && !_registered.count(req.key)) {
			missing = true;
			break;
		}
	}

	if (required.empty()) {
		if (!_atlas)
			return false;

		_atlas.reset();
		_registered.clear();
		_generation++;

		return true;
	}

	if (!missing)
		return false;

	size_t live = 0;

	for (const ShapeRequest &req : required)
		if (req.info)
			live++;

	if (_registered.size() > live * 2 + kStaleAllowance)
		_registered.clear();

	for (const ShapeRequest &req : required)
		if (req.info)
			_registered[req.key] = *req.info;

	return rebuild();
}

bool AtlasCache::rebuild()
{
	auto atlas = std::make_shared<slughorn::Atlas>();

	for (const auto &[key, info] : _registered)
		atlas->addShape(slughorn::Key(key), info);

	try {
		atlas->build();
	} catch (const std::exception &e) {
		slugged_log(LOG_ERROR, "atlas build failed: %s", e.what());

		_atlas.reset();
		_registered.clear();
		_generation++;

		return true;
	}

	_atlas = std::move(atlas);
	_generation++;

	return true;
}

std::optional<slughorn::Atlas::Shape> AtlasCache::shape(const std::string &key) const
{
	if (!_atlas)
		return std::nullopt;

	return _atlas->getShape(slughorn::Key(key));
}

bool ImageAtlas::ensure(const std::vector<Request> &required)
{
	bool complete = true;

	for (const Request &req : required) {
		if (req.bitmap && !_slots.count(req.key)) {
			complete = false;
			break;
		}
	}

	if (complete && (!required.empty() || _slots.empty()))
		return false;

	// Repack from scratch: colour glyphs are few, and a fresh pack drops the
	// ones no longer shown.
	std::vector<const Request *> order;

	for (const Request &req : required)
		if (req.bitmap)
			order.push_back(&req);

	std::sort(order.begin(), order.end(),
		  [](const Request *a, const Request *b) { return a->bitmap->height > b->bitmap->height; });

	_slots.clear();

	struct Placed {
		const Request *req;
		uint32_t x, y;
	};

	std::vector<Placed> placed;

	uint32_t x = 0;
	uint32_t y = 0;
	uint32_t shelf = 0;

	for (const Request *req : order) {
		const uint32_t w = uint32_t(req->bitmap->width) + kImagePadding;
		const uint32_t h = uint32_t(req->bitmap->height) + kImagePadding;

		if (w > kImageAtlasWidth)
			continue;

		if (x + w > kImageAtlasWidth) {
			x = 0;
			y += shelf;
			shelf = 0;
		}

		if (y + h > kImageAtlasMaxHeight) {
			slugged_log(LOG_WARNING, "too many colour glyphs on one source; some will not draw");
			break;
		}

		placed.push_back({req, x, y});

		x += w;
		shelf = std::max(shelf, h);
	}

	_width = placed.empty() ? 0 : kImageAtlasWidth;
	_height = placed.empty() ? 0 : y + shelf;

	auto pixels = std::make_shared<std::vector<uint8_t>>(size_t(_width) * size_t(_height) * 4, 0);

	for (const Placed &p : placed) {
		const BitmapGlyph &bmp = *p.req->bitmap;

		for (int row = 0; row < bmp.height; row++) {
			std::copy_n(&bmp.rgba[size_t(row) * size_t(bmp.width) * 4], size_t(bmp.width) * 4,
				    &(*pixels)[(size_t(p.y + uint32_t(row)) * _width + p.x) * 4]);
		}

		Slot slot;

		slot.u0 = float(p.x) / float(_width);
		slot.v0 = float(p.y) / float(_height);
		slot.u1 = float(p.x + uint32_t(bmp.width)) / float(_width);
		slot.v1 = float(p.y + uint32_t(bmp.height)) / float(_height);

		_slots[p.req->key] = slot;
	}

	_pixels = std::move(pixels);
	_generation++;

	return true;
}

std::optional<ImageAtlas::Slot> ImageAtlas::slot(const std::string &key) const
{
	const auto it = _slots.find(key);

	if (it == _slots.end())
		return std::nullopt;

	return it->second;
}

} // namespace slugged
