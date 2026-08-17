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

#include "atlas_cache.hpp"
#include "geometry.hpp"

#include <cstdint>

#include <obs-module.h>

namespace slugged {

// Owns every GPU object for one source and issues the draw.
//
// Every method here must be called inside an obs_enter_graphics() /
// obs_leave_graphics() pair, or from a render callback where OBS already holds
// the graphics context.
class Renderer {
public:
	Renderer() = default;
	~Renderer();

	Renderer(const Renderer &) = delete;
	Renderer &operator=(const Renderer &) = delete;

	// Loads the shared effect. Safe to call repeatedly.
	bool loadEffect();

	// Uploads the atlas's curve and band textures if `cache` has been rebuilt
	// since the last upload.
	bool syncTextures(const AtlasCache &cache);

	// Replaces the vertex and index buffers.
	bool setGeometry(const GeometryBuffers &buffers);

	// Draws the current geometry. `elapsed` drives shader-side animation.
	void draw(float elapsed, float opacity);

	// Draws a plain filled rectangle, used for the document background.
	static void drawBackground(uint32_t width, uint32_t height, const Rgba &color);

	void releaseGeometry();
	void releaseTextures();

	bool hasGeometry() const { return _indexCount > 0; }

private:
	gs_effect_t *_effect = nullptr;

	gs_texture_t *_curveTex = nullptr;
	gs_texture_t *_bandTex = nullptr;

	gs_vertbuffer_t *_vertexBuffer = nullptr;
	gs_indexbuffer_t *_indexBuffer = nullptr;

	size_t _indexCount = 0;

	uint32_t _curveWidth = 0;
	uint32_t _curveHeight = 0;
	uint32_t _bandWidth = 0;
	uint32_t _bandHeight = 0;

	// Atlas generation currently resident on the GPU.
	uint64_t _uploadedGeneration = UINT64_MAX;
};

} // namespace slugged
