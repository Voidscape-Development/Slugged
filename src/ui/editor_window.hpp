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

#include "../core/document.hpp"

#include <QMainWindow>
#include <QPointer>

#include <functional>
#include <map>
#include <string>

#include <obs.h>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QWidget;

namespace slugged {

class PreviewWidget;
class ColorButton;

// The WYSIWYG editor for one Slugged source.
//
// The preview on the left is a live OBS render of the source itself, so what is
// shown is exactly what the scene shows. The inspector on the right edits the
// document; every change is applied to the source immediately rather than on an
// OK button, which is what makes it editable "on the fly".
//
// Styling applies to the current selection in the text area. With nothing
// selected it applies to the whole document, which is the behaviour that makes
// sense for a source whose text is usually a line or two.
class EditorWindow : public QMainWindow {
	Q_OBJECT

public:
	explicit EditorWindow(obs_source_t *source, QWidget *parent = nullptr);
	~EditorWindow() override;

	obs_source_t *source() const { return _source; }

	// Reloads from the source, for when settings changed elsewhere.
	void reload();

private slots:
	void onTextChanged();
	void onStyleChanged();
	void onLayoutChanged();
	void onMotionChanged();
	void onFontFamilyChanged();

private:
	void buildUi();
	QWidget *buildTextPanel();
	QWidget *buildFontPanel();
	QWidget *buildFillPanel();
	QWidget *buildLayoutPanel();
	QWidget *buildMotionPanel();

	void rebuildAxisControls();

	// Pushes the edited document into the source.
	void apply();

	// Refreshes the inspector from `_document` without emitting change signals.
	void syncControls();

	// Applies `fn` to every style in the current selection, or to the whole
	// document when nothing is selected.
	void applyToSelection(const std::function<void(Style &)> &fn);

	// The style shown in the inspector: the one at the cursor, falling back to
	// the document default.
	const Style &styleAtCursor() const;

	obs_source_t *_source = nullptr;
	Document _document;

	// Set while syncControls() is running so control signals do not feed back
	// into the document.
	bool _updating = false;

	PreviewWidget *_preview = nullptr;
	QPlainTextEdit *_textEdit = nullptr;

	QComboBox *_fontFamily = nullptr;
	QSpinBox *_fontSize = nullptr;
	QComboBox *_fontWeight = nullptr;
	QCheckBox *_fontItalic = nullptr;
	QFormLayout *_axisLayout = nullptr;
	std::map<std::string, QSlider *> _axisSliders;

	ColorButton *_fillColor = nullptr;
	QComboBox *_fillType = nullptr;
	ColorButton *_gradientEnd = nullptr;
	QDoubleSpinBox *_gradientAngle = nullptr;
	QCheckBox *_gradientPerGlyph = nullptr;

	QCheckBox *_outlineEnabled = nullptr;
	QDoubleSpinBox *_outlineWidth = nullptr;
	ColorButton *_outlineColor = nullptr;
	QComboBox *_outlineJoin = nullptr;

	QCheckBox *_shadowEnabled = nullptr;
	QDoubleSpinBox *_shadowX = nullptr;
	QDoubleSpinBox *_shadowY = nullptr;
	ColorButton *_shadowColor = nullptr;

	QDoubleSpinBox *_letterSpacing = nullptr;
	QDoubleSpinBox *_lineHeight = nullptr;

	QComboBox *_align = nullptr;
	QComboBox *_valign = nullptr;
	QComboBox *_wrap = nullptr;
	QCheckBox *_fixedSize = nullptr;
	QSpinBox *_boxWidth = nullptr;
	QSpinBox *_boxHeight = nullptr;
	QDoubleSpinBox *_padding = nullptr;
	QCheckBox *_background = nullptr;
	ColorButton *_backgroundColor = nullptr;

	QComboBox *_motion = nullptr;
	QComboBox *_motionOrder = nullptr;
	QDoubleSpinBox *_motionDuration = nullptr;
	QDoubleSpinBox *_motionStagger = nullptr;
	QDoubleSpinBox *_motionParam = nullptr;
	QCheckBox *_scrollEnabled = nullptr;
	QDoubleSpinBox *_scrollX = nullptr;
	QDoubleSpinBox *_scrollY = nullptr;
};

} // namespace slugged
