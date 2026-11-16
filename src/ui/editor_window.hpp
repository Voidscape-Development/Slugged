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
#include "../text/edit_map.hpp"

#include <QMainWindow>
#include <QSyntaxHighlighter>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include <obs.h>

class QAction;
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
class QStackedWidget;
class QTimer;
class QWidget;

namespace slugged {

namespace ui {
class Section;
class Segmented;
class Switch;
} // namespace ui

class ColorButton;
class EditorWindow;
class InsertPanel;
class PreviewWidget;
class StartPage;
class StylesPanel;

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

// The editor for one Slugged source.
//
// The window is built around three ideas:
//
//   - You type where the text is. The preview is a live OBS render of the
//     source itself, and it is also where the caret is: clicks and keys on it
//     edit the text. (Where that cannot work -- no live preview, scrolling
//     text, markup, text from a file -- a plain text box takes over.)
//   - It always says what a control will change. With text selected, style
//     controls apply to the selection; with nothing selected they set the
//     source's base style, which reaches every character that was not styled
//     on its own. The panel's header names which of the two it is.
//   - One panel, folded. Text, Effects, Layout, Motion and More are sections
//     of a single column; a folded section shows what it is set to.
//
// Every change applies immediately and is undoable from OBS's Edit menu.
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

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;
	void closeEvent(QCloseEvent *event) override;

private:
	enum SidePage { SideInspector = 0, SideInsert = 1, SideStyles = 2 };

	// ---- construction ----
	void buildUi();
	QWidget *buildTopBar();
	QWidget *buildCanvasColumn();
	QWidget *buildContextBar();
	QWidget *buildBottomBar();
	QWidget *buildInspector();
	void buildTextSection();
	void buildEffectsSection();
	void buildLayoutSection();
	void buildMotionSection();
	void buildMoreSection();

	void rebuildAxisControls();
	void populateFamilies();

	// ---- state to screen ----

	// Refreshes every control from the document without feeding back.
	void syncControls();
	void syncScope(bool selection, size_t start, size_t end);
	void syncVisibility();
	void syncSummaries();

	// Decides where the text is typed -- on the preview or in the box -- and
	// says why when it is the box.
	void syncEditMode();

	// The caret, selection and live-value marks drawn on the preview.
	void syncOverlay();

	void showSide(SidePage page);
	void showStartPage(bool cancellable);
	void onTick();

	// ---- editing ----
	void onTextChanged();
	void onExternalUpdate();
	bool handleTextKey(class QKeyEvent *event);
	void insertAtCursor(const QString &text);

	// Applies a style change to the selection, or to the base style when
	// nothing is selected.
	void applyStyle(const StyleOverride &delta);

	// Applies a paragraph change to the paragraphs the selection touches, or
	// to every paragraph when nothing is selected.
	void applyParagraph(const ParagraphOverride &delta);

	// Applies a change to the document as a whole (layout, motion, ...).
	void applyDocument(const std::function<void(Document &)> &change);

	void clearFormatting();
	void nudgeSize(float factor);

	// Writes the document to the source as one undoable step.
	void commit();

	// Changes the source's settings directly, as one undoable step.
	void writeSettings(const std::function<void(obs_data_t *)> &change, bool reloadAfter);

	// Shows `motion` on the source without making it the setting, and puts
	// the real one back.
	void previewMotion(Motion motion);
	void endMotionPreview();

	void callProc(const char *name);
	void triggerHistory(bool redo);
	void renameSource();

	// UTF-8 byte offsets of the current selection, or false with none.
	bool selectionRange(size_t &start, size_t &end) const;
	size_t cursorOffset() const;
	void setCursorOffsets(size_t anchor, size_t position);

	static void sourceUpdated(void *data, calldata_t *call);

	obs_source_t *_source = nullptr;

	Document _document;
	bool _markup = false;
	bool _fileMode = false;
	VariableMap _variables;

	// The text box contents as of the last edit, for diffing the next one.
	QString _lastText;

	// The settings as this window last wrote them, so its own updates are not
	// mistaken for someone else's.
	std::string _lastCommitted;

	bool _updating = false;
	bool _canvasEditing = false;
	bool _startOffered = false;
	bool _previewingMotion = false;
	int _pendingPreview = -1;
	std::atomic<bool> _detached{false};

	std::shared_ptr<const EditMap> _editMap;

	// ---- shell ----
	QStackedWidget *_pages = nullptr;
	StartPage *_startPage = nullptr;
	QStackedWidget *_side = nullptr;
	InsertPanel *_insertPanel = nullptr;
	StylesPanel *_stylesPanel = nullptr;
	QTimer *_tick = nullptr;
	QTimer *_previewDelay = nullptr;

	QPushButton *_nameButton = nullptr;
	QPushButton *_undoButton = nullptr;
	QPushButton *_redoButton = nullptr;
	QPushButton *_stylesButton = nullptr;
	QPushButton *_insertButton = nullptr;
	QAction *_undoAction = nullptr;
	QAction *_redoAction = nullptr;

	// ---- canvas column ----
	PreviewWidget *_preview = nullptr;
	QLabel *_previewFallback = nullptr;
	QPlainTextEdit *_textEdit = nullptr;
	StyleHighlighter *_highlighter = nullptr;

	QStackedWidget *_context = nullptr;
	QLabel *_contextNote = nullptr;
	QPushButton *_hintDismiss = nullptr;
	QLabel *_quickSize = nullptr;
	QPushButton *_quickBold = nullptr;
	QPushButton *_quickItalic = nullptr;
	ColorButton *_quickColor = nullptr;

	QPushButton *_zoomButton = nullptr;
	QWidget *_timerBar = nullptr;
	QLabel *_timerValue = nullptr;
	QPushButton *_timerToggle = nullptr;
	QPushButton *_textBoxToggle = nullptr;

	// ---- inspector ----
	QWidget *_scopeHeader = nullptr;
	QLabel *_scopeTitle = nullptr;
	QLabel *_scopeDetail = nullptr;
	QPushButton *_clearFormatting = nullptr;

	ui::Section *_textSection = nullptr;
	ui::Section *_effectsSection = nullptr;
	ui::Section *_layoutSection = nullptr;
	ui::Section *_motionSection = nullptr;
	ui::Section *_moreSection = nullptr;
	QLabel *_selectionNote = nullptr;

	QComboBox *_fontFamily = nullptr;
	QSpinBox *_fontSize = nullptr;
	QComboBox *_fontWeight = nullptr;
	QPushButton *_fontBold = nullptr;
	QPushButton *_fontItalic = nullptr;
	QPushButton *_fontUnderline = nullptr;
	QPushButton *_fontStrikeout = nullptr;
	ColorButton *_fillColor = nullptr;
	ui::Segmented *_fillMode = nullptr;
	QWidget *_gradientBox = nullptr;
	ColorButton *_fillColor2 = nullptr;
	QComboBox *_gradientType = nullptr;
	QDoubleSpinBox *_gradientAngle = nullptr;
	QWidget *_gradientAngleRow = nullptr;
	QComboBox *_gradientSpan = nullptr;
	QLabel *_differsLegend = nullptr;

	ui::Switch *_outlineEnabled = nullptr;
	QWidget *_outlineBox = nullptr;
	QDoubleSpinBox *_outlineWidth = nullptr;
	ColorButton *_outlineColor = nullptr;
	QComboBox *_outlineJoin = nullptr;

	ui::Switch *_shadowEnabled = nullptr;
	QWidget *_shadowBox = nullptr;
	QDoubleSpinBox *_shadowX = nullptr;
	QDoubleSpinBox *_shadowY = nullptr;
	ColorButton *_shadowColor = nullptr;

	ui::Switch *_background = nullptr;
	QWidget *_backgroundBox = nullptr;
	ColorButton *_backgroundColor = nullptr;
	QDoubleSpinBox *_backgroundRadius = nullptr;
	QDoubleSpinBox *_padding = nullptr;

	ui::Segmented *_align = nullptr;
	QLabel *_alignHint = nullptr;
	ui::Segmented *_sizeMode = nullptr;
	QWidget *_boxControls = nullptr;
	QSpinBox *_boxWidth = nullptr;
	QSpinBox *_boxHeight = nullptr;
	QComboBox *_wrap = nullptr;
	ui::Segmented *_valign = nullptr;
	QDoubleSpinBox *_lineHeight = nullptr;

	std::map<int, QPushButton *> _motionTiles;
	QWidget *_motionDetails = nullptr;
	QComboBox *_motionTrigger = nullptr;
	QWidget *_motionTriggerRow = nullptr;
	QCheckBox *_motionChangedOnly = nullptr;
	QCheckBox *_motionExit = nullptr;
	QDoubleSpinBox *_motionLoopInterval = nullptr;
	QWidget *_motionLoopRow = nullptr;
	ui::Segmented *_motionOrder = nullptr;
	QComboBox *_motionDirection = nullptr;
	QWidget *_motionDirectionRow = nullptr;
	QSlider *_motionPace = nullptr;
	QWidget *_motionPaceRow = nullptr;
	QPushButton *_fineTuneToggle = nullptr;
	QWidget *_fineTune = nullptr;
	QDoubleSpinBox *_motionDuration = nullptr;
	QWidget *_motionDurationRow = nullptr;
	QDoubleSpinBox *_motionStagger = nullptr;
	QWidget *_motionStaggerRow = nullptr;
	QDoubleSpinBox *_motionParam = nullptr;
	QLabel *_motionParamLabel = nullptr;
	QWidget *_motionParamRow = nullptr;
	QDoubleSpinBox *_motionSpeed = nullptr;
	QWidget *_motionSpeedRow = nullptr;

	ui::Switch *_scrollEnabled = nullptr;
	QWidget *_scrollBox = nullptr;
	QDoubleSpinBox *_scrollX = nullptr;
	QDoubleSpinBox *_scrollY = nullptr;
	QCheckBox *_scrollLoop = nullptr;
	QDoubleSpinBox *_scrollGap = nullptr;
	QWidget *_scrollGapRow = nullptr;

	QDoubleSpinBox *_letterSpacing = nullptr;
	QDoubleSpinBox *_wordSpacing = nullptr;
	QCheckBox *_tabularDigits = nullptr;
	QLineEdit *_features = nullptr;
	QLineEdit *_language = nullptr;
	QWidget *_axisBox = nullptr;
	QFormLayout *_axisLayout = nullptr;
	std::map<std::string, QSlider *> _axisSliders;
	std::string _axisFontKey;

	QWidget *_wholeTextBox = nullptr;
	QSpinBox *_opacity = nullptr;
	QComboBox *_direction = nullptr;
	QDoubleSpinBox *_spaceBefore = nullptr;
	QDoubleSpinBox *_spaceAfter = nullptr;
	ui::Segmented *_textFrom = nullptr;
	QWidget *_fileBox = nullptr;
	QLineEdit *_filePath = nullptr;
	QCheckBox *_chatlog = nullptr;
	QSpinBox *_chatlogLines = nullptr;
	QCheckBox *_markupBox = nullptr;
	QLineEdit *_variablesFile = nullptr;
};

} // namespace slugged
