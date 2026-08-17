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

#include "preview_widget.hpp"
#include "../util/log.hpp"

#include <algorithm>
#include <cmath>

#include <QResizeEvent>
#include <QShowEvent>
#include <QWindow>

#if defined(__linux__) || defined(__FreeBSD__)
#include <QGuiApplication>
#endif

namespace slugged {

PreviewWidget::PreviewWidget(obs_source_t *source, QWidget *parent) : QWidget(parent), _source(source)
{
	// The obs_display draws directly into this widget's native window, so Qt
	// must neither paint nor clear it.
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_NativeWindow);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);

	setMinimumSize(320, 180);
}

PreviewWidget::~PreviewWidget()
{
	destroyDisplay();
}

void PreviewWidget::setZoom(float zoom)
{
	_zoom = std::max(0.05f, zoom);

	if (_display)
		obs_display_update_color_space(_display);
}

void PreviewWidget::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);

	if (!_display)
		createDisplay();
}

void PreviewWidget::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);

	if (!_display)
		return;

	const qreal ratio = devicePixelRatioF();

	obs_display_resize(_display, uint32_t(width() * ratio), uint32_t(height() * ratio));
}

bool PreviewWidget::createDisplay()
{
	if (_display || !_source)
		return false;

	// Force the native window into existence before asking for its handle.
	QWindow *handle = windowHandle();

	if (!handle) {
		winId();
		handle = windowHandle();
	}

	if (!handle)
		return false;

	gs_init_data init = {};

	const qreal ratio = devicePixelRatioF();

	init.cx = uint32_t(std::max(1.0, width() * ratio));
	init.cy = uint32_t(std::max(1.0, height() * ratio));
	init.format = GS_BGRA;
	init.zsformat = GS_ZS_NONE;

#if defined(_WIN32)
	init.window.hwnd = reinterpret_cast<void *>(winId());
#elif defined(__APPLE__)
	init.window.view = reinterpret_cast<id>(winId());
#else
	// X11 only. Under Wayland there is no window id OBS can render into, so
	// the caller falls back to a message rather than showing a dead widget.
	auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();

	if (!x11) {
		slugged_log(LOG_INFO, "editor preview needs X11; falling back to the main OBS preview");
		return false;
	}

	init.window.id = uint32_t(winId());
	init.window.display = x11->display();
#endif

	_display = obs_display_create(&init, 0x000000);

	if (!_display) {
		slugged_log(LOG_WARNING, "could not create the editor preview display");
		return false;
	}

	obs_display_add_draw_callback(_display, drawCallback, this);

	return true;
}

void PreviewWidget::destroyDisplay()
{
	if (!_display)
		return;

	obs_display_remove_draw_callback(_display, drawCallback, this);
	obs_display_destroy(_display);

	_display = nullptr;
}

void PreviewWidget::drawCallback(void *data, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<PreviewWidget *>(data);

	if (!self || !self->_source)
		return;

	const uint32_t sourceWidth = std::max(1u, obs_source_get_width(self->_source));
	const uint32_t sourceHeight = std::max(1u, obs_source_get_height(self->_source));

	// Fit the source inside the widget, then apply the user's zoom on top.
	const float fit = std::min(float(cx) / float(sourceWidth), float(cy) / float(sourceHeight));
	const float scale = fit * self->_zoom;

	const int drawWidth = int(std::round(float(sourceWidth) * scale));
	const int drawHeight = int(std::round(float(sourceHeight) * scale));
	const int offsetX = (int(cx) - drawWidth) / 2;
	const int offsetY = (int(cy) - drawHeight) / 2;

	gs_projection_push();
	gs_viewport_push();

	gs_ortho(0.0f, float(sourceWidth), 0.0f, float(sourceHeight), -100.0f, 100.0f);
	gs_set_viewport(offsetX, offsetY, drawWidth, drawHeight);

	if (self->_checkerboard) {
		// A mid-grey ground behind the text makes partial alpha and outline
		// colours readable without needing a real checkerboard texture.
		gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);

		if (solid) {
			gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");

			vec4 grey;
			vec4_set(&grey, 0.16f, 0.16f, 0.18f, 1.0f);

			gs_effect_set_vec4(color, &grey);

			while (gs_effect_loop(solid, "Solid"))
				gs_draw_sprite(nullptr, 0, sourceWidth, sourceHeight);
		}
	}

	obs_source_video_render(self->_source);

	gs_viewport_pop();
	gs_projection_pop();
}

} // namespace slugged
