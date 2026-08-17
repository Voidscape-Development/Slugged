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

#include <graphics/vec4.h>

namespace slugged {

namespace {

// Copies a float attribute array into a bmalloc'd block. libobs takes ownership
// of everything hanging off gs_vb_data and frees it with bfree, so these cannot
// be plain new[] or std::vector storage.
float *copyFloats(const std::vector<float> &src)
{
	if (src.empty())
		return nullptr;

	float *dst = static_cast<float *>(bmalloc(src.size() * sizeof(float)));

	std::memcpy(dst, src.data(), src.size() * sizeof(float));

	return dst;
}

} // namespace

Renderer::~Renderer()
{
	// Effects loaded through obs_module are owned by the module, but textures
	// and buffers are ours. Callers destroy this inside a graphics context.
	releaseGeometry();
	releaseTextures();
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

	_effect = gs_effect_create_from_file(path, &errors);

	if (!_effect) {
		slugged_log(LOG_ERROR, "failed to compile slugged.effect: %s", errors ? errors : "unknown error");
	}

	bfree(path);
	bfree(errors);

	return _effect != nullptr;
}

void Renderer::releaseTextures()
{
	if (_curveTex) {
		gs_texture_destroy(_curveTex);
		_curveTex = nullptr;
	}

	if (_bandTex) {
		gs_texture_destroy(_bandTex);
		_bandTex = nullptr;
	}

	_uploadedGeneration = UINT64_MAX;
}

void Renderer::releaseGeometry()
{
	if (_vertexBuffer) {
		gs_vertexbuffer_destroy(_vertexBuffer);
		_vertexBuffer = nullptr;
	}

	if (_indexBuffer) {
		gs_indexbuffer_destroy(_indexBuffer);
		_indexBuffer = nullptr;
	}

	_indexCount = 0;
}

bool Renderer::syncTextures(const AtlasCache &cache)
{
	if (cache.generation() == _uploadedGeneration)
		return true;

	releaseTextures();

	const slughorn::Atlas *atlas = cache.atlas();

	if (!atlas)
		return false;

	const slughorn::Atlas::TextureData &curve = atlas->getCurveTextureData();
	const slughorn::Atlas::TextureData &band = atlas->getBandTextureData();

	if (curve.empty() || band.empty())
		return false;

	{
		const uint8_t *data = curve.bytes.data();

		_curveTex = gs_texture_create(curve.width, curve.height, GS_RGBA32F, 1, &data, 0);
		_curveWidth = curve.width;
		_curveHeight = curve.height;
	}

	{
		// slughorn hands this over as RGBA16UI. libobs has no UINT format at
		// all, so it is uploaded as GS_RGBA16 (UNORM) -- the bytes are
		// identical, only the interpretation differs, and the shader multiplies
		// by 65535 to recover the original integers exactly.
		const uint8_t *data = band.bytes.data();

		_bandTex = gs_texture_create(band.width, band.height, GS_RGBA16, 1, &data, 0);
		_bandWidth = band.width;
		_bandHeight = band.height;
	}

	if (!_curveTex || !_bandTex) {
		slugged_log(LOG_ERROR, "failed to create Slug data textures (%ux%u curve, %ux%u band)", curve.width,
			    curve.height, band.width, band.height);

		releaseTextures();

		return false;
	}

	_uploadedGeneration = cache.generation();

	return true;
}

bool Renderer::setGeometry(const GeometryBuffers &buffers)
{
	releaseGeometry();

	if (buffers.empty())
		return false;

	const size_t vertices = buffers.vertexCount();

	// gs_vb_data and everything it points at must come from bmalloc; libobs
	// frees the whole graph itself when the buffer is destroyed.
	gs_vb_data *data = gs_vbdata_create();

	data->num = vertices;
	data->points = static_cast<vec3 *>(bmalloc(vertices * sizeof(vec3)));

	// positions carries 4 floats per vertex precisely because vec3 is 16 bytes
	// wide, so this is a straight copy rather than a strided one.
	std::memcpy(data->points, buffers.positions.data(), vertices * sizeof(vec3));

	data->num_tex = 6;
	data->tvarray = static_cast<gs_tvertarray *>(bmalloc(data->num_tex * sizeof(gs_tvertarray)));

	const std::vector<float> *sources[6] = {
		&buffers.em, &buffers.bandXform, &buffers.shapeData, &buffers.color, &buffers.fx, &buffers.pivot,
	};

	for (size_t i = 0; i < 6; i++) {
		data->tvarray[i].width = 4;
		data->tvarray[i].array = copyFloats(*sources[i]);
	}

	_vertexBuffer = gs_vertexbuffer_create(data, 0);

	if (!_vertexBuffer) {
		slugged_log(LOG_ERROR, "failed to create vertex buffer for %zu vertices", vertices);
		return false;
	}

	uint32_t *indices = static_cast<uint32_t *>(bmalloc(buffers.indices.size() * sizeof(uint32_t)));

	std::memcpy(indices, buffers.indices.data(), buffers.indices.size() * sizeof(uint32_t));

	_indexBuffer = gs_indexbuffer_create(GS_UNSIGNED_LONG, indices, buffers.indices.size(), 0);

	if (!_indexBuffer) {
		slugged_log(LOG_ERROR, "failed to create index buffer");
		releaseGeometry();

		return false;
	}

	_indexCount = buffers.indices.size();

	return true;
}

void Renderer::draw(float elapsed, float opacity)
{
	if (!_effect || !_vertexBuffer || !_indexBuffer || !_curveTex || !_bandTex)
		return;

	gs_eparam_t *curveParam = gs_effect_get_param_by_name(_effect, "curve_tex");
	gs_eparam_t *bandParam = gs_effect_get_param_by_name(_effect, "band_tex");
	gs_eparam_t *curveTexel = gs_effect_get_param_by_name(_effect, "curve_texel");
	gs_eparam_t *bandTexel = gs_effect_get_param_by_name(_effect, "band_texel");
	gs_eparam_t *bandWidth = gs_effect_get_param_by_name(_effect, "band_width");
	gs_eparam_t *elapsedParam = gs_effect_get_param_by_name(_effect, "elapsed");
	gs_eparam_t *opacityParam = gs_effect_get_param_by_name(_effect, "opacity");

	gs_effect_set_texture(curveParam, _curveTex);
	gs_effect_set_texture(bandParam, _bandTex);

	vec2 curveTexelSize;
	vec2 bandTexelSize;

	vec2_set(&curveTexelSize, _curveWidth ? 1.0f / float(_curveWidth) : 0.0f,
		 _curveHeight ? 1.0f / float(_curveHeight) : 0.0f);
	vec2_set(&bandTexelSize, _bandWidth ? 1.0f / float(_bandWidth) : 0.0f,
		 _bandHeight ? 1.0f / float(_bandHeight) : 0.0f);

	gs_effect_set_vec2(curveTexel, &curveTexelSize);
	gs_effect_set_vec2(bandTexel, &bandTexelSize);
	gs_effect_set_float(bandWidth, float(_bandWidth));
	gs_effect_set_float(elapsedParam, elapsed);
	gs_effect_set_float(opacityParam, opacity);

	gs_blend_state_push();
	gs_reset_blend_state();

	// Straight-alpha source over destination, with the destination's alpha
	// accumulated so the source composites correctly when it is itself drawn
	// into a texture (which is what happens as soon as a filter is attached).
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	gs_enable_depth_test(false);

	// Glyph quads are mirrored vertically to convert Slug's y-up em space into
	// OBS's y-down source space, which reverses their winding order.
	const gs_cull_mode previousCull = gs_get_cull_mode();
	gs_set_cull_mode(GS_NEITHER);

	gs_technique_t *tech = gs_effect_get_technique(_effect, "Draw");

	if (tech) {
		gs_technique_begin(tech);
		gs_technique_begin_pass(tech, 0);

		gs_load_vertexbuffer(_vertexBuffer);
		gs_load_indexbuffer(_indexBuffer);

		gs_draw(GS_TRIS, 0, uint32_t(_indexCount));

		gs_technique_end_pass(tech);
		gs_technique_end(tech);
	}

	gs_load_indexbuffer(nullptr);
	gs_load_vertexbuffer(nullptr);

	gs_set_cull_mode(previousCull);
	gs_blend_state_pop();
}

void Renderer::drawBackground(uint32_t width, uint32_t height, const Rgba &color)
{
	if (!width || !height || color.a <= 0.0f)
		return;

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);

	if (!solid)
		return;

	gs_eparam_t *colorParam = gs_effect_get_param_by_name(solid, "color");

	vec4 rgba;
	vec4_set(&rgba, color.r, color.g, color.b, color.a);

	gs_effect_set_vec4(colorParam, &rgba);

	gs_blend_state_push();
	gs_reset_blend_state();
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	while (gs_effect_loop(solid, "Solid"))
		gs_draw_sprite(nullptr, 0, width, height);

	gs_blend_state_pop();
}

} // namespace slugged
