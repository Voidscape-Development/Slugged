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

#include "../text/edit_map.hpp"

#include <QPointF>
#include <QWidget>

#include <atomic>
#include <mutex>
#include <vector>

#include <obs.h>

namespace slugged {

// A live OBS render of the source being edited, and the surface it is edited on.
//
// This embeds a real obs_display bound to the widget's native window and draws
// the actual source through it, which is what makes the editor genuinely
// WYSIWYG: the preview runs the same Slug shader, the same atlas and the same
// geometry as the scene does, rather than approximating it with Qt's own text
// rendering.
//
// Typing happens here too. The widget does not hold any text itself: it turns
// clicks into positions in the source's own pixel space and hands keys and
// input-method events to whatever widget does hold the text, then draws the
// caret and selection that widget reports, in the same pass as the source.
//
// obs_display needs a native window handle. That is available on Windows, macOS
// and X11; under Wayland it is not, so creation fails cleanly there and the
// editor falls back to a text box.
class PreviewWidget : public QWidget {
	Q_OBJECT

public:
	// What to draw over and under the source. All in source pixels.
	struct Overlay {
		bool caretVisible = false;
		EditMap::Rect caret;

		std::vector<EditMap::Rect> selection;

		// Live values, underlined so they read as different from typed text.
		std::vector<EditMap::Rect> tokens;
	};

	explicit PreviewWidget(obs_source_t *source, QWidget *parent = nullptr);
	~PreviewWidget() override;

	// False when no native display could be created, so the caller can show a
	// fallback instead of an empty black box.
	bool isLive() const { return _display != nullptr; }

	// True once creating the display has been tried, which happens the first
	// time the widget is shown. Until then "not live" only means "not yet".
	bool hasTried() const { return _tried; }

	// 0 fits the source to the widget; anything else is screen pixels per
	// source pixel.
	void setScale(float scale);
	float scale() const { return _scale; }

	// Stops drawing the source. Safe from any thread.
	void detach();

	// Whether clicks and keys edit text. Off, the widget is just a picture.
	void setEditable(bool editable);
	bool isEditable() const { return _editable; }

	// The widget that owns the text: keys and input-method events go to it.
	void setInputTarget(QWidget *target) { _inputTarget = target; }

	void setOverlay(const Overlay &overlay);

	// Restarts the caret's blink so it is solid right after it moves.
	void showCaretNow();

signals:
	// The first attempt to create the display has finished, one way or the other.
	void displayResolved(bool live);

	// A press or drag at a point in source pixels.
	void pressed(QPointF sourcePoint, bool extendSelection);
	void dragged(QPointF sourcePoint);
	void doubleClicked(QPointF sourcePoint);

	void focusChanged(bool focused);

protected:
	void showEvent(QShowEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;

	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;

	void keyPressEvent(QKeyEvent *event) override;
	void keyReleaseEvent(QKeyEvent *event) override;
	void inputMethodEvent(QInputMethodEvent *event) override;
	QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

	void focusInEvent(QFocusEvent *event) override;
	void focusOutEvent(QFocusEvent *event) override;
	void timerEvent(QTimerEvent *event) override;

	// The display renders straight to the native window, so Qt must not paint
	// over it.
	QPaintEngine *paintEngine() const override { return nullptr; }

private:
	struct View {
		float scale = 1.0f;
		float offsetX = 0.0f;
		float offsetY = 0.0f;
	};

	bool createDisplay();
	void destroyDisplay();

	// How the source is placed in a surface of (cx, cy) device pixels.
	static View viewFor(obs_source_t *shown, uint32_t cx, uint32_t cy, float scale);
	QPointF toSource(const QPointF &widgetPoint) const;

	static void drawCallback(void *data, uint32_t cx, uint32_t cy);

	obs_source_t *_source = nullptr;
	obs_display_t *_display = nullptr;
	QWidget *_inputTarget = nullptr;

	// Read by the graphics thread while it draws.
	std::atomic<float> _scale{0.0f};
	std::atomic<bool> _editable{false};
	std::atomic<float> _deviceRatio{1.0f};

	bool _dragging = false;
	bool _tried = false;
	int _blinkTimer = 0;

	// Shared with the graphics thread.
	std::mutex _overlayMutex;
	Overlay _overlay;
	bool _blinkOn = true;
	bool _focused = false;
};

} // namespace slugged
