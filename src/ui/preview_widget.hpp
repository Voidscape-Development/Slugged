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

#include <QWidget>

#include <obs.h>

namespace slugged {

// A live OBS render of the source being edited.
//
// This embeds a real obs_display bound to the widget's native window and draws
// the actual source through it, which is what makes the editor genuinely
// WYSIWYG: the preview runs the same Slug shader, the same atlas and the same
// geometry as the scene does, rather than approximating it with Qt's own text
// rendering. Zooming the preview re-solves glyph coverage exactly as scaling the
// source in a scene would.
//
// obs_display needs a native window handle. That is available on Windows, macOS
// and X11; under Wayland it is not, so creation fails cleanly there and the
// editor falls back to a message telling the user to watch OBS's own preview.
class PreviewWidget : public QWidget {
	Q_OBJECT

public:
	explicit PreviewWidget(obs_source_t *source, QWidget *parent = nullptr);
	~PreviewWidget() override;

	// False when no native display could be created, so the caller can show a
	// fallback instead of an empty black box.
	bool isLive() const { return _display != nullptr; }

	// Zoom factor applied on top of the fit-to-widget scale.
	void setZoom(float zoom);
	float zoom() const { return _zoom; }

	// Draws a checkerboard behind the source so partial alpha is visible.
	void setShowCheckerboard(bool show) { _checkerboard = show; }

protected:
	void showEvent(QShowEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;

	// The display renders straight to the native window, so Qt must not paint
	// over it.
	QPaintEngine *paintEngine() const override { return nullptr; }

private:
	bool createDisplay();
	void destroyDisplay();

	static void drawCallback(void *data, uint32_t cx, uint32_t cy);

	obs_source_t *_source = nullptr;
	obs_display_t *_display = nullptr;

	float _zoom = 1.0f;
	bool _checkerboard = true;
};

} // namespace slugged
