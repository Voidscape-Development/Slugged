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

#include "../text/layout.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "slughorn/slughorn.hpp"

namespace slugged {

// Builds and owns the slughorn Atlas for a source.
//
// slughorn's Atlas is immutable once built: shapes go in with addShape(), then
// build() packs them into the curve and band textures and the atlas is sealed.
// So this class tracks which shapes the current text needs and rebuilds the
// whole atlas whenever that set grows. That is cheaper than it sounds, because
// the set is derived from the document's full text rather than from what is
// currently visible -- a typewriter reveal or a scrolling ticker animates
// through glyphs that were all registered up front, and never triggers a
// rebuild mid-animation.
class AtlasCache {
public:
	AtlasCache();
	~AtlasCache();

	// Registers everything `glyphs` needs, rebuilding if the shape set grew.
	// Returns true when a rebuild happened, meaning the GPU textures are stale
	// and must be re-uploaded.
	bool ensure(const std::vector<PositionedGlyph> &glyphs);

	const slughorn::Atlas *atlas() const { return _atlas.get(); }

	// Bumped on every rebuild, so the renderer can tell whether its uploaded
	// textures still match.
	uint64_t generation() const { return _generation; }

	// Atlas key for a glyph's filled outline. The face's slot becomes
	// slughorn's 8-bit key mask and the glyph index the 21-bit value, so two
	// faces contributing the same glyph index stay distinct.
	static slughorn::Key fillKey(uint8_t slot, uint32_t glyphIndex);

	// Atlas key for a stroked copy of a glyph at a given em-space width.
	// Strokes are separate shapes, so they need their own namespace; a string
	// key keeps them clearly apart from fills without eating mask bits.
	static slughorn::Key outlineKey(uint8_t slot, uint32_t glyphIndex, float widthEm);

	// Stroke widths are bucketed so that dragging the outline slider does not
	// generate a distinct shape per pixel of travel.
	static float bucketWidth(float widthEm);

private:
	struct Request {
		uint8_t slot = 0;
		uint32_t glyphIndex = 0;
		FontFace *face = nullptr;

		// 0 for a fill, otherwise the bucketed stroke width in em units.
		float outlineWidthEm = 0.0f;

		LineJoin join = LineJoin::Round;
		LineCap cap = LineCap::Round;
		float miterLimit = 4.0f;

		bool operator==(const Request &o) const
		{
			return slot == o.slot && glyphIndex == o.glyphIndex && outlineWidthEm == o.outlineWidthEm;
		}
	};

	struct RequestHash {
		size_t operator()(const Request &r) const
		{
			size_t h = std::hash<uint32_t>{}(r.glyphIndex);

			h = h * 31 + std::hash<uint32_t>{}(r.slot);
			h = h * 31 + std::hash<float>{}(r.outlineWidthEm);

			return h;
		}
	};

	bool rebuild(const std::unordered_set<Request, RequestHash> &requests);
	bool addFill(const Request &req);
	bool addOutline(const Request &req);

	std::unique_ptr<slughorn::Atlas> _atlas;
	std::unordered_set<Request, RequestHash> _registered;

	uint64_t _generation = 0;
};

} // namespace slugged
