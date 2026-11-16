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

#include "editor_window.hpp"
#include "color_button.hpp"
#include "convert_dialog.hpp"
#include "editor_common.hpp"
#include "insert_panel.hpp"
#include "preview_widget.hpp"
#include "start_page.hpp"
#include "styles_panel.hpp"
#include "widgets.hpp"
#include "../obs/editor_bridge.hpp"
#include "../obs/saved_styles.hpp"
#include "../obs/settings.hpp"
#include "../obs/slugged_source.hpp"
#include "../obs/start_points.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFontInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QSpinBox>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTextBlock>
#include <QTimer>
#include <QVBoxLayout>

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

namespace slugged {

using namespace ui;

namespace {

// ---- undo ------------------------------------------------------------------

std::string undoPayload(obs_source_t *source, const std::string &settingsJson)
{
	obs_data_t *wrapper = obs_data_create();
	obs_data_t *settingsData = obs_data_create_from_json(settingsJson.c_str());

	obs_data_set_string(wrapper, "uuid", obs_source_get_uuid(source));
	obs_data_set_obj(wrapper, "settings", settingsData);

	const char *json = obs_data_get_json(wrapper);
	std::string out = json ? json : "";

	obs_data_release(settingsData);
	obs_data_release(wrapper);

	return out;
}

void applyUndoPayload(const char *payload)
{
	obs_data_t *wrapper = obs_data_create_from_json(payload);

	if (!wrapper)
		return;

	obs_data_t *settingsData = obs_data_get_obj(wrapper, "settings");
	obs_source_t *source = obs_get_source_by_uuid(obs_data_get_string(wrapper, "uuid"));

	if (source && settingsData)
		obs_source_reset_settings(source, settingsData);

	obs_source_release(source);
	obs_data_release(settingsData);
	obs_data_release(wrapper);
}

std::string settingsJson(obs_data_t *data)
{
	const char *json = obs_data_get_json(data);

	return json ? json : "";
}

// ---- preferences ---------------------------------------------------------------
//
// The one thing the editor remembers about its user rather than about a source:
// whether the "click the text and type" hint has been dismissed.

const char *const kPrefsFile = "editor.json";

bool hintDismissed()
{
	static int cached = -1;

	if (cached >= 0)
		return cached == 1;

	cached = 0;

	if (char *path = obs_module_config_path(kPrefsFile)) {
		if (obs_data_t *data = obs_data_create_from_json_file(path)) {
			cached = obs_data_get_bool(data, "hint_dismissed") ? 1 : 0;
			obs_data_release(data);
		}

		bfree(path);
	}

	return cached == 1;
}

void dismissHint()
{
	if (char *dir = obs_module_config_path(nullptr)) {
		os_mkdirs(dir);
		bfree(dir);
	}

	if (char *path = obs_module_config_path(kPrefsFile)) {
		obs_data_t *data = obs_data_create();

		obs_data_set_bool(data, "hint_dismissed", true);
		obs_data_save_json(data, path);
		obs_data_release(data);

		bfree(path);
	}
}

bool usesTimer(const QString &text)
{
	return text.contains(QStringLiteral("{timer")) || text.contains(QStringLiteral("{countdown"));
}

} // namespace

// ---------------------------------------------------------------------------
// Highlighting
// ---------------------------------------------------------------------------

StyleHighlighter::StyleHighlighter(EditorWindow *editor, QTextDocument *document)
	: QSyntaxHighlighter(document),
	  _editor(editor)
{
}

void StyleHighlighter::highlightBlock(const QString &text)
{
	if (_editor->_markup) {
		// Tags in a muted colour, so the text itself reads first.
		static const QRegularExpression tag(QStringLiteral("\\[/?[a-zA-Z]*(=[^\\]\\n]*)?\\]"));

		QTextCharFormat format;
		format.setForeground(QColor(140, 140, 150));

		for (auto it = tag.globalMatch(text); it.hasNext();) {
			const auto match = it.next();
			setFormat(int(match.capturedStart()), int(match.capturedLength()), format);
		}

		return;
	}

	const int index = currentBlock().blockNumber();
	const Document &doc = _editor->_document;

	if (index < 0 || size_t(index) >= doc.blocks.size())
		return;

	const Block &block = doc.blocks[size_t(index)];
	const float baseSize = std::max(doc.base.sizePx, 1.0f);
	const QFont font = _editor->_textEdit->font();

	int position = 0;

	for (const Run &run : block.runs) {
		const int length = int(QString::fromStdString(run.text).size());

		if (length == 0)
			continue;

		const Style style = doc.resolve(run);

		QTextCharFormat format;

		format.setFontWeight(std::clamp(style.font.weight, 100, 900));
		format.setFontItalic(style.font.italic);
		format.setFontUnderline(style.underline);
		format.setFontStrikeOut(style.strikeout);
		format.setForeground(toQColor(style.fill.color));

		if (!style.font.family.empty())
			format.setFontFamilies({QString::fromStdString(style.font.family)});

		// Relative size, kept within a range that still reads in a text box.
		const double scale = std::clamp(double(style.sizePx / baseSize), 0.7, 1.6);

		// A font sized in pixels (by a style sheet, say) reports no point
		// size of its own; QFontInfo resolves either kind.
		format.setFontPointSize(std::max(1.0, QFontInfo(font).pointSizeF() * scale));

		if (style.outline.enabled)
			format.setTextOutline(QPen(toQColor(style.outline.color), 0.6));

		setFormat(position, length, format);

		position += length;
	}
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

EditorWindow::EditorWindow(obs_source_t *source, QWidget *parent) : QMainWindow(parent), _source(source)
{
	setWindowTitle(tr_("Editor.Title") + " - " + QString::fromUtf8(obs_source_get_name(source)));
	setAttribute(Qt::WA_DeleteOnClose);
	resize(1240, 800);

	buildUi();

	setEditorAttached(_source, true);
	reload();

	signal_handler_connect(obs_source_get_signal_handler(_source), "update", sourceUpdated, this);

	_tick = new QTimer(this);
	_tick->setInterval(50);
	connect(_tick, &QTimer::timeout, this, &EditorWindow::onTick);
	_tick->start();

	// Hovering a motion shows it, after a moment, so sweeping the pointer
	// across the grid does not fire all nine.
	_previewDelay = new QTimer(this);
	_previewDelay->setSingleShot(true);
	_previewDelay->setInterval(350);
	connect(_previewDelay, &QTimer::timeout, this, [this]() {
		if (_pendingPreview >= 0)
			previewMotion(Motion(_pendingPreview));
	});
}

EditorWindow::~EditorWindow()
{
	detach();
}

void EditorWindow::detach()
{
	if (_detached.exchange(true))
		return;

	signal_handler_disconnect(obs_source_get_signal_handler(_source), "update", sourceUpdated, this);
	setEditorAttached(_source, false);

	if (_preview)
		_preview->detach();

	if (_insertPanel)
		_insertPanel->detach();
}

void EditorWindow::sourceUpdated(void *data, calldata_t *)
{
	// Any thread; the window lives on the UI thread.
	QPointer<EditorWindow> window = static_cast<EditorWindow *>(data);

	QMetaObject::invokeMethod(
		window,
		[window]() {
			if (window)
				window->onExternalUpdate();
		},
		Qt::QueuedConnection);
}

void EditorWindow::buildUi()
{
	setStyleSheet(ui::styleSheet());

	_pages = new QStackedWidget(this);

	// ---- the editor itself -------------------------------------------------
	auto *editorPage = new QWidget(_pages);
	editorPage->setObjectName("editorRoot");
	editorPage->setAttribute(Qt::WA_StyledBackground);

	auto *pageLayout = new QVBoxLayout(editorPage);
	pageLayout->setContentsMargins(0, 0, 0, 0);
	pageLayout->setSpacing(0);

	auto *body = new QHBoxLayout();
	body->setContentsMargins(0, 0, 0, 0);
	body->setSpacing(0);

	_side = new QStackedWidget(editorPage);
	_side->setFixedWidth(348);

	_insertPanel = new InsertPanel(_source, _side);
	_stylesPanel = new StylesPanel(_side);

	_side->addWidget(buildInspector());
	_side->addWidget(_insertPanel);
	_side->addWidget(_stylesPanel);

	body->addWidget(buildCanvasColumn(), 1);
	body->addWidget(_side);

	pageLayout->addWidget(buildTopBar());
	pageLayout->addLayout(body, 1);

	// ---- starting points ---------------------------------------------------
	_startPage = new StartPage(_pages);

	_pages->addWidget(editorPage);
	_pages->addWidget(_startPage);

	setCentralWidget(_pages);

	connect(_startPage, &StartPage::chosen, this, [this](const QString &id) {
		_pages->setCurrentIndex(0);

		writeSettings([&](obs_data_t *data) { applyStartPoint(data, id.toStdString()); }, true);

		if (id == QLatin1String("file")) {
			// The one starting point that needs something from the user
			// before it shows anything.
			_moreSection->setExpanded(true);
			_filePath->setFocus();
		} else {
			// Ready to type, with nothing selected: the panel should open
			// on the whole text, not on a selection nobody made.
			_textEdit->moveCursor(QTextCursor::End);

			if (_canvasEditing)
				_preview->setFocus();
			else
				_textEdit->setFocus();
		}
	});

	connect(_startPage, &StartPage::cancelled, this, [this]() { _pages->setCurrentIndex(0); });
	connect(_startPage, &StartPage::convertRequested, this, []() { showConvertDialog(); });

	connect(_insertPanel, &InsertPanel::closeRequested, this, [this]() { showSide(SideInspector); });
	connect(_insertPanel, &InsertPanel::insertRequested, this, [this](const QString &token) {
		insertAtCursor(token);
		showSide(SideInspector);
	});
	connect(_insertPanel, &InsertPanel::sourceVariablesChanged, this, [this](const VariableMap &values) {
		_variables = values;

		writeSettings(
			[&](obs_data_t *data) { settings::variablesToData(data, settings::kVariablesKey, values); },
			false);
	});

	connect(_stylesPanel, &StylesPanel::closeRequested, this, [this]() { showSide(SideInspector); });
	connect(_stylesPanel, &StylesPanel::saveRequested, this, [this](const QString &name, bool withMotion) {
		styles::put(styles::capture(_document, name.toStdString(), withMotion));
		_stylesPanel->refresh();
	});
	connect(_stylesPanel, &StylesPanel::applyRequested, this, [this](const QString &name) {
		SavedStyle style;

		if (!styles::find(name.toStdString(), style))
			return;

		size_t start = 0;
		size_t end = 0;

		if (selectionRange(start, end))
			styles::apply(_document, start, end, style);
		else
			styles::apply(_document, style);

		_highlighter->rehighlight();

		commit();
		syncControls();
	});
}

QWidget *EditorWindow::buildTopBar()
{
	auto *bar = new QWidget(this);
	bar->setObjectName("topBar");
	bar->setAttribute(Qt::WA_StyledBackground);

	auto *layout = new QHBoxLayout(bar);
	layout->setContentsMargins(14, 8, 14, 8);
	layout->setSpacing(8);

	auto *brand = new QLabel(QStringLiteral("Slugged"), bar);
	brand->setObjectName("brand");

	_nameButton = new QPushButton(QString::fromUtf8(obs_source_get_name(_source)), bar);
	_nameButton->setObjectName("nameButton");
	_nameButton->setToolTip(tr_("Editor.Rename"));
	_nameButton->setCursor(Qt::PointingHandCursor);

	connect(_nameButton, &QPushButton::clicked, this, &EditorWindow::renameSource);

	auto *live = new QLabel(tr_("Editor.Live"), bar);
	live->setObjectName("muted");

	_undoButton = iconButton(Glyph::Undo, tr_("Editor.Undo"), bar);
	_redoButton = iconButton(Glyph::Redo, tr_("Editor.Redo"), bar);

	connect(_undoButton, &QPushButton::clicked, this, [this]() { triggerHistory(false); });
	connect(_redoButton, &QPushButton::clicked, this, [this]() { triggerHistory(true); });

	// The editor's history is OBS's own, so Edit > Undo in the main window and
	// these buttons are the same thing.
	if (auto *main = static_cast<QWidget *>(obs_frontend_get_main_window())) {
		_undoAction = main->findChild<QAction *>(QStringLiteral("actionMainUndo"));
		_redoAction = main->findChild<QAction *>(QStringLiteral("actionMainRedo"));
	}

	auto followAction = [](QAction *action, QPushButton *button) {
		if (!action) {
			button->hide();
			return;
		}

		button->setEnabled(action->isEnabled());

		connect(action, &QAction::changed, button,
			[action, button]() { button->setEnabled(action->isEnabled()); });
	};

	followAction(_undoAction, _undoButton);
	followAction(_redoAction, _redoButton);

	_stylesButton = new QPushButton(tr_("Styles.Title"), bar);
	_stylesButton->setCheckable(true);

	_insertButton = new QPushButton(tr_("Insert.Open"), bar);
	_insertButton->setIcon(icon(Glyph::Plus));
	_insertButton->setCheckable(true);

	connect(_stylesButton, &QPushButton::clicked, this,
		[this](bool on) { showSide(on ? SideStyles : SideInspector); });
	connect(_insertButton, &QPushButton::clicked, this,
		[this](bool on) { showSide(on ? SideInsert : SideInspector); });

	auto *done = new QPushButton(tr_("Editor.Done"), bar);
	done->setObjectName("primary");

	connect(done, &QPushButton::clicked, this, &QWidget::close);

	layout->addWidget(brand);
	layout->addWidget(_nameButton);
	layout->addWidget(live);
	layout->addStretch(1);
	layout->addWidget(_undoButton);
	layout->addWidget(_redoButton);
	layout->addWidget(_stylesButton);
	layout->addWidget(_insertButton);
	layout->addWidget(done);

	return bar;
}

QWidget *EditorWindow::buildCanvasColumn()
{
	auto *column = new QWidget(this);
	column->setObjectName("canvasColumn");
	column->setAttribute(Qt::WA_StyledBackground);

	auto *layout = new QVBoxLayout(column);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	_preview = new PreviewWidget(_source, column);

	_previewFallback = new QLabel(tr_("Editor.PreviewUnavailable"), column);
	_previewFallback->setObjectName("muted");
	_previewFallback->setWordWrap(true);
	_previewFallback->setAlignment(Qt::AlignCenter);
	_previewFallback->setContentsMargins(40, 40, 40, 40);
	_previewFallback->hide();

	_textEdit = new QPlainTextEdit(column);
	_textEdit->setTabChangesFocus(true);
	_textEdit->setMaximumHeight(170);
	_textEdit->setAccessibleName(tr_("Text"));

	// One history: OBS's. The box keeping a second one of its own would undo
	// typing but not styling.
	_textEdit->setUndoRedoEnabled(false);
	_textEdit->installEventFilter(this);
	_textEdit->hide();

	_highlighter = new StyleHighlighter(this, _textEdit->document());

	connect(_textEdit, &QPlainTextEdit::textChanged, this, &EditorWindow::onTextChanged);

	// Moving the cursor or the selection changes what the controls edit.
	connect(_textEdit, &QPlainTextEdit::selectionChanged, this, [this]() {
		if (!_updating)
			syncControls();
	});
	connect(_textEdit, &QPlainTextEdit::cursorPositionChanged, this, [this]() {
		_preview->showCaretNow();
		syncOverlay();
	});

	connect(_preview, &PreviewWidget::displayResolved, this, [this]() { syncEditMode(); });

	// The preview holds no text of its own: it reports where it was clicked
	// and hands its keys to the text box, shown or not.
	_preview->setInputTarget(_textEdit);

	connect(_preview, &PreviewWidget::pressed, this, [this](QPointF at, bool extend) {
		if (!_editMap)
			return;

		const size_t offset = _editMap->hitTest(float(at.x()), float(at.y()));

		setCursorOffsets(extend ? utf8Offset(_lastText, _textEdit->textCursor().anchor()) : offset, offset);
	});

	connect(_preview, &PreviewWidget::dragged, this, [this](QPointF at) {
		if (!_editMap)
			return;

		setCursorOffsets(utf8Offset(_lastText, _textEdit->textCursor().anchor()),
				 _editMap->hitTest(float(at.x()), float(at.y())));
	});

	connect(_preview, &PreviewWidget::doubleClicked, this, [this](QPointF at) {
		if (!_editMap)
			return;

		const uint32_t offset = _editMap->hitTest(float(at.x()), float(at.y()));

		// A live value is one thing, so a double click takes all of it.
		for (uint32_t probe : {offset + 1, offset > 0 ? offset - 1 : 0}) {
			uint32_t start = 0;
			uint32_t end = 0;

			if (_editMap->tokenAt(probe, start, end)) {
				setCursorOffsets(start, end);
				return;
			}
		}

		setCursorOffsets(offset, offset);

		QTextCursor cursor = _textEdit->textCursor();
		cursor.select(QTextCursor::WordUnderCursor);
		_textEdit->setTextCursor(cursor);
	});

	layout->addWidget(buildContextBar());
	layout->addWidget(_preview, 1);
	layout->addWidget(_previewFallback, 1);
	layout->addWidget(_textEdit);
	layout->addWidget(buildBottomBar());

	return column;
}

QWidget *EditorWindow::buildContextBar()
{
	// One strip above the preview, always the same height so the preview never
	// jumps: a note about how to edit when nothing is selected, and the handful
	// of things most often done to a selection when something is.
	_context = new QStackedWidget(this);
	_context->setObjectName("contextBar");
	_context->setAttribute(Qt::WA_StyledBackground);
	_context->setFixedHeight(46);

	// ---- nothing selected ----
	auto *note = new QWidget(_context);
	auto *noteLayout = new QHBoxLayout(note);
	noteLayout->setContentsMargins(14, 0, 14, 0);

	_contextNote = new QLabel(note);

	_hintDismiss = new QPushButton(tr_("Editor.Hint.Dismiss"), note);
	_hintDismiss->setObjectName("ghost");

	connect(_hintDismiss, &QPushButton::clicked, this, [this]() {
		dismissHint();
		syncEditMode();
	});

	noteLayout->addWidget(_contextNote, 1);
	noteLayout->addWidget(_hintDismiss);

	// ---- a selection ----
	auto *quick = new QWidget(_context);
	auto *quickLayout = new QHBoxLayout(quick);
	quickLayout->setContentsMargins(14, 0, 14, 0);
	quickLayout->setSpacing(6);

	auto *label = new QLabel(tr_("Editor.Quick.Label"), quick);
	label->setObjectName("muted");

	auto *smaller = new QPushButton(QStringLiteral("A"), quick);
	smaller->setObjectName("toggle");
	smaller->setStyleSheet("font-size: 11px; font-weight: 600;");
	smaller->setToolTip(tr_("Editor.Quick.Smaller"));
	smaller->setAccessibleName(tr_("Editor.Quick.Smaller"));

	auto *bigger = new QPushButton(QStringLiteral("A"), quick);
	bigger->setObjectName("toggle");
	bigger->setStyleSheet("font-size: 17px; font-weight: 600;");
	bigger->setToolTip(tr_("Editor.Quick.Bigger"));
	bigger->setAccessibleName(tr_("Editor.Quick.Bigger"));

	connect(smaller, &QPushButton::clicked, this, [this]() { nudgeSize(1.0f / 1.1f); });
	connect(bigger, &QPushButton::clicked, this, [this]() { nudgeSize(1.1f); });

	_quickSize = new QLabel(quick);
	_quickSize->setMinimumWidth(34);
	_quickSize->setAlignment(Qt::AlignCenter);

	_quickBold = new QPushButton(QStringLiteral("B"), quick);
	_quickBold->setObjectName("toggle");
	_quickBold->setCheckable(true);
	_quickBold->setStyleSheet("font-weight: 800;");
	_quickBold->setToolTip(tr_("Editor.Bold"));
	_quickBold->setAccessibleName(tr_("Editor.Bold"));

	connect(_quickBold, &QPushButton::clicked, this, [this](bool on) {
		StyleOverride d;
		d.weight = on ? 700 : 400;
		applyStyle(d);
		syncControls();
	});

	_quickItalic = new QPushButton(QStringLiteral("I"), quick);
	_quickItalic->setObjectName("toggle");
	_quickItalic->setCheckable(true);
	_quickItalic->setStyleSheet("font-style: italic; font-family: Georgia, serif;");
	_quickItalic->setToolTip(tr_("Editor.Italic"));
	_quickItalic->setAccessibleName(tr_("Editor.Italic"));

	connect(_quickItalic, &QPushButton::clicked, this, [this](bool on) {
		StyleOverride d;
		d.italic = on;
		applyStyle(d);
		syncControls();
	});

	_quickColor = new ColorButton(quick);
	_quickColor->setCaption(tr_("Color"));

	connect(_quickColor, &ColorButton::colorChanged, this, [this]() {
		StyleOverride d;
		d.color = _quickColor->color();
		applyStyle(d);
		syncControls();
	});

	auto *clear = new QPushButton(tr_("Editor.Quick.Clear"), quick);
	clear->setObjectName("ghost");

	connect(clear, &QPushButton::clicked, this, &EditorWindow::clearFormatting);

	quickLayout->addWidget(label);
	quickLayout->addWidget(smaller);
	quickLayout->addWidget(_quickSize);
	quickLayout->addWidget(bigger);
	quickLayout->addSpacing(8);
	quickLayout->addWidget(_quickBold);
	quickLayout->addWidget(_quickItalic);
	quickLayout->addWidget(_quickColor);
	quickLayout->addStretch(1);
	quickLayout->addWidget(clear);

	_context->addWidget(note);
	_context->addWidget(quick);

	return _context;
}

QWidget *EditorWindow::buildBottomBar()
{
	auto *bar = new QWidget(this);
	bar->setObjectName("bottomBar");
	bar->setAttribute(Qt::WA_StyledBackground);

	auto *layout = new QHBoxLayout(bar);
	layout->setContentsMargins(14, 8, 14, 8);
	layout->setSpacing(8);

	// ---- zoom ----
	_zoomButton = new QPushButton(tr_("Editor.Zoom.Fit"), bar);
	_zoomButton->setObjectName("ghost");
	_zoomButton->setToolTip(tr_("Editor.Zoom"));
	_zoomButton->setAccessibleName(tr_("Editor.Zoom"));

	auto *zoomMenu = new QMenu(_zoomButton);

	auto addZoom = [this, zoomMenu](const QString &label, float scale) {
		connect(zoomMenu->addAction(label), &QAction::triggered, this, [this, label, scale]() {
			_preview->setScale(scale);
			_zoomButton->setText(label);
		});
	};

	addZoom(tr_("Editor.Zoom.Fit"), 0.0f);
	addZoom(QStringLiteral("25%"), 0.25f);
	addZoom(QStringLiteral("50%"), 0.5f);
	addZoom(QStringLiteral("100%"), 1.0f);
	addZoom(QStringLiteral("200%"), 2.0f);

	// The button names the current zoom; pressing it offers the others.
	connect(_zoomButton, &QPushButton::clicked, this, [this, zoomMenu]() {
		zoomMenu->exec(_zoomButton->mapToGlobal(QPoint(0, -zoomMenu->sizeHint().height())));
	});

	// ---- timer: only when the text counts something ----
	_timerBar = new QWidget(bar);

	auto *timerLayout = new QHBoxLayout(_timerBar);
	timerLayout->setContentsMargins(0, 0, 0, 0);
	timerLayout->setSpacing(8);

	auto *timerLabel = new QLabel(tr_("Editor.Timer"), _timerBar);
	timerLabel->setObjectName("muted");

	_timerValue = new QLabel(_timerBar);
	_timerValue->setObjectName("timerValue");
	_timerValue->setMinimumWidth(48);

	_timerToggle = iconButton(Glyph::Pause, tr_("Timer.Toggle"), _timerBar);

	auto *timerReset = iconButton(Glyph::Reset, tr_("Timer.Reset"), _timerBar);

	connect(_timerToggle, &QPushButton::clicked, this, [this]() { callProc("timer_toggle"); });
	connect(timerReset, &QPushButton::clicked, this, [this]() { callProc("timer_reset"); });

	timerLayout->addWidget(timerLabel);
	timerLayout->addWidget(_timerValue);
	timerLayout->addWidget(_timerToggle);
	timerLayout->addWidget(timerReset);

	// ---- the text box, for those who want it ----
	_textBoxToggle = new QPushButton(tr_("Editor.TextBox"), bar);
	_textBoxToggle->setObjectName("ghost");
	_textBoxToggle->setCheckable(true);
	_textBoxToggle->setToolTip(tr_("Editor.TextBox.Hint"));

	connect(_textBoxToggle, &QPushButton::clicked, this, [this]() { syncEditMode(); });

	auto *play = new QPushButton(tr_("Editor.PlayMotion"), bar);
	play->setObjectName("ghost");
	play->setIcon(icon(Glyph::Play));
	play->setIconSize(QSize(12, 12));

	connect(play, &QPushButton::clicked, this, [this]() { callProc("replay_motion"); });

	layout->addWidget(_zoomButton);
	layout->addStretch(1);
	layout->addWidget(_timerBar);
	layout->addStretch(1);
	layout->addWidget(_textBoxToggle);
	layout->addWidget(play);

	return bar;
}

// ---------------------------------------------------------------------------
// Shell
// ---------------------------------------------------------------------------

void EditorWindow::closeEvent(QCloseEvent *event)
{
	endMotionPreview();

	QMainWindow::closeEvent(event);
}

void EditorWindow::showSide(SidePage page)
{
	_side->setCurrentIndex(int(page));

	_insertButton->setChecked(page == SideInsert);
	_stylesButton->setChecked(page == SideStyles);

	if (page == SideInsert) {
		_insertPanel->setSourceVariables(_variables);
		_insertPanel->focusSearch();
	} else if (page == SideStyles) {
		_stylesPanel->refresh();
	} else if (_canvasEditing) {
		_preview->setFocus();
	} else if (_textEdit->isVisible()) {
		_textEdit->setFocus();
	}
}

void EditorWindow::showStartPage(bool cancellable)
{
	_startPage->setCancellable(cancellable);
	_pages->setCurrentIndex(1);
}

void EditorWindow::onTick()
{
	if (_detached)
		return;

	// Frames are built off-thread, so the map for what was just typed arrives
	// a moment after the keystroke.
	std::shared_ptr<const EditMap> map = editMapOf(_source);

	if (map != _editMap) {
		_editMap = std::move(map);
		syncOverlay();
	}

	const bool timer = !_fileMode && usesTimer(_lastText);

	if (_timerBar->isVisible() != timer)
		_timerBar->setVisible(timer);

	if (timer) {
		const TimerState state = timerStateOf(_source);

		_timerValue->setText(QString::fromStdString(formatDuration(double(state.seconds))));

		const bool showsPause = _timerToggle->property("running").toBool();

		if (showsPause != state.running || !_timerToggle->property("running").isValid()) {
			_timerToggle->setProperty("running", state.running);
			_timerToggle->setIcon(icon(state.running ? Glyph::Pause : Glyph::Play));
		}
	}
}

void EditorWindow::syncEditMode()
{
	const bool live = _preview->isLive();

	// Typing on the preview needs the text to be where the caret says it is,
	// and to be the text that is stored.
	const bool canType = live && !_markup && !_fileMode && !_document.scroll.enabled;

	_canvasEditing = canType;

	// Whether the preview is live is only known once it has been shown; until
	// then it stays in place so that it can be.
	const bool failed = _preview->hasTried() && !live;

	_preview->setVisible(!failed);
	_previewFallback->setVisible(failed);

	_preview->setEditable(canType);

	_textBoxToggle->setVisible(canType);
	_textEdit->setVisible(!_fileMode && (!canType || _textBoxToggle->isChecked()));

	QString note;
	bool dismissible = false;

	if (_fileMode) {
		note = tr_("Editor.Note.File");
	} else if (_markup) {
		note = tr_("Editor.Note.Markup");
	} else if (failed) {
		note = tr_("Editor.Note.NoPreview");
	} else if (_document.scroll.enabled) {
		note = tr_("Editor.Note.Scroll");
	} else if (!hintDismissed()) {
		note = tr_("Editor.Hint");
		dismissible = true;
	}

	_contextNote->setText(note);
	_hintDismiss->setVisible(dismissible);

	syncOverlay();
}

void EditorWindow::syncOverlay()
{
	PreviewWidget::Overlay overlay;

	if (_canvasEditing && _editMap && !_editMap->empty()) {
		size_t start = 0;
		size_t end = 0;

		if (selectionRange(start, end)) {
			overlay.selection = _editMap->selection(uint32_t(start), uint32_t(end));
		} else {
			overlay.caretVisible = true;
		}

		overlay.caret = _editMap->caret(uint32_t(cursorOffset()));
		overlay.tokens = _editMap->tokens();
	}

	_preview->setOverlay(overlay);
}

void EditorWindow::callProc(const char *name)
{
	if (_detached)
		return;

	calldata_t call = {};
	proc_handler_call(obs_source_get_proc_handler(_source), name, &call);
	calldata_free(&call);
}

void EditorWindow::triggerHistory(bool redo)
{
	QAction *action = redo ? _redoAction : _undoAction;

	if (action && action->isEnabled())
		action->trigger();
}

void EditorWindow::renameSource()
{
	if (_detached)
		return;

	const QString current = QString::fromUtf8(obs_source_get_name(_source));

	bool ok = false;
	const QString name = QInputDialog::getText(this, tr_("Editor.Rename"), tr_("Editor.Rename.Label"),
						   QLineEdit::Normal, current, &ok)
				     .trimmed();

	if (!ok || name.isEmpty() || name == current)
		return;

	// OBS finds sources by name, so two may not share one. Filters are named
	// within the source they sit on and are left to OBS's own dialog.
	if (obs_source_get_type(_source) == OBS_SOURCE_TYPE_INPUT) {
		if (obs_source_t *taken = obs_get_source_by_name(name.toUtf8().constData())) {
			obs_source_release(taken);
			_nameButton->setToolTip(tr_("Editor.Rename.Taken").arg(name));
			return;
		}
	}

	obs_source_set_name(_source, name.toUtf8().constData());

	_nameButton->setText(name);
	_nameButton->setToolTip(tr_("Editor.Rename"));
	setWindowTitle(tr_("Editor.Title") + " - " + name);
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

bool EditorWindow::selectionRange(size_t &start, size_t &end) const
{
	if (_markup || _fileMode || !_textEdit)
		return false;

	const QTextCursor cursor = _textEdit->textCursor();

	if (!cursor.hasSelection())
		return false;

	const QString text = _textEdit->toPlainText();

	start = utf8Offset(text, cursor.selectionStart());
	end = utf8Offset(text, cursor.selectionEnd());

	return end > start;
}

size_t EditorWindow::cursorOffset() const
{
	return utf8Offset(_textEdit->toPlainText(), _textEdit->textCursor().position());
}

void EditorWindow::setCursorOffsets(size_t anchor, size_t position)
{
	const QString text = _textEdit->toPlainText();

	QTextCursor cursor = _textEdit->textCursor();

	cursor.setPosition(utf16Position(text, anchor));
	cursor.setPosition(utf16Position(text, position), QTextCursor::KeepAnchor);

	_textEdit->setTextCursor(cursor);
}

void EditorWindow::insertAtCursor(const QString &text)
{
	if (_fileMode)
		return;

	_textEdit->textCursor().insertText(text);
}

bool EditorWindow::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == _textEdit && event->type() == QEvent::KeyPress) {
		if (handleTextKey(static_cast<QKeyEvent *>(event)))
			return true;
	}

	// Motion tiles preview on hover.
	if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
		for (const auto &[motion, tile] : _motionTiles) {
			if (watched != tile)
				continue;

			if (event->type() == QEvent::Enter) {
				_pendingPreview = motion;
				_previewDelay->start();
			} else {
				_pendingPreview = -1;
				_previewDelay->stop();
				endMotionPreview();
			}

			break;
		}
	}

	return QMainWindow::eventFilter(watched, event);
}

// Keys the text box would handle differently from how the editor wants them
// handled. Returns true when the key is dealt with. Keys typed on the preview
// arrive here too, since the preview forwards them to the box.
bool EditorWindow::handleTextKey(QKeyEvent *event)
{
	if (event->matches(QKeySequence::Undo)) {
		triggerHistory(false);
		return true;
	}

	if (event->matches(QKeySequence::Redo)) {
		triggerHistory(true);
		return true;
	}

	if (event->key() == Qt::Key_Escape && _side->currentIndex() != SideInspector) {
		showSide(SideInspector);
		return true;
	}

	if (_markup || _fileMode)
		return false;

	// An opening brace starts a live value: offer the list instead.
	if (event->text() == QLatin1String("{")) {
		showSide(SideInsert);
		return true;
	}

	if (!_canvasEditing || !_editMap || _editMap->empty())
		return false;

	const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
	const QTextCursor cursor = _textEdit->textCursor();
	const uint32_t position = uint32_t(cursorOffset());

	// Up and down follow the lines as they are on screen -- wrapped lines
	// included -- rather than the box's own idea of them.
	if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) {
		const uint32_t target = _editMap->vertical(position, event->key() == Qt::Key_Up ? -1 : 1);

		setCursorOffsets(shift ? utf8Offset(_lastText, cursor.anchor()) : target, target);
		return true;
	}

	// A live value is one thing: deleting into it removes it whole, and the
	// caret steps over it rather than into its braces.
	if (!cursor.hasSelection()) {
		const bool back = event->key() == Qt::Key_Backspace;
		const bool forward = event->key() == Qt::Key_Delete;
		const bool left = event->key() == Qt::Key_Left;
		const bool right = event->key() == Qt::Key_Right;

		if (back || forward || left || right) {
			const uint32_t probe = (back || left) ? (position > 0 ? position - 1 : 0) : position + 1;

			uint32_t start = 0;
			uint32_t end = 0;

			// The map can be one keystroke behind the text, so only trust
			// a token that is still there.
			const QByteArray bytes = _lastText.toUtf8();

			if (_editMap->tokenAt(probe, start, end) && end <= uint32_t(bytes.size()) &&
			    bytes[start] == '{' && bytes[end - 1] == '}') {
				if (back || forward) {
					setCursorOffsets(start, end);
					_textEdit->textCursor().removeSelectedText();
				} else {
					const uint32_t target = left ? start : end;

					setCursorOffsets(shift ? utf8Offset(_lastText, cursor.anchor()) : target,
							 target);
				}

				return true;
			}
		}
	}

	return false;
}

void EditorWindow::applyStyle(const StyleOverride &delta)
{
	if (_updating)
		return;

	size_t start = 0;
	size_t end = 0;

	if (selectionRange(start, end))
		_document.applyStyle(start, end, delta);
	else
		_document.applyBaseStyle(delta);

	_updating = true;
	_highlighter->rehighlight();
	_updating = false;

	commit();
	syncSummaries();
}

void EditorWindow::applyParagraph(const ParagraphOverride &delta)
{
	if (_updating)
		return;

	size_t start = 0;
	size_t end = 0;

	if (selectionRange(start, end))
		_document.applyParagraph(start, end, delta);
	else
		_document.applyBaseParagraph(delta);

	commit();
	syncSummaries();
}

void EditorWindow::applyDocument(const std::function<void(Document &)> &change)
{
	if (_updating)
		return;

	change(_document);
	commit();

	syncVisibility();
	syncSummaries();
}

void EditorWindow::clearFormatting()
{
	size_t start = 0;
	size_t end = 0;

	if (!selectionRange(start, end))
		return;

	_document.clearStyle(start, end);

	_updating = true;
	_highlighter->rehighlight();
	_updating = false;

	commit();
	syncControls();
}

void EditorWindow::nudgeSize(float factor)
{
	size_t start = 0;
	size_t end = 0;

	const float current = selectionRange(start, end) ? _document.styleAt(start + 1).sizePx : _document.base.sizePx;

	// At least a pixel, so small text does not get stuck.
	float next = std::round(current * factor);

	if (next == std::round(current))
		next += factor > 1.0f ? 1.0f : -1.0f;

	StyleOverride d;
	d.size = std::clamp(next, 1.0f, 2000.0f);

	applyStyle(d);
	syncControls();
}

void EditorWindow::onTextChanged()
{
	if (_updating)
		return;

	const QString text = _textEdit->toPlainText();

	if (text == _lastText)
		return;

	if (!_markup) {
		// Turn the change into one edit, so every character that did not
		// change keeps its styling. Positions are snapped so the edit never
		// splits a surrogate pair.
		const int limit = int(std::min(text.size(), _lastText.size()));

		int prefix = 0;

		while (prefix < limit && text[prefix] == _lastText[prefix])
			prefix++;

		if (prefix > 0 && prefix < int(text.size()) && text[prefix - 1].isHighSurrogate())
			prefix--;

		int suffix = 0;

		while (suffix < limit - prefix &&
		       text[text.size() - 1 - suffix] == _lastText[_lastText.size() - 1 - suffix])
			suffix++;

		if (suffix > 0 && text[text.size() - suffix].isLowSurrogate())
			suffix--;

		const size_t start = utf8Offset(_lastText, prefix);
		const size_t end = utf8Offset(_lastText, int(_lastText.size()) - suffix);
		const std::string inserted = text.mid(prefix, int(text.size()) - suffix - prefix).toStdString();

		_document.replaceText(start, end, inserted);

		_updating = true;
		_highlighter->rehighlight();
		_updating = false;
	}

	_lastText = text;

	commit();
}

void EditorWindow::onExternalUpdate()
{
	if (_detached)
		return;

	obs_data_t *data = obs_source_get_settings(_source);
	const std::string json = settingsJson(data);
	obs_data_release(data);

	// Our own writes come back through the same signal.
	if (json == _lastCommitted)
		return;

	reload();
}

void EditorWindow::writeSettings(const std::function<void(obs_data_t *)> &change, bool reloadAfter)
{
	if (_detached)
		return;

	// A hovered motion is not a setting; make sure it is never what gets
	// compared against or built upon.
	endMotionPreview();

	obs_data_t *data = obs_source_get_settings(_source);

	if (!data)
		return;

	const std::string before = settingsJson(data);

	change(data);

	const std::string after = settingsJson(data);

	_lastCommitted = after;

	obs_source_update(_source, data);
	obs_data_release(data);

	// One undo step per burst of changes: OBS merges repeated actions of the
	// same name, so dragging a slider is undone in one go.
	if (before != after) {
		const std::string undo = undoPayload(_source, before);
		const std::string redo = undoPayload(_source, after);

		obs_frontend_add_undo_redo_action(obs_module_text("Editor.UndoName"), applyUndoPayload,
						  applyUndoPayload, undo.c_str(), redo.c_str(), true);
	}

	if (reloadAfter)
		reload();
}

void EditorWindow::commit()
{
	writeSettings(
		[this](obs_data_t *data) {
			settings::save(data, _document, _markup);

			if (_markup)
				obs_data_set_string(data, "text", _textEdit->toPlainText().toStdString().c_str());
		},
		false);
}

void EditorWindow::previewMotion(Motion motion)
{
	if (_detached || _document.motion.motion == motion)
		return;

	obs_data_t *data = obs_source_get_settings(_source);

	if (!data)
		return;

	Document shown = _document;

	shown.motion.motion = motion;

	const MotionParamSpec amount = motionParamSpec(motion);

	if (amount.label && (shown.motion.param < amount.min || shown.motion.param > amount.max))
		shown.motion.param = amount.defaultValue;

	// Seen, not chosen: no undo step, and reload() must not mistake it for
	// somebody else's change.
	settings::save(data, shown, _markup);

	_lastCommitted = settingsJson(data);
	_previewingMotion = true;

	obs_source_update(_source, data);
	obs_data_release(data);
}

void EditorWindow::endMotionPreview()
{
	if (!_previewingMotion || _detached)
		return;

	_previewingMotion = false;

	obs_data_t *data = obs_source_get_settings(_source);

	if (!data)
		return;

	settings::save(data, _document, _markup);

	_lastCommitted = settingsJson(data);

	obs_source_update(_source, data);
	obs_data_release(data);
}

void EditorWindow::reload()
{
	if (_detached)
		return;

	obs_data_t *data = obs_source_get_settings(_source);

	if (!data)
		return;

	settings::migrate(data);

	const settings::SourceConfig config = settings::load(data);

	_document = config.document;
	_markup = config.markup;
	_fileMode = config.mode == settings::SourceMode::File;
	_variables = config.variables;
	_lastCommitted = settingsJson(data);
	_previewingMotion = false;

	const bool untouched = isUntouched(data);

	_updating = true;

	_filePath->setText(QString::fromUtf8(obs_data_get_string(data, "file")));
	_chatlog->setChecked(obs_data_get_bool(data, "chatlog"));
	_chatlogLines->setValue(int(obs_data_get_int(data, "chatlog_lines")));
	_variablesFile->setText(QString::fromUtf8(obs_data_get_string(data, "variables_file")));
	_counterVariable->setText(QString::fromUtf8(obs_data_get_string(data, settings::kCounterVariableKey)));
	_counterStep->setValue(obs_data_get_double(data, settings::kCounterStepKey));
	setCombo(_filterMode, int(config.filterMode));

	obs_data_release(data);

	const QString text = _markup ? QString::fromStdString(config.text)
				     : QString::fromStdString(_document.plainText());

	if (_textEdit->toPlainText() != text) {
		QTextCursor cursor = _textEdit->textCursor();
		const int anchor = cursor.anchor();
		const int position = cursor.position();

		_textEdit->setPlainText(text);

		cursor = _textEdit->textCursor();
		cursor.setPosition(std::min(anchor, int(text.size())));
		cursor.setPosition(std::min(position, int(text.size())), QTextCursor::KeepAnchor);
		_textEdit->setTextCursor(cursor);
	}

	_lastText = text;
	_highlighter->rehighlight();
	_markupBox->setChecked(_markup);
	_textFrom->setValue(_fileMode ? 1 : 0);

	_updating = false;

	_insertPanel->setSourceVariables(_variables);

	syncControls();
	syncEditMode();

	// A source nobody has put anything into yet is offered somewhere to
	// start, once.
	if (untouched && !_startOffered)
		showStartPage(true);

	_startOffered = true;
}

// ---------------------------------------------------------------------------
// editor_bridge
// ---------------------------------------------------------------------------

namespace {

// Touched from the UI thread and from whatever thread destroys a source.
std::mutex g_windowsMutex;
std::map<obs_source_t *, EditorWindow *> g_windows;

} // namespace

namespace editor {

bool available()
{
	return true;
}

void openFor(obs_source_t *source)
{
	if (!source)
		return;

	EditorWindow *existing = nullptr;

	{
		std::lock_guard<std::mutex> lock(g_windowsMutex);

		const auto it = g_windows.find(source);

		if (it != g_windows.end())
			existing = it->second;
	}

	if (existing) {
		existing->reload();
		existing->show();
		existing->raise();
		existing->activateWindow();

		return;
	}

	auto *parent = static_cast<QWidget *>(obs_frontend_get_main_window());
	auto *window = new EditorWindow(source, parent);

	{
		std::lock_guard<std::mutex> lock(g_windowsMutex);
		g_windows[source] = window;
	}

	QObject::connect(window, &QObject::destroyed, [source, window]() {
		std::lock_guard<std::mutex> lock(g_windowsMutex);

		const auto it = g_windows.find(source);

		if (it != g_windows.end() && it->second == window)
			g_windows.erase(it);
	});

	window->show();
}

// Sources can be destroyed on any thread -- OBS has a destruction thread --
// while widgets belong to the UI thread. Waiting for the UI thread here could
// deadlock against a UI thread that is itself waiting for destruction to
// finish, so the window is cut loose from the source immediately and deleted
// later.
void closeFor(obs_source_t *source)
{
	EditorWindow *window = nullptr;

	{
		std::lock_guard<std::mutex> lock(g_windowsMutex);

		const auto it = g_windows.find(source);

		if (it == g_windows.end())
			return;

		window = it->second;
		g_windows.erase(it);
	}

	window->detach();

	QMetaObject::invokeMethod(window, [window]() { delete window; }, Qt::QueuedConnection);
}

void shutdown()
{
	std::map<obs_source_t *, EditorWindow *> windows;

	{
		std::lock_guard<std::mutex> lock(g_windowsMutex);
		windows = std::move(g_windows);
		g_windows.clear();
	}

	for (auto &[source, window] : windows) {
		UNUSED_PARAMETER(source);

		window->detach();
		delete window;
	}
}

} // namespace editor

} // namespace slugged
