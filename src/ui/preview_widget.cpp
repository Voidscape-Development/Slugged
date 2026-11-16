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

#include <QCoreApplication>
#include <QFocusEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimerEvent>
#include <QWindow>

#if defined(__linux__) || defined(__FreeBSD__)
#include <QGuiApplication>
#endif

namespace slugged {

namespace {

// The source that is actually drawn: an overlay filter has no size or picture
// of its own, so it is shown the way the scene shows it, over what it sits on.
obs_source_t *shownSource(obs_source_t *source)
{
	if (source && obs_source_get_type(source) == OBS_SOURCE_TYPE_FILTER) {
		if (obs_source_t *parent = obs_filter_get_parent(source))
			return parent;
	}

	return source;
}

void fillRect(gs_effect_t *solid, gs_eparam_t *color, float x, float y, float w, float h, float r, float g, float b,
	      float a)
{
	if (w <= 0.0f || h <= 0.0f)
		return;

	vec4 value;
	vec4_set(&value, r, g, b, a);
	gs_effect_set_vec4(color, &value);

	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	gs_matrix_scale3f(w, h, 1.0f);

	while (gs_effect_loop(solid, "Solid"))
		gs_draw_sprite(nullptr, 0, 1, 1);

	gs_matrix_pop();
}

} // namespace

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

void PreviewWidget::detach()
{
	// The display's own lock makes removing the callback safe from any
	// thread; once it returns, no draw can be touching the source.
	if (_display)
		obs_display_remove_draw_callback(_display, drawCallback, this);

	_source = nullptr;
}

void PreviewWidget::setScale(float scale)
{
	_scale = std::max(0.0f, scale);
}

void PreviewWidget::setEditable(bool editable)
{
	if (_editable == editable)
		return;

	_editable = editable;

	setFocusPolicy(editable ? Qt::StrongFocus : Qt::NoFocus);
	setAttribute(Qt::WA_InputMethodEnabled, editable);
	setCursor(editable ? Qt::IBeamCursor : Qt::ArrowCursor);

	if (!editable)
		clearFocus();
}

void PreviewWidget::setOverlay(const Overlay &overlay)
{
	std::lock_guard<std::mutex> lock(_overlayMutex);

	_overlay = overlay;
}

void PreviewWidget::showCaretNow()
{
	{
		std::lock_guard<std::mutex> lock(_overlayMutex);

		_blinkOn = true;
	}

	if (_blinkTimer) {
		killTimer(_blinkTimer);
		_blinkTimer = startTimer(530);
	}
}

void PreviewWidget::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);

	if (_display || _tried)
		return;

	createDisplay();

	_tried = true;

	emit displayResolved(_display != nullptr);
}

void PreviewWidget::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);

	const qreal ratio = devicePixelRatioF();

	_deviceRatio = float(ratio);

	if (!_display)
		return;

	obs_display_resize(_display, uint32_t(width() * ratio), uint32_t(height() * ratio));
}

// ---- input -------------------------------------------------------------------

PreviewWidget::View PreviewWidget::viewFor(obs_source_t *shown, uint32_t cx, uint32_t cy, float scale)
{
	const float sourceWidth = float(std::max(1u, obs_source_get_width(shown)));
	const float sourceHeight = float(std::max(1u, obs_source_get_height(shown)));

	View view;

	// Fitting leaves a margin, so the text's own edge and the widget's are
	// never the same line; and it never blows a small source up past 1:1.
	const float fit = std::min({float(cx) * 0.9f / sourceWidth, float(cy) * 0.9f / sourceHeight, 1.0f});

	view.scale = scale > 0.0f ? scale : std::max(fit, 0.01f);
	view.offsetX = std::round((float(cx) - sourceWidth * view.scale) * 0.5f);
	view.offsetY = std::round((float(cy) - sourceHeight * view.scale) * 0.5f);

	return view;
}

QPointF PreviewWidget::toSource(const QPointF &widgetPoint) const
{
	obs_source_t *shown = shownSource(_source);

	if (!shown)
		return {};

	const qreal ratio = devicePixelRatioF();
	const View view = viewFor(shown, uint32_t(width() * ratio), uint32_t(height() * ratio), _scale * float(ratio));

	return QPointF((widgetPoint.x() * ratio - view.offsetX) / view.scale,
		       (widgetPoint.y() * ratio - view.offsetY) / view.scale);
}

void PreviewWidget::mousePressEvent(QMouseEvent *event)
{
	if (!_editable || event->button() != Qt::LeftButton) {
		QWidget::mousePressEvent(event);
		return;
	}

	setFocus(Qt::MouseFocusReason);

	_dragging = true;

	emit pressed(toSource(event->position()), event->modifiers().testFlag(Qt::ShiftModifier));
}

void PreviewWidget::mouseMoveEvent(QMouseEvent *event)
{
	if (_editable && _dragging)
		emit dragged(toSource(event->position()));
	else
		QWidget::mouseMoveEvent(event);
}

void PreviewWidget::mouseReleaseEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton)
		_dragging = false;

	QWidget::mouseReleaseEvent(event);
}

void PreviewWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
	if (_editable && event->button() == Qt::LeftButton)
		emit doubleClicked(toSource(event->position()));
	else
		QWidget::mouseDoubleClickEvent(event);
}

void PreviewWidget::keyPressEvent(QKeyEvent *event)
{
	if (_editable && _inputTarget)
		QCoreApplication::sendEvent(_inputTarget, event);
	else
		QWidget::keyPressEvent(event);
}

void PreviewWidget::keyReleaseEvent(QKeyEvent *event)
{
	if (_editable && _inputTarget)
		QCoreApplication::sendEvent(_inputTarget, event);
	else
		QWidget::keyReleaseEvent(event);
}

void PreviewWidget::inputMethodEvent(QInputMethodEvent *event)
{
	if (_editable && _inputTarget)
		QCoreApplication::sendEvent(_inputTarget, event);
	else
		QWidget::inputMethodEvent(event);
}

QVariant PreviewWidget::inputMethodQuery(Qt::InputMethodQuery query) const
{
	if (!_editable || !_inputTarget)
		return QWidget::inputMethodQuery(query);

	// The text lives in the input target; only where the caret is on screen
	// is this widget's to answer, so a candidate window opens next to it.
	if (query == Qt::ImCursorRectangle) {
		obs_source_t *shown = shownSource(_source);

		if (!shown)
			return QRect();

		EditMap::Rect caret;

		{
			auto *self = const_cast<PreviewWidget *>(this);
			std::lock_guard<std::mutex> lock(self->_overlayMutex);

			caret = _overlay.caret;
		}

		const qreal ratio = devicePixelRatioF();
		const View view =
			viewFor(shown, uint32_t(width() * ratio), uint32_t(height() * ratio), _scale * float(ratio));

		return QRectF((caret.x * view.scale + view.offsetX) / ratio,
			      (caret.y * view.scale + view.offsetY) / ratio, 1.0, caret.height * view.scale / ratio)
			.toRect();
	}

	return _inputTarget->inputMethodQuery(query);
}

void PreviewWidget::focusInEvent(QFocusEvent *event)
{
	QWidget::focusInEvent(event);

	{
		std::lock_guard<std::mutex> lock(_overlayMutex);

		_focused = true;
		_blinkOn = true;
	}

	if (!_blinkTimer)
		_blinkTimer = startTimer(530);

	emit focusChanged(true);
}

void PreviewWidget::focusOutEvent(QFocusEvent *event)
{
	QWidget::focusOutEvent(event);

	{
		std::lock_guard<std::mutex> lock(_overlayMutex);

		_focused = false;
	}

	if (_blinkTimer) {
		killTimer(_blinkTimer);
		_blinkTimer = 0;
	}

	emit focusChanged(false);
}

void PreviewWidget::timerEvent(QTimerEvent *event)
{
	if (event->timerId() != _blinkTimer) {
		QWidget::timerEvent(event);
		return;
	}

	std::lock_guard<std::mutex> lock(_overlayMutex);

	_blinkOn = !_blinkOn;
}

// ---- display -----------------------------------------------------------------

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

	_deviceRatio = float(ratio);

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

	_display = obs_display_create(&init, 0x1A1615);

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

	obs_source_t *shown = shownSource(self->_source);

	const float sourceWidth = float(std::max(1u, obs_source_get_width(shown)));
	const float sourceHeight = float(std::max(1u, obs_source_get_height(shown)));

	// The UI thread asks for a scale in widget pixels; this surface is in
	// device pixels, and the two differ on a high-DPI screen.
	const View view = viewFor(shown, cx, cy, self->_scale * self->_deviceRatio);

	Overlay overlay;
	bool caret = false;
	bool editable = self->_editable;

	{
		std::lock_guard<std::mutex> lock(self->_overlayMutex);

		overlay = self->_overlay;
		caret = overlay.caretVisible && self->_focused && self->_blinkOn;
	}

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = solid ? gs_effect_get_param_by_name(solid, "color") : nullptr;

	gs_projection_push();
	gs_viewport_push();

	// Work in source pixels, placed where the view puts them, so everything
	// below is drawn in the same space the edit map is in.
	gs_set_viewport(0, 0, int(cx), int(cy));
	gs_ortho(-view.offsetX / view.scale, (float(cx) - view.offsetX) / view.scale, -view.offsetY / view.scale,
		 (float(cy) - view.offsetY) / view.scale, -100.0f, 100.0f);

	// One screen pixel, in source pixels.
	const float px = 1.0f / view.scale;

	if (color) {
		// A slightly lighter ground under the source shows where its box is,
		// and makes partial alpha readable.
		// (A source with nothing to show has no box worth marking.)
		if (sourceWidth > 1.0f && sourceHeight > 1.0f)
			fillRect(solid, color, 0.0f, 0.0f, sourceWidth, sourceHeight, 0.125f, 0.13f, 0.15f, 1.0f);

		for (const EditMap::Rect &r : overlay.selection)
			fillRect(solid, color, r.x, r.y, r.width, r.height, 0.24f, 0.29f, 0.48f, 1.0f);
	}

	obs_source_video_render(shown);

	if (color) {
		gs_blend_state_push();
		gs_enable_blending(true);
		gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

		if (editable) {
			// The source's own bounds.
			const float t = px;

			fillRect(solid, color, -t, -t, sourceWidth + 2 * t, t, 0.54f, 0.48f, 0.24f, 0.9f);
			fillRect(solid, color, -t, sourceHeight, sourceWidth + 2 * t, t, 0.54f, 0.48f, 0.24f, 0.9f);
			fillRect(solid, color, -t, 0.0f, t, sourceHeight, 0.54f, 0.48f, 0.24f, 0.9f);
			fillRect(solid, color, sourceWidth, 0.0f, t, sourceHeight, 0.54f, 0.48f, 0.24f, 0.9f);
		}

		// Live values carry a dotted underline: they are text the source
		// writes itself, not text that was typed.
		for (const EditMap::Rect &r : overlay.tokens) {
			const float dot = 3.0f * px;

			for (float x = r.x; x < r.x + r.width; x += dot * 2.0f)
				fillRect(solid, color, x, r.y + r.height - 3.0f * px, std::min(dot, r.x + r.width - x),
					 2.0f * px, 0.64f, 0.66f, 0.71f, 0.9f);
		}

		if (caret)
			fillRect(solid, color, overlay.caret.x - px, overlay.caret.y, 2.0f * px, overlay.caret.height,
				 0.96f, 0.78f, 0.29f, 1.0f);

		gs_blend_state_pop();
	}

	gs_viewport_pop();
	gs_projection_pop();
}

} // namespace slugged
