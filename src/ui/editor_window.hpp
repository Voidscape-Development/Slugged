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
#include "../core/variables.hpp"

#include <QMainWindow>
#include <QSyntaxHighlighter>

#include <atomic>
#include <functional>
#include <map>
#include <string>

#include <obs.h>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QTableWidget;
class QWidget;

namespace slugged {

class PreviewWidget;
class ColorButton;
class EditorWindow;

// Shows each run's styling in the editor's text box: weight, slant, underline,
// strikeout, colour, outline and relative size. In markup mode it greys out the
// tags instead.
class StyleHighlighter : public QSyntaxHighlighter {
	Q_OBJECT

public:
	StyleHighlighter(EditorWindow *editor, QTextDocument *document);

protected:
	void highlightBlock(const QString &text) override;

private:
	EditorWindow *_editor;
};

// The WYSIWYG editor for one Slugged source.
//
// The preview is a live OBS render of the source itself. Every change applies
// immediately and is undoable from OBS's Edit menu.
//
// With text selected, style controls apply to the selection. With nothing
// selected they set the source's base style, which reaches every character that
// was not styled on its own -- the same thing the properties dialog edits.
// Each control changes only its own setting: changing the colour never resets
// the size of a word that was made bigger.
class EditorWindow : public QMainWindow {
	Q_OBJECT

	friend class StyleHighlighter;

public:
	explicit EditorWindow(obs_source_t *source, QWidget *parent = nullptr);
	~EditorWindow() override;

	obs_source_t *source() const { return _source; }

	// Reloads from the source's settings, keeping the cursor where it was.
	void reload();

	// Stops using the source: no more preview, signals or writes. Safe from
	// any thread; used when the source is destroyed before the window.
	void detach();

private:
	// ---- construction ----
	void buildUi();
	QWidget *buildTextPanel();
	QWidget *buildFontPanel();
	QWidget *buildFillPanel();
	QWidget *buildLayoutPanel();
	QWidget *buildMotionPanel();
	QWidget *buildVariablesPanel();

	void rebuildAxisControls();
	void populateFamilies();
	void syncMotionVisibility();

	// ---- editing ----
	void onTextChanged();
	void onVariablesChanged();
	void onExternalUpdate();

	// Applies a style change to the selection, or to the base style when
	// nothing is selected.
	void applyStyle(const StyleOverride &delta);

	// Applies a paragraph change to the paragraphs the selection touches, or
	// to every paragraph when nothing is selected.
	void applyParagraph(const ParagraphOverride &delta);

	// Applies a change to the document as a whole (layout, motion, ...).
	void applyDocument(const std::function<void(Document &)> &change);

	void clearFormatting();

	// Writes the document to the source as one undoable step.
	void commit();

	// Refreshes every control from the document without feeding back.
	void syncControls();

	// UTF-8 byte offsets of the current selection, or nullopt with none.
	bool selectionRange(size_t &start, size_t &end) const;
	size_t cursorOffset() const;

	static VariableMap tableToMap(QTableWidget *table);
	static void mapToTable(QTableWidget *table, const VariableMap &values);

	static void sourceUpdated(void *data, calldata_t *call);

	obs_source_t *_source = nullptr;

	Document _document;
	bool _markup = false;

	// The text box contents as of the last edit, for diffing the next one.
	QString _lastText;

	// The settings as this window last wrote them, so its own updates are not
	// mistaken for someone else's.
	std::string _lastCommitted;

	bool _updating = false;
	std::atomic<bool> _detached{false};

	PreviewWidget *_preview = nullptr;
	QPlainTextEdit *_textEdit = nullptr;
	StyleHighlighter *_highlighter = nullptr;
	QLabel *_scopeLabel = nullptr;
	QCheckBox *_markupBox = nullptr;
	QPushButton *_clearFormatting = nullptr;

	QComboBox *_fontFamily = nullptr;
	QSpinBox *_fontSize = nullptr;
	QComboBox *_fontWeight = nullptr;
	QCheckBox *_fontItalic = nullptr;
	QCheckBox *_fontUnderline = nullptr;
	QCheckBox *_fontStrikeout = nullptr;
	QCheckBox *_tabularDigits = nullptr;
	QLineEdit *_features = nullptr;
	QLineEdit *_language = nullptr;
	QFormLayout *_axisLayout = nullptr;
	std::map<std::string, QSlider *> _axisSliders;
	std::string _axisFontKey;

	QComboBox *_fillType = nullptr;
	ColorButton *_fillColor = nullptr;
	ColorButton *_fillColor2 = nullptr;
	QDoubleSpinBox *_gradientAngle = nullptr;
	QComboBox *_gradientSpan = nullptr;

	QCheckBox *_outlineEnabled = nullptr;
	QDoubleSpinBox *_outlineWidth = nullptr;
	ColorButton *_outlineColor = nullptr;
	QComboBox *_outlineJoin = nullptr;

	QCheckBox *_shadowEnabled = nullptr;
	QDoubleSpinBox *_shadowX = nullptr;
	QDoubleSpinBox *_shadowY = nullptr;
	ColorButton *_shadowColor = nullptr;

	QDoubleSpinBox *_letterSpacing = nullptr;
	QDoubleSpinBox *_wordSpacing = nullptr;

	QComboBox *_align = nullptr;
	QComboBox *_direction = nullptr;
	QDoubleSpinBox *_lineHeight = nullptr;
	QDoubleSpinBox *_spaceBefore = nullptr;
	QDoubleSpinBox *_spaceAfter = nullptr;

	QComboBox *_valign = nullptr;
	QComboBox *_wrap = nullptr;
	QCheckBox *_fixedSize = nullptr;
	QSpinBox *_boxWidth = nullptr;
	QSpinBox *_boxHeight = nullptr;
	QDoubleSpinBox *_padding = nullptr;
	QSpinBox *_opacity = nullptr;
	QCheckBox *_background = nullptr;
	ColorButton *_backgroundColor = nullptr;
	QDoubleSpinBox *_backgroundRadius = nullptr;

	QFormLayout *_motionForm = nullptr;
	QComboBox *_motion = nullptr;
	QComboBox *_motionOrder = nullptr;
	QComboBox *_motionDirection = nullptr;
	QDoubleSpinBox *_motionDuration = nullptr;
	QDoubleSpinBox *_motionStagger = nullptr;
	QDoubleSpinBox *_motionParam = nullptr;
	QLabel *_motionParamLabel = nullptr;
	QDoubleSpinBox *_motionSpeed = nullptr;
	QComboBox *_motionTrigger = nullptr;
	QComboBox *_motionScope = nullptr;
	QCheckBox *_motionExit = nullptr;
	QDoubleSpinBox *_motionLoopInterval = nullptr;

	QCheckBox *_scrollEnabled = nullptr;
	QDoubleSpinBox *_scrollX = nullptr;
	QDoubleSpinBox *_scrollY = nullptr;
	QCheckBox *_scrollLoop = nullptr;
	QDoubleSpinBox *_scrollGap = nullptr;

	QTableWidget *_sourceVariables = nullptr;
	QTableWidget *_globalVariables = nullptr;
};

} // namespace slugged
