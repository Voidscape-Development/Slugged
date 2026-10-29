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

#include "renderer.hpp"
#include "../util/log.hpp"

#include <cstring>

#include <graphics/vec2.h>
#include <graphics/vec4.h>

namespace slugged {

// A GPU texture shared between the current and the outgoing frame when both
// were built from the same atlas.
struct Renderer::Texture {
	gs_texture_t *texture = nullptr;
	uint64_t generation = 0;
	uint32_t width = 0;
	uint32_t height = 0;

	~Texture()
	{
		if (texture)
			gs_texture_destroy(texture);
	}
};

struct Renderer::Slot {
	gs_vertbuffer_t *vertices = nullptr;
	gs_indexbuffer_t *indices = nullptr;
	uint32_t indexCount = 0;

	gs_vertbuffer_t *backgroundVertices = nullptr;
	gs_indexbuffer_t *backgroundIndices = nullptr;
	uint32_t backgroundCount = 0;

	std::shared_ptr<Texture> curve;
	std::shared_ptr<Texture> band;
	std::shared_ptr<Texture> image;

	~Slot()
	{
		if (vertices)
			gs_vertexbuffer_destroy(vertices);

		if (indices)
			gs_indexbuffer_destroy(indices);

		if (backgroundVertices)
			gs_vertexbuffer_destroy(backgroundVertices);

		if (backgroundIndices)
			gs_indexbuffer_destroy(backgroundIndices);
	}
};

namespace {

// libobs takes ownership of everything hanging off gs_vb_data and frees it with
// bfree, so attribute arrays must come from bmalloc.
float *copyFloats(const std::vector<float> &src)
{
	if (src.empty())
		return nullptr;

	auto *dst = static_cast<float *>(bmalloc(src.size() * sizeof(float)));

	std::memcpy(dst, src.data(), src.size() * sizeof(float));

	return dst;
}

bool createBuffers(const GeometryBuffers &buffers, gs_vertbuffer_t *&vertices, gs_indexbuffer_t *&indices,
		   uint32_t &count)
{
	if (buffers.empty())
		return true;

	const size_t vertexCount = buffers.vertexCount();

	gs_vb_data *data = gs_vbdata_create();

	data->num = vertexCount;

	// Positions carry 4 floats per vertex precisely because vec3 is 16 bytes.
	data->points = static_cast<vec3 *>(bmalloc(vertexCount * sizeof(vec3)));
	std::memcpy(data->points, buffers.positions.data(), vertexCount * sizeof(vec3));

	data->num_tex = GeometryBuffers::AttributeCount;
	data->tvarray = static_cast<gs_tvertarray *>(bmalloc(data->num_tex * sizeof(gs_tvertarray)));

	for (size_t i = 0; i < GeometryBuffers::AttributeCount; i++) {
		data->tvarray[i].width = 4;
		data->tvarray[i].array = copyFloats(buffers.attributes[i]);
	}

	vertices = gs_vertexbuffer_create(data, 0);

	if (!vertices) {
		slugged_log(LOG_ERROR, "failed to create a vertex buffer for %zu vertices", vertexCount);
		return false;
	}

	auto *idx = static_cast<uint32_t *>(bmalloc(buffers.indices.size() * sizeof(uint32_t)));
	std::memcpy(idx, buffers.indices.data(), buffers.indices.size() * sizeof(uint32_t));

	indices = gs_indexbuffer_create(GS_UNSIGNED_LONG, idx, buffers.indices.size(), 0);

	if (!indices) {
		slugged_log(LOG_ERROR, "failed to create an index buffer");
		return false;
	}

	count = uint32_t(buffers.indices.size());

	return true;
}

std::shared_ptr<Renderer::Texture> makeTexture(uint32_t width, uint32_t height, gs_color_format format,
					       const uint8_t *bytes, uint64_t generation)
{
	auto texture = std::make_shared<Renderer::Texture>();

	texture->texture = gs_texture_create(width, height, format, 1, &bytes, 0);
	texture->generation = generation;
	texture->width = width;
	texture->height = height;

	if (!texture->texture)
		return nullptr;

	return texture;
}

} // namespace

Renderer::Renderer() = default;

Renderer::~Renderer()
{
	release();
}

bool Renderer::loadEffect()
{
	if (_effect)
		return true;

	char *path = obs_module_file("effects/slugged.effect");

	if (!path) {
		slugged_log(LOG_ERROR, "effects/slugged.effect is missing from the plugin data directory");
		return false;
	}

	char *errors = nullptr;

	// libobs caches effects by path, so every source shares one compile.
	_effect = gs_effect_create_from_file(path, &errors);

	if (!_effect)
		slugged_log(LOG_ERROR, "failed to compile slugged.effect: %s", errors ? errors : "unknown error");

	bfree(path);
	bfree(errors);

	return _effect != nullptr;
}

bool Renderer::upload(const FrameOutput &frame, bool keepPrevious)
{
	auto slot = std::make_unique<Slot>();

	// Reuse the current textures when the atlas has not changed since.
	const Slot *previous = _current.get();

	if (frame.atlas) {
		if (previous && previous->curve && previous->curve->generation == frame.atlasGeneration) {
			slot->curve = previous->curve;
			slot->band = previous->band;
		} else {
			const auto &curve = frame.atlas->getCurveTextureData();
			const auto &band = frame.atlas->getBandTextureData();

			if (!curve.empty() && !band.empty()) {
				slot->curve = makeTexture(curve.width, curve.height, GS_RGBA32F, curve.bytes.data(),
							  frame.atlasGeneration);

				// RGBA16UI uploaded as UNORM; the shader multiplies by 65535.
				slot->band = makeTexture(band.width, band.height, GS_RGBA16, band.bytes.data(),
							 frame.atlasGeneration);

				if (!slot->curve || !slot->band)
					slugged_log(LOG_ERROR,
						    "failed to create Slug data textures (%ux%u curve, %ux%u band)",
						    curve.width, curve.height, band.width, band.height);
			}
		}
	}

	if (frame.image && frame.imageWidth > 0 && frame.imageHeight > 0) {
		if (previous && previous->image && previous->image->generation == frame.imageGeneration)
			slot->image = previous->image;
		else
			slot->image = makeTexture(frame.imageWidth, frame.imageHeight, GS_RGBA, frame.image->data(),
						  frame.imageGeneration);
	}

	if (!createBuffers(frame.geometry, slot->vertices, slot->indices, slot->indexCount) ||
	    !createBuffers(frame.background, slot->backgroundVertices, slot->backgroundIndices, slot->backgroundCount))
		return false;

	if (keepPrevious && _current && _current->indexCount > 0)
		_outgoing = std::move(_current);

	_current = std::move(slot);

	return true;
}

bool Renderer::hasGeometry() const
{
	return _current && _current->indexCount > 0;
}

bool Renderer::hasOutgoing() const
{
	return _outgoing && _outgoing->indexCount > 0;
}

void Renderer::releaseOutgoing()
{
	_outgoing.reset();
}

void Renderer::release()
{
	_current.reset();
	_outgoing.reset();
}

void Renderer::drawBackground(const DrawParams &params)
{
	if (_current && _current->backgroundCount > 0)
		drawSlot(*_current, true, params);
}

void Renderer::draw(const DrawParams &params)
{
	if (_current && _current->indexCount > 0)
		drawSlot(*_current, false, params);
}

void Renderer::drawOutgoing(const DrawParams &params)
{
	if (_outgoing && _outgoing->indexCount > 0)
		drawSlot(*_outgoing, false, params);
}

void Renderer::drawSlot(const Slot &slot, bool background, const DrawParams &params)
{
	if (!_effect)
		return;

	gs_vertbuffer_t *vertices = background ? slot.backgroundVertices : slot.vertices;
	gs_indexbuffer_t *indices = background ? slot.backgroundIndices : slot.indices;
	const uint32_t count = background ? slot.backgroundCount : slot.indexCount;

	if (!vertices || !indices || !count)
		return;

	// Glyph quads need the atlas; the background does not.
	if (!background && (!slot.curve || !slot.band) && !slot.image)
		return;

	auto param = [this](const char *name) {
		return gs_effect_get_param_by_name(_effect, name);
	};

	gs_effect_set_texture(param("curve_tex"), slot.curve ? slot.curve->texture : nullptr);
	gs_effect_set_texture(param("band_tex"), slot.band ? slot.band->texture : nullptr);
	gs_effect_set_texture(param("image_tex"), slot.image ? slot.image->texture : nullptr);

	vec2 curveTexel;
	vec2 bandTexel;

	vec2_set(&curveTexel, slot.curve ? 1.0f / float(slot.curve->width) : 0.0f,
		 slot.curve ? 1.0f / float(slot.curve->height) : 0.0f);
	vec2_set(&bandTexel, slot.band ? 1.0f / float(slot.band->width) : 0.0f,
		 slot.band ? 1.0f / float(slot.band->height) : 0.0f);

	gs_effect_set_vec2(param("curve_texel"), &curveTexel);
	gs_effect_set_vec2(param("band_texel"), &bandTexel);
	gs_effect_set_float(param("band_width"), slot.band ? float(slot.band->width) : 1.0f);

	gs_effect_set_float(param("elapsed"), params.elapsed);
	gs_effect_set_float(param("opacity"), params.opacity);
	gs_effect_set_float(param("scope_changed"), params.scopeChanged ? 1.0f : 0.0f);
	gs_effect_set_float(param("reverse"), params.reverse ? 1.0f : 0.0f);

	vec2 offset;
	vec2_set(&offset, background ? 0.0f : params.offsetX, background ? 0.0f : params.offsetY);
	gs_effect_set_vec2(param("scroll_offset"), &offset);

	vec4 clip;

	if (params.clip)
		vec4_set(&clip, 0.0f, 0.0f, params.clipWidth, params.clipHeight);
	else
		vec4_set(&clip, -1e9f, -1e9f, 1e9f, 1e9f);

	gs_effect_set_vec4(param("clip_rect"), &clip);

	gs_blend_state_push();
	gs_reset_blend_state();

	// Straight-alpha source over destination, accumulating destination alpha
	// so the result composites correctly when drawn into a texture (as it is
	// once a filter is attached).
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_enable_depth_test(false);

	// Glyph quads are mirrored vertically to turn y-up em space into OBS's
	// y-down source space, which reverses their winding.
	const gs_cull_mode previousCull = gs_get_cull_mode();
	gs_set_cull_mode(GS_NEITHER);

	if (gs_technique_t *tech = gs_effect_get_technique(_effect, "Draw")) {
		gs_technique_begin(tech);
		gs_technique_begin_pass(tech, 0);

		gs_load_vertexbuffer(vertices);
		gs_load_indexbuffer(indices);

		gs_draw(GS_TRIS, 0, count);

		gs_technique_end_pass(tech);
		gs_technique_end(tech);
	}

	gs_load_indexbuffer(nullptr);
	gs_load_vertexbuffer(nullptr);

	gs_set_cull_mode(previousCull);
	gs_blend_state_pop();
}

} // namespace slugged
