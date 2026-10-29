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

#include "frame.hpp"

#include <cstdint>
#include <memory>

#include <obs-module.h>

namespace slugged {

// Owns the GPU objects for one source and issues its draws.
//
// Every method must be called with a graphics context current: inside a render
// callback, or between obs_enter_graphics() and obs_leave_graphics().
//
// It holds two uploaded frames: the current one, and optionally the one before
// it, so old text can animate out while new text waits to animate in.
class Renderer {
public:
	struct DrawParams {
		float elapsed = 0.0f;
		float opacity = 1.0f;

		// Only characters marked as changed animate.
		bool scopeChanged = false;

		// Play the motion backwards.
		bool reverse = false;

		float offsetX = 0.0f;
		float offsetY = 0.0f;

		bool clip = false;
		float clipWidth = 0.0f;
		float clipHeight = 0.0f;
	};

	Renderer();
	~Renderer();

	Renderer(const Renderer &) = delete;
	Renderer &operator=(const Renderer &) = delete;

	bool loadEffect();

	// Uploads a frame. With `keepPrevious`, the frame being replaced stays
	// available to drawOutgoing() until releaseOutgoing().
	bool upload(const FrameOutput &frame, bool keepPrevious);

	void drawBackground(const DrawParams &params);
	void draw(const DrawParams &params);
	void drawOutgoing(const DrawParams &params);

	bool hasGeometry() const;
	bool hasOutgoing() const;

	void releaseOutgoing();
	void release();

	struct Texture;
	struct Slot;

private:
	void drawSlot(const Slot &slot, bool background, const DrawParams &params);

	gs_effect_t *_effect = nullptr;

	std::unique_ptr<Slot> _current;
	std::unique_ptr<Slot> _outgoing;
};

} // namespace slugged
