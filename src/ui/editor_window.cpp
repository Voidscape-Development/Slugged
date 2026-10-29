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
#include "preview_widget.hpp"
#include "../obs/editor_bridge.hpp"
#include "../obs/settings.hpp"
#include "../text/font_enum.hpp"
#include "../text/font_manager.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBlock>
#include <QVBoxLayout>

#include <obs-frontend-api.h>
#include <obs-module.h>

namespace slugged {

namespace {

QString tr_(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

// Qt measures text in UTF-16 code units; the document in UTF-8 bytes.
size_t utf8Offset(const QString &text, int position)
{
	return size_t(text.left(std::clamp(position, 0, int(text.size()))).toUtf8().size());
}

QColor toQColor(const Rgba &c)
{
	QColor q;
	q.setRgbF(qreal(c.r), qreal(c.g), qreal(c.b), qreal(std::max(c.a, 0.35f)));
	return q;
}

template<typename E> QComboBox *enumCombo(QWidget *parent, std::initializer_list<std::pair<E, const char *>> items)
{
	auto *combo = new QComboBox(parent);

	for (const auto &[value, key] : items)
		combo->addItem(tr_(key), int(value));

	return combo;
}

template<typename E> E comboValue(const QComboBox *combo)
{
	return E(combo->currentData().toInt());
}

void setCombo(QComboBox *combo, int value)
{
	const int index = combo->findData(value);

	if (index >= 0)
		combo->setCurrentIndex(index);
}

QDoubleSpinBox *spin(QWidget *parent, double lo, double hi, double step, const char *suffix = nullptr, int decimals = 1)
{
	auto *box = new QDoubleSpinBox(parent);

	box->setRange(lo, hi);
	box->setSingleStep(step);
	box->setDecimals(decimals);
	box->setKeyboardTracking(false);

	if (suffix)
		box->setSuffix(QString::fromUtf8(suffix));

	return box;
}

// Hides a form row along with its label.
void setRowVisible(QFormLayout *form, QWidget *field, bool visible)
{
	if (!form || !field)
		return;

	field->setVisible(visible);

	if (QWidget *label = form->labelForField(field))
		label->setVisible(visible);
}

const std::pair<int, const char *> kWeights[] = {
	{100, "Editor.Weight.Thin"},    {200, "Editor.Weight.ExtraLight"}, {300, "Editor.Weight.Light"},
	{400, "Editor.Weight.Regular"}, {500, "Editor.Weight.Medium"},     {600, "Editor.Weight.SemiBold"},
	{700, "Editor.Weight.Bold"},    {800, "Editor.Weight.ExtraBold"},  {900, "Editor.Weight.Black"},
};

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
		format.setFontPointSize(std::max(1.0, font.pointSizeF() * scale));

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
	resize(1240, 760);

	buildUi();
	reload();

	signal_handler_connect(obs_source_get_signal_handler(_source), "update", sourceUpdated, this);
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

	if (_preview)
		_preview->detach();
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
	auto *splitter = new QSplitter(Qt::Horizontal, this);

	// ---- preview -------------------------------------------------------
	auto *previewHost = new QWidget(splitter);
	auto *previewLayout = new QVBoxLayout(previewHost);

	_preview = new PreviewWidget(_source, previewHost);
	previewLayout->addWidget(_preview, 1);

	if (!_preview->isLive()) {
		auto *notice = new QLabel(tr_("Editor.PreviewUnavailable"), previewHost);

		notice->setWordWrap(true);
		notice->setAlignment(Qt::AlignCenter);
		previewLayout->addWidget(notice);
	}

	auto *zoomRow = new QHBoxLayout();
	auto *zoom = new QSlider(Qt::Horizontal, previewHost);

	zoom->setRange(10, 400);
	zoom->setValue(100);

	connect(zoom, &QSlider::valueChanged, this, [this](int value) { _preview->setZoom(float(value) / 100.0f); });

	zoomRow->addWidget(new QLabel(tr_("Editor.Zoom"), previewHost));
	zoomRow->addWidget(zoom, 1);
	previewLayout->addLayout(zoomRow);

	splitter->addWidget(previewHost);

	// ---- text and inspector ---------------------------------------------
	auto *right = new QWidget(splitter);
	auto *rightLayout = new QVBoxLayout(right);

	rightLayout->addWidget(buildTextPanel(), 2);

	auto *tabs = new QTabWidget(right);

	tabs->addTab(buildFontPanel(), tr_("Editor.Tab.Font"));
	tabs->addTab(buildFillPanel(), tr_("Editor.Tab.Style"));
	tabs->addTab(buildLayoutPanel(), tr_("Editor.Tab.Layout"));
	tabs->addTab(buildMotionPanel(), tr_("Editor.Tab.Motion"));
	tabs->addTab(buildVariablesPanel(), tr_("Editor.Tab.Variables"));

	rightLayout->addWidget(tabs, 3);

	splitter->addWidget(right);
	splitter->setStretchFactor(0, 3);
	splitter->setStretchFactor(1, 2);

	setCentralWidget(splitter);
}

QWidget *EditorWindow::buildTextPanel()
{
	auto *panel = new QWidget(this);
	auto *layout = new QVBoxLayout(panel);
	layout->setContentsMargins(0, 0, 0, 0);

	_textEdit = new QPlainTextEdit(panel);
	_textEdit->setTabChangesFocus(true);

	_highlighter = new StyleHighlighter(this, _textEdit->document());

	connect(_textEdit, &QPlainTextEdit::textChanged, this, &EditorWindow::onTextChanged);

	// Moving the cursor or the selection changes what the controls edit.
	connect(_textEdit, &QPlainTextEdit::selectionChanged, this, [this]() {
		if (!_updating)
			syncControls();
	});

	_scopeLabel = new QLabel(panel);
	_scopeLabel->setWordWrap(true);

	_markupBox = new QCheckBox(tr_("Markup"), panel);
	_markupBox->setToolTip(tr_("Markup.Hint"));

	connect(_markupBox, &QCheckBox::toggled, this, [this](bool on) {
		if (_updating)
			return;

		// Switching mode keeps the words; inline styling only exists in the
		// mode it was made in.
		_markup = on;

		obs_data_t *data = obs_source_get_settings(_source);
		obs_data_set_bool(data, "markup", on);
		obs_data_release(data);

		commit();
		reload();
	});

	_clearFormatting = new QPushButton(tr_("Editor.ClearFormatting"), panel);
	connect(_clearFormatting, &QPushButton::clicked, this, &EditorWindow::clearFormatting);

	auto *row = new QHBoxLayout();
	row->addWidget(_scopeLabel, 1);
	row->addWidget(_markupBox);
	row->addWidget(_clearFormatting);

	layout->addWidget(_textEdit, 1);
	layout->addLayout(row);

	return panel;
}

QWidget *EditorWindow::buildFontPanel()
{
	auto *panel = new QWidget(this);
	auto *outer = new QVBoxLayout(panel);
	auto *form = new QFormLayout();

	_fontFamily = new QComboBox(panel);
	_fontFamily->setEditable(true);
	_fontFamily->setInsertPolicy(QComboBox::NoInsert);
	_fontFamily->completer()->setCompletionMode(QCompleter::PopupCompletion);
	_fontFamily->completer()->setFilterMode(Qt::MatchContains);

	populateFamilies();

	// Only a finished choice applies: typing "Ari" on the way to "Arial"
	// must not load three fonts.
	auto applyFamily = [this]() {
		if (_updating)
			return;

		const std::string family = _fontFamily->currentText().trimmed().toStdString();

		if (family.empty())
			return;

		StyleOverride delta;
		delta.family = family;
		delta.axes = AxisMap{};
		applyStyle(delta);
		rebuildAxisControls();
	};

	connect(_fontFamily, &QComboBox::activated, this, applyFamily);
	connect(_fontFamily->lineEdit(), &QLineEdit::editingFinished, this, applyFamily);

	auto *rescan = new QPushButton(tr_("Editor.RescanFonts"), panel);

	connect(rescan, &QPushButton::clicked, this, [this]() {
		FontManager::instance().refresh();
		_axisFontKey.clear();
		populateFamilies();
		syncControls();
	});

	auto *familyRow = new QHBoxLayout();
	familyRow->addWidget(_fontFamily, 1);
	familyRow->addWidget(rescan);

	_fontSize = new QSpinBox(panel);
	_fontSize->setRange(1, 2000);
	_fontSize->setSuffix(" px");
	_fontSize->setKeyboardTracking(false);

	connect(_fontSize, &QSpinBox::valueChanged, this, [this](int v) {
		StyleOverride d;
		d.size = float(v);
		applyStyle(d);
	});

	_fontWeight = new QComboBox(panel);

	for (const auto &[weight, key] : kWeights)
		_fontWeight->addItem(tr_(key), weight);

	connect(_fontWeight, &QComboBox::activated, this, [this]() {
		StyleOverride d;
		d.weight = _fontWeight->currentData().toInt();
		applyStyle(d);
	});

	auto boolField = [this, panel](const char *label, auto setter) {
		auto *box = new QCheckBox(tr_(label), panel);

		connect(box, &QCheckBox::toggled, this, [this, setter](bool on) {
			StyleOverride d;
			setter(d, on);
			applyStyle(d);
		});

		return box;
	};

	_fontItalic = boolField("Editor.Italic", [](StyleOverride &d, bool on) { d.italic = on; });
	_fontUnderline = boolField("Editor.Underline", [](StyleOverride &d, bool on) { d.underline = on; });
	_fontStrikeout = boolField("Editor.Strikeout", [](StyleOverride &d, bool on) { d.strikeout = on; });
	_tabularDigits = boolField("TabularDigits", [](StyleOverride &d, bool on) { d.tabularDigits = on; });
	_tabularDigits->setToolTip(tr_("TabularDigits.Hint"));

	_features = new QLineEdit(panel);
	_features->setPlaceholderText("smcp, -liga, ss01");

	connect(_features, &QLineEdit::editingFinished, this, [this]() {
		StyleOverride d;
		d.features = _features->text().toStdString();
		applyStyle(d);
	});

	_language = new QLineEdit(panel);
	_language->setPlaceholderText("en, tr, sr-Latn...");

	connect(_language, &QLineEdit::editingFinished, this, [this]() {
		StyleOverride d;
		d.language = _language->text().trimmed().toStdString();
		applyStyle(d);
	});

	form->addRow(tr_("Editor.Family"), familyRow);
	form->addRow(tr_("Editor.Size"), _fontSize);
	form->addRow(tr_("Editor.Weight"), _fontWeight);
	form->addRow(QString(), _fontItalic);
	form->addRow(QString(), _fontUnderline);
	form->addRow(QString(), _fontStrikeout);
	form->addRow(QString(), _tabularDigits);
	form->addRow(tr_("Editor.Features"), _features);
	form->addRow(tr_("Editor.Language"), _language);

	outer->addLayout(form);

	// Variable-font axes are read from the font, so each font exposes exactly
	// the axes it has.
	auto *axisBox = new QGroupBox(tr_("Editor.Axes"), panel);
	_axisLayout = new QFormLayout(axisBox);

	outer->addWidget(axisBox);
	outer->addStretch(1);

	auto *scroll = new QScrollArea(this);
	scroll->setWidget(panel);
	scroll->setWidgetResizable(true);

	return scroll;
}

QWidget *EditorWindow::buildFillPanel()
{
	auto *panel = new QWidget(this);
	auto *outer = new QVBoxLayout(panel);

	auto colorField = [this, panel](auto setter) {
		auto *button = new ColorButton(panel);

		connect(button, &ColorButton::colorChanged, this, [this, button, setter]() {
			StyleOverride d;
			setter(d, button->color());
			applyStyle(d);
		});

		return button;
	};

	auto floatField = [this, panel](double lo, double hi, double step, const char *suffix, auto setter) {
		auto *box = spin(panel, lo, hi, step, suffix);

		connect(box, &QDoubleSpinBox::valueChanged, this, [this, setter](double v) {
			StyleOverride d;
			setter(d, float(v));
			applyStyle(d);
		});

		return box;
	};

	// ---- fill ----
	auto *fillBox = new QGroupBox(tr_("Editor.Fill"), panel);
	auto *fillForm = new QFormLayout(fillBox);

	_fillType = enumCombo<FillType>(fillBox, {{FillType::Solid, "Fill.Solid"},
						  {FillType::Linear, "Fill.Linear"},
						  {FillType::Radial, "Fill.Radial"}});

	connect(_fillType, &QComboBox::activated, this, [this]() {
		StyleOverride d;
		d.fillType = comboValue<FillType>(_fillType);
		applyStyle(d);
		syncControls();
	});

	_fillColor = colorField([](StyleOverride &d, Rgba c) { d.color = c; });
	_fillColor2 = colorField([](StyleOverride &d, Rgba c) { d.color2 = c; });
	_gradientAngle =
		floatField(-180.0, 180.0, 5.0, "\xC2\xB0", [](StyleOverride &d, float v) { d.gradientAngle = v; });

	_gradientSpan = enumCombo<GradientSpan>(fillBox, {{GradientSpan::Text, "Fill.Span.Text"},
							  {GradientSpan::Character, "Fill.Span.Character"}});

	connect(_gradientSpan, &QComboBox::activated, this, [this]() {
		StyleOverride d;
		d.gradientSpan = comboValue<GradientSpan>(_gradientSpan);
		applyStyle(d);
	});

	fillForm->addRow(tr_("Fill"), _fillType);
	fillForm->addRow(tr_("Color"), _fillColor);
	fillForm->addRow(tr_("Fill.Color2"), _fillColor2);
	fillForm->addRow(tr_("Fill.Angle"), _gradientAngle);
	fillForm->addRow(tr_("Fill.Span"), _gradientSpan);

	// ---- outline ----
	auto *outlineBox = new QGroupBox(tr_("Outline"), panel);
	auto *outlineForm = new QFormLayout(outlineBox);

	_outlineEnabled = new QCheckBox(tr_("Editor.Enabled"), outlineBox);

	connect(_outlineEnabled, &QCheckBox::toggled, this, [this](bool on) {
		StyleOverride d;
		d.outline = on;
		applyStyle(d);
	});

	_outlineWidth = floatField(0.5, 100.0, 0.5, " px", [](StyleOverride &d, float v) { d.outlineWidth = v; });
	_outlineColor = colorField([](StyleOverride &d, Rgba c) { d.outlineColor = c; });

	_outlineJoin = enumCombo<LineJoin>(outlineBox, {{LineJoin::Round, "Outline.Join.Round"},
							{LineJoin::Miter, "Outline.Join.Miter"},
							{LineJoin::Bevel, "Outline.Join.Bevel"}});

	connect(_outlineJoin, &QComboBox::activated, this, [this]() {
		StyleOverride d;
		d.outlineJoin = comboValue<LineJoin>(_outlineJoin);
		applyStyle(d);
	});

	outlineForm->addRow(QString(), _outlineEnabled);
	outlineForm->addRow(tr_("Outline.Width"), _outlineWidth);
	outlineForm->addRow(tr_("Outline.Color"), _outlineColor);
	outlineForm->addRow(tr_("Outline.Join"), _outlineJoin);

	// ---- shadow ----
	auto *shadowBox = new QGroupBox(tr_("Shadow"), panel);
	auto *shadowForm = new QFormLayout(shadowBox);

	_shadowEnabled = new QCheckBox(tr_("Editor.Enabled"), shadowBox);

	connect(_shadowEnabled, &QCheckBox::toggled, this, [this](bool on) {
		StyleOverride d;
		d.shadow = on;
		applyStyle(d);
	});

	_shadowX = floatField(-500.0, 500.0, 1.0, " px", [](StyleOverride &d, float v) { d.shadowX = v; });
	_shadowY = floatField(-500.0, 500.0, 1.0, " px", [](StyleOverride &d, float v) { d.shadowY = v; });
	_shadowColor = colorField([](StyleOverride &d, Rgba c) { d.shadowColor = c; });

	shadowForm->addRow(QString(), _shadowEnabled);
	shadowForm->addRow(tr_("Shadow.X"), _shadowX);
	shadowForm->addRow(tr_("Shadow.Y"), _shadowY);
	shadowForm->addRow(tr_("Shadow.Color"), _shadowColor);

	// ---- spacing ----
	auto *spacingBox = new QGroupBox(tr_("Editor.Spacing"), panel);
	auto *spacingForm = new QFormLayout(spacingBox);

	_letterSpacing = floatField(-200.0, 200.0, 0.5, " px", [](StyleOverride &d, float v) { d.letterSpacing = v; });
	_wordSpacing = floatField(-200.0, 200.0, 0.5, " px", [](StyleOverride &d, float v) { d.wordSpacing = v; });

	spacingForm->addRow(tr_("LetterSpacing"), _letterSpacing);
	spacingForm->addRow(tr_("Editor.WordSpacing"), _wordSpacing);

	outer->addWidget(fillBox);
	outer->addWidget(outlineBox);
	outer->addWidget(shadowBox);
	outer->addWidget(spacingBox);
	outer->addStretch(1);

	auto *scroll = new QScrollArea(this);
	scroll->setWidget(panel);
	scroll->setWidgetResizable(true);

	return scroll;
}

QWidget *EditorWindow::buildLayoutPanel()
{
	auto *panel = new QWidget(this);
	auto *outer = new QVBoxLayout(panel);

	// ---- paragraph ----
	auto *paragraphBox = new QGroupBox(tr_("Editor.Paragraph"), panel);
	auto *paragraphForm = new QFormLayout(paragraphBox);

	_align = enumCombo<HAlign>(paragraphBox, {{HAlign::Start, "Align.Start"},
						  {HAlign::Center, "Align.Center"},
						  {HAlign::End, "Align.End"},
						  {HAlign::Justify, "Align.Justify"}});
	_align->setToolTip(tr_("Align.Hint"));

	connect(_align, &QComboBox::activated, this, [this]() {
		ParagraphOverride d;
		d.align = comboValue<HAlign>(_align);
		applyParagraph(d);
	});

	_direction = enumCombo<Direction>(paragraphBox, {{Direction::Auto, "Direction.Auto"},
							 {Direction::LTR, "Direction.LTR"},
							 {Direction::RTL, "Direction.RTL"}});

	connect(_direction, &QComboBox::activated, this, [this]() {
		ParagraphOverride d;
		d.direction = comboValue<Direction>(_direction);
		applyParagraph(d);
	});

	auto paragraphFloat = [this, paragraphBox](double lo, double hi, double step, const char *suffix, auto setter) {
		auto *box = spin(paragraphBox, lo, hi, step, suffix, 2);

		connect(box, &QDoubleSpinBox::valueChanged, this, [this, setter](double v) {
			ParagraphOverride d;
			setter(d, float(v));
			applyParagraph(d);
		});

		return box;
	};

	_lineHeight = paragraphFloat(0.3, 5.0, 0.05, nullptr, [](ParagraphOverride &d, float v) { d.lineHeight = v; });
	_spaceBefore =
		paragraphFloat(0.0, 1000.0, 1.0, " px", [](ParagraphOverride &d, float v) { d.spaceBefore = v; });
	_spaceAfter = paragraphFloat(0.0, 1000.0, 1.0, " px", [](ParagraphOverride &d, float v) { d.spaceAfter = v; });

	paragraphForm->addRow(tr_("Align"), _align);
	paragraphForm->addRow(tr_("Direction"), _direction);
	paragraphForm->addRow(tr_("LineHeight"), _lineHeight);
	paragraphForm->addRow(tr_("Editor.SpaceBefore"), _spaceBefore);
	paragraphForm->addRow(tr_("Editor.SpaceAfter"), _spaceAfter);

	// ---- box ----
	auto *boxGroup = new QGroupBox(tr_("Editor.Box"), panel);
	auto *boxForm = new QFormLayout(boxGroup);

	_fixedSize = new QCheckBox(tr_("Extents"), boxGroup);
	connect(_fixedSize, &QCheckBox::toggled, this, [this](bool on) {
		applyDocument([on](Document &d) { d.sizeMode = on ? SizeMode::Fixed : SizeMode::Auto; });
	});

	_boxWidth = new QSpinBox(boxGroup);
	_boxWidth->setRange(1, 16384);
	_boxWidth->setKeyboardTracking(false);
	connect(_boxWidth, &QSpinBox::valueChanged, this,
		[this](int v) { applyDocument([v](Document &d) { d.boxWidth = float(v); }); });

	_boxHeight = new QSpinBox(boxGroup);
	_boxHeight->setRange(1, 16384);
	_boxHeight->setKeyboardTracking(false);
	connect(_boxHeight, &QSpinBox::valueChanged, this,
		[this](int v) { applyDocument([v](Document &d) { d.boxHeight = float(v); }); });

	_wrap = enumCombo<WrapMode>(boxGroup, {{WrapMode::None, "Wrap.None"},
					       {WrapMode::Word, "Wrap.Word"},
					       {WrapMode::Character, "Wrap.Character"}});
	connect(_wrap, &QComboBox::activated, this,
		[this]() { applyDocument([this](Document &d) { d.wrap = comboValue<WrapMode>(_wrap); }); });

	_valign = enumCombo<VAlign>(boxGroup, {{VAlign::Top, "VAlign.Top"},
					       {VAlign::Middle, "VAlign.Middle"},
					       {VAlign::Bottom, "VAlign.Bottom"}});
	_valign->setToolTip(tr_("VAlign.Hint"));
	connect(_valign, &QComboBox::activated, this,
		[this]() { applyDocument([this](Document &d) { d.valign = comboValue<VAlign>(_valign); }); });

	_padding = spin(boxGroup, 0.0, 1000.0, 1.0, " px");
	connect(_padding, &QDoubleSpinBox::valueChanged, this,
		[this](double v) { applyDocument([v](Document &d) { d.padding = float(v); }); });

	_opacity = new QSpinBox(boxGroup);
	_opacity->setRange(0, 100);
	_opacity->setSuffix(" %");
	_opacity->setKeyboardTracking(false);
	connect(_opacity, &QSpinBox::valueChanged, this,
		[this](int v) { applyDocument([v](Document &d) { d.opacity = float(v) / 100.0f; }); });

	boxForm->addRow(QString(), _fixedSize);
	boxForm->addRow(tr_("Extents.Width"), _boxWidth);
	boxForm->addRow(tr_("Extents.Height"), _boxHeight);
	boxForm->addRow(tr_("Wrap"), _wrap);
	boxForm->addRow(tr_("VAlign"), _valign);
	boxForm->addRow(tr_("Padding"), _padding);
	boxForm->addRow(tr_("Opacity"), _opacity);

	// ---- background ----
	auto *backgroundGroup = new QGroupBox(tr_("Background"), panel);
	auto *backgroundForm = new QFormLayout(backgroundGroup);

	_background = new QCheckBox(tr_("Editor.Enabled"), backgroundGroup);
	connect(_background, &QCheckBox::toggled, this,
		[this](bool on) { applyDocument([on](Document &d) { d.background.enabled = on; }); });

	_backgroundColor = new ColorButton(backgroundGroup);
	connect(_backgroundColor, &ColorButton::colorChanged, this,
		[this]() { applyDocument([this](Document &d) { d.background.color = _backgroundColor->color(); }); });

	_backgroundRadius = spin(backgroundGroup, 0.0, 500.0, 1.0, " px");
	connect(_backgroundRadius, &QDoubleSpinBox::valueChanged, this,
		[this](double v) { applyDocument([v](Document &d) { d.background.radius = float(v); }); });

	backgroundForm->addRow(QString(), _background);
	backgroundForm->addRow(tr_("Background.Color"), _backgroundColor);
	backgroundForm->addRow(tr_("Background.Radius"), _backgroundRadius);

	outer->addWidget(paragraphBox);
	outer->addWidget(boxGroup);
	outer->addWidget(backgroundGroup);
	outer->addStretch(1);

	auto *scroll = new QScrollArea(this);
	scroll->setWidget(panel);
	scroll->setWidgetResizable(true);

	return scroll;
}

QWidget *EditorWindow::buildMotionPanel()
{
	auto *panel = new QWidget(this);
	auto *form = new QFormLayout(panel);

	_motionForm = form;

	auto motionCombo = [this](QComboBox *combo, auto setter) {
		connect(combo, &QComboBox::activated, this, [this, combo, setter]() {
			applyDocument([&](Document &d) { setter(d.motion, combo); });
			syncMotionVisibility();
		});

		return combo;
	};

	auto motionFloat = [this, panel](double lo, double hi, double step, const char *suffix, int decimals,
					 auto setter) {
		auto *box = spin(panel, lo, hi, step, suffix, decimals);

		connect(box, &QDoubleSpinBox::valueChanged, this,
			[this, setter](double v) { applyDocument([&](Document &d) { setter(d.motion, float(v)); }); });

		return box;
	};

	_motion = motionCombo(enumCombo<Motion>(panel, {{Motion::None, "Motion.None"},
							{Motion::Fade, "Motion.Fade"},
							{Motion::Slide, "Motion.Slide"},
							{Motion::Pop, "Motion.Pop"},
							{Motion::Typewriter, "Motion.Typewriter"},
							{Motion::Wave, "Motion.Wave"},
							{Motion::Blur, "Motion.Blur"},
							{Motion::Spin, "Motion.Spin"},
							{Motion::Bounce, "Motion.Bounce"}}),
			      [this](MotionSpec &m, QComboBox *c) {
				      m.motion = comboValue<Motion>(c);

				      // A value meant for another preset is replaced by this
				      // preset's own default: 48 px is a slide, not a scale.
				      const MotionParamSpec amount = motionParamSpec(m.motion);

				      if (amount.label && (m.param < amount.min || m.param > amount.max))
					      m.param = amount.defaultValue;
			      });

	_motionOrder = motionCombo(enumCombo<MotionOrder>(panel, {{MotionOrder::Together, "Motion.Order.Together"},
								  {MotionOrder::PerCharacter, "Motion.Order.Character"},
								  {MotionOrder::PerWord, "Motion.Order.Word"},
								  {MotionOrder::PerLine, "Motion.Order.Line"}}),
				   [](MotionSpec &m, QComboBox *c) { m.order = comboValue<MotionOrder>(c); });

	_motionDirection =
		motionCombo(enumCombo<MotionDirection>(panel, {{MotionDirection::Up, "Motion.Direction.Up"},
							       {MotionDirection::Down, "Motion.Direction.Down"},
							       {MotionDirection::Left, "Motion.Direction.Left"},
							       {MotionDirection::Right, "Motion.Direction.Right"}}),
			    [](MotionSpec &m, QComboBox *c) { m.direction = comboValue<MotionDirection>(c); });

	_motionDuration = motionFloat(0.01, 10.0, 0.05, " s", 2, [](MotionSpec &m, float v) { m.duration = v; });
	_motionStagger = motionFloat(0.0, 2.0, 0.005, " s", 3, [](MotionSpec &m, float v) { m.stagger = v; });
	_motionParam = motionFloat(-1000.0, 1000.0, 1.0, nullptr, 2, [](MotionSpec &m, float v) { m.param = v; });
	_motionParamLabel = new QLabel(tr_("Motion.Amount"), panel);
	_motionSpeed = motionFloat(0.05, 5.0, 0.05, " Hz", 2, [](MotionSpec &m, float v) { m.speed = v; });

	_motionTrigger =
		motionCombo(enumCombo<MotionTrigger>(panel, {{MotionTrigger::Never, "Motion.Replay.Never"},
							     {MotionTrigger::TextChange, "Motion.Replay.TextChange"},
							     {MotionTrigger::Show, "Motion.Replay.Show"},
							     {MotionTrigger::Activate, "Motion.Replay.Activate"},
							     {MotionTrigger::Loop, "Motion.Replay.Loop"}}),
			    [](MotionSpec &m, QComboBox *c) { m.trigger = comboValue<MotionTrigger>(c); });

	_motionScope = motionCombo(enumCombo<MotionScope>(panel, {{MotionScope::Changed, "Motion.Scope.Changed"},
								  {MotionScope::All, "Motion.Scope.All"}}),
				   [](MotionSpec &m, QComboBox *c) { m.scope = comboValue<MotionScope>(c); });

	_motionExit = new QCheckBox(tr_("Motion.Exit"), panel);
	connect(_motionExit, &QCheckBox::toggled, this,
		[this](bool on) { applyDocument([on](Document &d) { d.motion.exit = on; }); });

	_motionLoopInterval =
		motionFloat(0.1, 3600.0, 0.5, " s", 1, [](MotionSpec &m, float v) { m.loopInterval = v; });

	auto *replay = new QPushButton(tr_("Motion.ReplayNow"), panel);
	connect(replay, &QPushButton::clicked, this, [this]() {
		calldata_t call = {};
		proc_handler_call(obs_source_get_proc_handler(_source), "replay_motion", &call);
		calldata_free(&call);
	});

	_scrollEnabled = new QCheckBox(tr_("Scroll"), panel);
	connect(_scrollEnabled, &QCheckBox::toggled, this,
		[this](bool on) { applyDocument([on](Document &d) { d.scroll.enabled = on; }); });

	auto scrollFloat = [this, panel](double lo, double hi, auto setter) {
		auto *box = spin(panel, lo, hi, 5.0, " px/s");

		connect(box, &QDoubleSpinBox::valueChanged, this,
			[this, setter](double v) { applyDocument([&](Document &d) { setter(d.scroll, float(v)); }); });

		return box;
	};

	_scrollX = scrollFloat(-4000.0, 4000.0, [](ScrollSpec &s, float v) { s.speedX = v; });
	_scrollY = scrollFloat(-4000.0, 4000.0, [](ScrollSpec &s, float v) { s.speedY = v; });

	_scrollLoop = new QCheckBox(tr_("Scroll.Loop"), panel);
	connect(_scrollLoop, &QCheckBox::toggled, this,
		[this](bool on) { applyDocument([on](Document &d) { d.scroll.loop = on; }); });

	_scrollGap = spin(panel, 0.0, 4000.0, 4.0, " px");
	connect(_scrollGap, &QDoubleSpinBox::valueChanged, this,
		[this](double v) { applyDocument([v](Document &d) { d.scroll.gap = float(v); }); });

	form->addRow(tr_("Motion"), _motion);
	form->addRow(tr_("Motion.Order"), _motionOrder);
	form->addRow(tr_("Motion.Direction"), _motionDirection);
	form->addRow(tr_("Motion.Duration"), _motionDuration);
	form->addRow(tr_("Motion.Stagger"), _motionStagger);
	form->addRow(_motionParamLabel, _motionParam);
	form->addRow(tr_("Motion.Speed"), _motionSpeed);
	form->addRow(tr_("Motion.Replay"), _motionTrigger);
	form->addRow(tr_("Motion.Scope"), _motionScope);
	form->addRow(QString(), _motionExit);
	form->addRow(tr_("Motion.Replay.Interval"), _motionLoopInterval);
	form->addRow(QString(), replay);
	form->addRow(QString(), _scrollEnabled);
	form->addRow(tr_("Scroll.X"), _scrollX);
	form->addRow(tr_("Scroll.Y"), _scrollY);
	form->addRow(QString(), _scrollLoop);
	form->addRow(tr_("Scroll.Gap"), _scrollGap);

	auto *scroll = new QScrollArea(this);
	scroll->setWidget(panel);
	scroll->setWidgetResizable(true);

	return scroll;
}

namespace {

// A two-column name/value table with Add and Remove beneath it.
QWidget *buildVariableTable(QWidget *parent, const QString &title, const QString &hint, QTableWidget *&table,
			    const std::function<void()> &onChanged)
{
	auto *box = new QGroupBox(title, parent);
	auto *layout = new QVBoxLayout(box);

	auto *caption = new QLabel(hint, box);
	caption->setWordWrap(true);

	table = new QTableWidget(0, 2, box);
	table->setHorizontalHeaderLabels({tr_("Variables.Name"), tr_("Variables.Value")});
	table->horizontalHeader()->setStretchLastSection(true);
	table->verticalHeader()->setVisible(false);
	table->setSelectionBehavior(QAbstractItemView::SelectRows);

	auto *buttons = new QHBoxLayout();
	auto *add = new QPushButton(tr_("Variables.Add"), box);
	auto *remove = new QPushButton(tr_("Variables.Remove"), box);

	QObject::connect(add, &QPushButton::clicked, table, [table]() {
		const int row = table->rowCount();

		table->insertRow(row);
		table->setItem(row, 0, new QTableWidgetItem());
		table->setItem(row, 1, new QTableWidgetItem());
		table->editItem(table->item(row, 0));
	});

	QObject::connect(remove, &QPushButton::clicked, table, [table, onChanged]() {
		const int row = table->currentRow();

		if (row < 0)
			return;

		table->removeRow(row);
		onChanged();
	});

	// A row with no name yet is skipped rather than stored.
	QObject::connect(table, &QTableWidget::itemChanged, table, [onChanged](QTableWidgetItem *) { onChanged(); });

	buttons->addWidget(add);
	buttons->addWidget(remove);
	buttons->addStretch(1);

	layout->addWidget(caption);
	layout->addWidget(table, 1);
	layout->addLayout(buttons);

	return box;
}

} // namespace

QWidget *EditorWindow::buildVariablesPanel()
{
	auto *panel = new QWidget(this);
	auto *layout = new QVBoxLayout(panel);

	auto *hint = new QLabel(tr_("Variables.Help"), panel);
	hint->setWordWrap(true);

	layout->addWidget(hint);

	layout->addWidget(buildVariableTable(panel, tr_("Variables.Source"), tr_("Variables.Source.Hint"),
					     _sourceVariables, [this]() { onVariablesChanged(); }));

	layout->addWidget(buildVariableTable(panel, tr_("Variables.Global"), tr_("Variables.Global.Hint"),
					     _globalVariables, [this]() { onVariablesChanged(); }));

	auto *timerRow = new QHBoxLayout();

	auto procButton = [this, panel](const char *label, const char *proc) {
		auto *button = new QPushButton(tr_(label), panel);

		connect(button, &QPushButton::clicked, this, [this, proc]() {
			calldata_t call = {};
			proc_handler_call(obs_source_get_proc_handler(_source), proc, &call);
			calldata_free(&call);
		});

		return button;
	};

	timerRow->addWidget(new QLabel(tr_("Editor.Timer"), panel));
	timerRow->addWidget(procButton("Timer.Toggle", "timer_toggle"));
	timerRow->addWidget(procButton("Timer.Reset", "timer_reset"));
	timerRow->addStretch(1);

	layout->addLayout(timerRow);

	return panel;
}

void EditorWindow::populateFamilies()
{
	const QString current = _fontFamily->currentText();
	const bool blocked = _fontFamily->blockSignals(true);

	_fontFamily->clear();

	for (const std::string &family : fontenum::families())
		_fontFamily->addItem(QString::fromStdString(family));

	_fontFamily->setCurrentText(current);
	_fontFamily->blockSignals(blocked);
}

void EditorWindow::rebuildAxisControls()
{
	size_t start = 0;
	size_t end = 0;

	const Style style = selectionRange(start, end) ? _document.styleAt(start + 1) : _document.base;

	// Reading axes needs the font lock, which the rebuild worker may hold;
	// only do it when the font actually changed.
	const std::string key =
		style.font.family + "\n" + style.font.file + "\n" + std::to_string(style.font.faceIndex);

	if (key == _axisFontKey) {
		const bool wasUpdating = _updating;
		_updating = true;

		for (auto &[tag, slider] : _axisSliders) {
			const auto it = style.font.axes.find(tag);

			if (it != style.font.axes.end())
				slider->setValue(int(it->second * 100.0f));
		}

		_updating = wasUpdating;
		return;
	}

	_axisFontKey = key;

	while (_axisLayout->rowCount() > 0)
		_axisLayout->removeRow(0);

	_axisSliders.clear();

	const std::vector<FontAxis> axes = FontManager::instance().axesOf(style.font);

	for (const FontAxis &axis : axes) {
		// The weight axis follows the Weight control.
		if (axis.tag == "wght")
			continue;

		auto *slider = new QSlider(Qt::Horizontal);

		// Sliders are integral; a factor of 100 keeps useful precision on
		// small ranges like slant (-10..0).
		slider->setRange(int(axis.minValue * 100.0f), int(axis.maxValue * 100.0f));

		const auto it = style.font.axes.find(axis.tag);
		slider->setValue(int((it != style.font.axes.end() ? it->second : axis.defaultValue) * 100.0f));

		const std::string tag = axis.tag;

		connect(slider, &QSlider::valueChanged, this, [this, tag](int raw) {
			if (_updating)
				return;

			size_t s = 0;
			size_t e = 0;

			AxisMap axesValue = selectionRange(s, e) ? _document.styleAt(s + 1).font.axes
								 : _document.base.font.axes;
			axesValue[tag] = float(raw) / 100.0f;

			StyleOverride d;
			d.axes = axesValue;
			applyStyle(d);
		});

		_axisSliders[axis.tag] = slider;
		_axisLayout->addRow(QString::fromStdString(axis.name), slider);
	}
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

bool EditorWindow::selectionRange(size_t &start, size_t &end) const
{
	if (_markup || !_textEdit)
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
}

void EditorWindow::applyDocument(const std::function<void(Document &)> &change)
{
	if (_updating)
		return;

	change(_document);
	commit();
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

void EditorWindow::onVariablesChanged()
{
	if (_updating || _detached)
		return;

	// The shared table is not part of any source; it is written straight
	// through and saved with the module's own config.
	GlobalVariables::replace(tableToMap(_globalVariables));
	settings::saveGlobalVariables();

	obs_data_t *data = obs_source_get_settings(_source);

	settings::variablesToData(data, settings::kVariablesKey, tableToMap(_sourceVariables));
	_lastCommitted = settingsJson(data);

	obs_source_update(_source, data);
	obs_data_release(data);
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

void EditorWindow::commit()
{
	if (_detached)
		return;

	obs_data_t *data = obs_source_get_settings(_source);

	if (!data)
		return;

	const std::string before = settingsJson(data);

	settings::save(data, _document, _markup);

	if (_markup)
		obs_data_set_string(data, "text", _textEdit->toPlainText().toStdString().c_str());

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
	_lastCommitted = settingsJson(data);

	mapToTable(_sourceVariables, config.variables);
	mapToTable(_globalVariables, GlobalVariables::snapshot());

	obs_data_release(data);

	const QString text = _markup ? QString::fromStdString(config.text)
				     : QString::fromStdString(_document.plainText());

	_updating = true;

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

	_updating = false;

	syncControls();
}

void EditorWindow::syncControls()
{
	_updating = true;

	size_t start = 0;
	size_t end = 0;

	const bool selection = selectionRange(start, end);

	// The controls show what they edit: the selection's first character, or
	// the base style.
	const Style style = selection ? _document.styleAt(start + 1) : _document.base;
	const ParagraphStyle paragraph = selection ? _document.paragraphAt(start) : _document.paragraph;

	if (_markup)
		_scopeLabel->setText(tr_("Editor.Scope.Markup"));
	else if (selection)
		_scopeLabel->setText(tr_("Editor.Scope.Selection"));
	else
		_scopeLabel->setText(tr_("Editor.Scope.All"));

	_clearFormatting->setEnabled(selection);

	_fontFamily->setCurrentText(QString::fromStdString(style.font.family));
	_fontSize->setValue(int(std::lround(style.sizePx)));

	int nearest = 0;

	for (int i = 0; i < _fontWeight->count(); i++)
		if (std::abs(_fontWeight->itemData(i).toInt() - style.font.weight) <
		    std::abs(_fontWeight->itemData(nearest).toInt() - style.font.weight))
			nearest = i;

	_fontWeight->setCurrentIndex(nearest);
	_fontItalic->setChecked(style.font.italic);
	_fontUnderline->setChecked(style.underline);
	_fontStrikeout->setChecked(style.strikeout);
	_tabularDigits->setChecked(style.tabularDigits);
	_features->setText(QString::fromStdString(style.features));
	_language->setText(QString::fromStdString(style.language));

	setCombo(_fillType, int(style.fill.type));
	_fillColor->setColor(style.fill.color);
	_fillColor2->setColor(style.fill.color2);
	_gradientAngle->setValue(double(style.fill.angleDeg));
	setCombo(_gradientSpan, int(style.fill.span));

	const bool gradient = style.fill.type != FillType::Solid;

	_fillColor2->setEnabled(gradient);
	_gradientAngle->setEnabled(style.fill.type == FillType::Linear);
	_gradientSpan->setEnabled(gradient);

	_outlineEnabled->setChecked(style.outline.enabled);
	_outlineWidth->setValue(double(style.outline.widthPx));
	_outlineColor->setColor(style.outline.color);
	setCombo(_outlineJoin, int(style.outline.join));

	_shadowEnabled->setChecked(style.shadow.enabled);
	_shadowX->setValue(double(style.shadow.offsetX));
	_shadowY->setValue(double(style.shadow.offsetY));
	_shadowColor->setColor(style.shadow.color);

	_letterSpacing->setValue(double(style.letterSpacing));
	_wordSpacing->setValue(double(style.wordSpacing));

	setCombo(_align, int(paragraph.align));
	setCombo(_direction, int(paragraph.direction));
	_lineHeight->setValue(double(paragraph.lineHeight));
	_spaceBefore->setValue(double(paragraph.spaceBefore));
	_spaceAfter->setValue(double(paragraph.spaceAfter));

	setCombo(_valign, int(_document.valign));
	setCombo(_wrap, int(_document.wrap));
	_fixedSize->setChecked(_document.sizeMode == SizeMode::Fixed);
	_boxWidth->setValue(int(std::lround(_document.boxWidth)));
	_boxHeight->setValue(int(std::lround(_document.boxHeight)));
	_padding->setValue(double(_document.padding));
	_opacity->setValue(std::clamp(int(std::lround(_document.opacity * 100.0f)), 0, 100));
	_background->setChecked(_document.background.enabled);
	_backgroundColor->setColor(_document.background.color);
	_backgroundRadius->setValue(double(_document.background.radius));

	const MotionSpec &m = _document.motion;

	setCombo(_motion, int(m.motion));
	setCombo(_motionOrder, int(m.order));
	setCombo(_motionDirection, int(m.direction));
	_motionDuration->setValue(double(m.duration));
	_motionStagger->setValue(double(m.stagger));
	_motionSpeed->setValue(double(m.speed));
	setCombo(_motionTrigger, int(m.trigger));
	setCombo(_motionScope, int(m.scope));
	_motionExit->setChecked(m.exit);
	_motionLoopInterval->setValue(double(m.loopInterval));

	_scrollEnabled->setChecked(_document.scroll.enabled);
	_scrollX->setValue(double(_document.scroll.speedX));
	_scrollY->setValue(double(_document.scroll.speedY));
	_scrollLoop->setChecked(_document.scroll.loop);
	_scrollGap->setValue(double(_document.scroll.gap));

	syncMotionVisibility();

	_updating = false;

	rebuildAxisControls();
}

void EditorWindow::syncMotionVisibility()
{
	const bool wasUpdating = _updating;
	_updating = true;

	const MotionSpec &m = _document.motion;

	const bool animated = m.motion != Motion::None;
	const bool timed = motionIsTransient(m.motion);
	const bool onText = timed && m.trigger == MotionTrigger::TextChange;
	const MotionParamSpec amount = motionParamSpec(m.motion);

	setRowVisible(_motionForm, _motionOrder, animated);
	setRowVisible(_motionForm, _motionDirection, motionHasDirection(m.motion));
	setRowVisible(_motionForm, _motionDuration, timed);
	setRowVisible(_motionForm, _motionStagger, animated && m.order != MotionOrder::Together);
	setRowVisible(_motionForm, _motionSpeed, m.motion == Motion::Wave);
	setRowVisible(_motionForm, _motionTrigger, timed);
	setRowVisible(_motionForm, _motionScope, onText);
	setRowVisible(_motionForm, _motionExit, onText);
	setRowVisible(_motionForm, _motionLoopInterval, timed && m.trigger == MotionTrigger::Loop);
	setRowVisible(_motionForm, _motionParam, amount.label != nullptr);

	if (amount.label) {
		_motionParamLabel->setText(tr_(amount.label));
		_motionParam->setRange(double(amount.min), double(amount.max));
		_motionParam->setSingleStep(double(amount.step));
		_motionParam->setValue(double(m.param));
	}

	const bool scrolling = _document.scroll.enabled;

	setRowVisible(_motionForm, _scrollX, scrolling);
	setRowVisible(_motionForm, _scrollY, scrolling);
	setRowVisible(_motionForm, _scrollLoop, scrolling);
	setRowVisible(_motionForm, _scrollGap, scrolling && _document.scroll.loop);

	_updating = wasUpdating;
}

VariableMap EditorWindow::tableToMap(QTableWidget *table)
{
	VariableMap out;

	for (int row = 0; row < table->rowCount(); row++) {
		const QTableWidgetItem *name = table->item(row, 0);
		const QTableWidgetItem *value = table->item(row, 1);

		if (!name)
			continue;

		const std::string key = normaliseVariableName(name->text().toStdString());

		if (!key.empty())
			out[key] = value ? value->text().toStdString() : std::string();
	}

	return out;
}

void EditorWindow::mapToTable(QTableWidget *table, const VariableMap &values)
{
	const bool blocked = table->blockSignals(true);

	table->setRowCount(0);

	for (const auto &[name, value] : values) {
		const int row = table->rowCount();

		table->insertRow(row);
		table->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(name)));
		table->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(value)));
	}

	table->blockSignals(blocked);
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
