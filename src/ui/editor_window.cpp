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
#include "../text/font_manager.hpp"
#include "../text/font_enum.hpp"
#include "../util/log.hpp"

#include <algorithm>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QVBoxLayout>

#include <obs-frontend-api.h>
#include <obs-module.h>

namespace slugged {

namespace {

QString tr_(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

// Applies `fn` to every style overlapping [start, end) in the flattened text.
//
// Runs are split at the selection boundaries so styling applies to exactly the
// selected characters. Adjacent runs are deliberately not merged back together
// afterwards: shaping treats them as one item anyway when their styles match,
// and merging would need a deep equality over the whole Style tree for no
// visible gain.
void applyToRange(Document &doc, size_t start, size_t end, const std::function<void(Style &)> &fn)
{
	size_t offset = 0;

	for (Block &block : doc.blocks) {
		std::vector<Run> rebuilt;

		for (Run &run : block.runs) {
			const size_t runStart = offset;
			const size_t runEnd = offset + run.text.size();

			offset = runEnd;

			const size_t from = std::max(start, runStart);
			const size_t to = std::min(end, runEnd);

			if (from >= to) {
				rebuilt.push_back(run);
				continue;
			}

			if (from > runStart)
				rebuilt.push_back(Run{run.text.substr(0, from - runStart), run.style});

			Run middle{run.text.substr(from - runStart, to - from), run.style};
			fn(middle.style);
			rebuilt.push_back(std::move(middle));

			if (to < runEnd)
				rebuilt.push_back(Run{run.text.substr(to - runStart), run.style});
		}

		block.runs = std::move(rebuilt);

		// The newline that separates blocks occupies one offset.
		offset += 1;
	}
}

int weightToIndex(int weight)
{
	if (weight <= 300)
		return 0;
	if (weight <= 400)
		return 1;
	if (weight <= 500)
		return 2;
	if (weight <= 600)
		return 3;
	if (weight <= 700)
		return 4;

	return 5;
}

int indexToWeight(int index)
{
	static const int weights[] = {300, 400, 500, 600, 700, 900};

	return weights[std::clamp(index, 0, 5)];
}

} // namespace

EditorWindow::EditorWindow(obs_source_t *source, QWidget *parent) : QMainWindow(parent), _source(source)
{
	setWindowTitle(tr_("Editor.Title") + " - " + QString::fromUtf8(obs_source_get_name(source)));
	setAttribute(Qt::WA_DeleteOnClose);
	resize(1180, 720);

	buildUi();
	reload();
}

EditorWindow::~EditorWindow() = default;

void EditorWindow::reload()
{
	obs_data_t *data = obs_source_get_settings(_source);

	if (data) {
		obs_data_t *documentData = obs_data_get_obj(data, "document");

		if (documentData) {
			settings::documentFromData(documentData, _document);
			obs_data_release(documentData);
		}

		obs_data_release(data);
	}

	if (_document.blocks.empty())
		_document.setPlainText("");

	syncControls();
}

void EditorWindow::apply()
{
	obs_data_t *data = obs_source_get_settings(_source);

	if (!data)
		return;

	settings::save(data, _document);
	obs_source_update(_source, data);

	obs_data_release(data);
}

const Style &EditorWindow::styleAtCursor() const
{
	if (!_textEdit)
		return _document.defaultStyle;

	const size_t cursor = size_t(_textEdit->textCursor().position());

	size_t offset = 0;

	for (const Block &block : _document.blocks) {
		for (const Run &run : block.runs) {
			const size_t runEnd = offset + run.text.size();

			// A cursor sitting at a run boundary reports the run to its
			// left, which matches how text editors behave.
			if (cursor >= offset && cursor <= runEnd && !run.text.empty())
				return run.style;

			offset = runEnd;
		}

		offset += 1;
	}

	if (!_document.blocks.empty() && !_document.blocks.front().runs.empty())
		return _document.blocks.front().runs.front().style;

	return _document.defaultStyle;
}

void EditorWindow::applyToSelection(const std::function<void(Style &)> &fn)
{
	if (_updating)
		return;

	QTextCursor cursor = _textEdit->textCursor();

	size_t start = 0;
	size_t end = SIZE_MAX;

	if (cursor.hasSelection()) {
		start = size_t(cursor.selectionStart());
		end = size_t(cursor.selectionEnd());
	}

	applyToRange(_document, start, end, fn);

	// The default style follows a whole-document edit so newly typed text
	// inherits it.
	if (!cursor.hasSelection())
		fn(_document.defaultStyle);

	apply();
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

void EditorWindow::buildUi()
{
	auto *splitter = new QSplitter(Qt::Horizontal, this);

	// ---- preview -------------------------------------------------------
	auto *previewHost = new QWidget(splitter);
	auto *previewLayout = new QVBoxLayout(previewHost);

	_preview = new PreviewWidget(_source, previewHost);

	previewLayout->addWidget(_preview, 1);

	if (!_preview->isLive()) {
		// Wayland and any other platform without a native window handle: say
		// so plainly instead of showing a black rectangle.
		auto *notice = new QLabel(tr_("Editor.PreviewUnavailable"), previewHost);

		notice->setWordWrap(true);
		notice->setAlignment(Qt::AlignCenter);

		previewLayout->addWidget(notice);
	}

	auto *zoomRow = new QHBoxLayout();
	auto *zoomLabel = new QLabel(tr_("Editor.Zoom"), previewHost);
	auto *zoom = new QSlider(Qt::Horizontal, previewHost);

	zoom->setRange(10, 400);
	zoom->setValue(100);

	connect(zoom, &QSlider::valueChanged, this, [this](int value) {
		if (_preview)
			_preview->setZoom(float(value) / 100.0f);
	});

	zoomRow->addWidget(zoomLabel);
	zoomRow->addWidget(zoom, 1);
	previewLayout->addLayout(zoomRow);

	splitter->addWidget(previewHost);

	// ---- inspector -----------------------------------------------------
	auto *tabs = new QTabWidget(splitter);

	tabs->addTab(buildTextPanel(), tr_("Editor.Tab.Text"));
	tabs->addTab(buildFontPanel(), tr_("Editor.Tab.Font"));
	tabs->addTab(buildFillPanel(), tr_("Editor.Tab.Style"));
	tabs->addTab(buildLayoutPanel(), tr_("Editor.Tab.Layout"));
	tabs->addTab(buildMotionPanel(), tr_("Editor.Tab.Motion"));

	splitter->addWidget(tabs);
	splitter->setStretchFactor(0, 3);
	splitter->setStretchFactor(1, 2);

	setCentralWidget(splitter);
}

QWidget *EditorWindow::buildTextPanel()
{
	auto *panel = new QWidget(this);
	auto *layout = new QVBoxLayout(panel);

	auto *hint = new QLabel(tr_("Editor.SelectionHint"), panel);
	hint->setWordWrap(true);

	_textEdit = new QPlainTextEdit(panel);
	_textEdit->setTabChangesFocus(true);

	connect(_textEdit, &QPlainTextEdit::textChanged, this, &EditorWindow::onTextChanged);

	// Moving the cursor re-reads the style under it so the inspector always
	// reflects the text the user is looking at.
	connect(_textEdit, &QPlainTextEdit::cursorPositionChanged, this, [this]() {
		if (!_updating)
			syncControls();
	});

	layout->addWidget(hint);
	layout->addWidget(_textEdit, 1);

	return panel;
}

QWidget *EditorWindow::buildFontPanel()
{
	auto *panel = new QWidget(this);
	auto *outer = new QVBoxLayout(panel);
	auto *form = new QFormLayout();

	_fontFamily = new QComboBox(panel);
	_fontFamily->setEditable(true);

	for (const std::string &family : fontenum::families())
		_fontFamily->addItem(QString::fromStdString(family));

	connect(_fontFamily, &QComboBox::currentTextChanged, this, &EditorWindow::onFontFamilyChanged);

	_fontSize = new QSpinBox(panel);
	_fontSize->setRange(1, 2000);
	_fontSize->setSuffix(" px");

	connect(_fontSize, &QSpinBox::valueChanged, this, &EditorWindow::onStyleChanged);

	_fontWeight = new QComboBox(panel);
	_fontWeight->addItems({"Light", "Regular", "Medium", "Semibold", "Bold", "Black"});

	connect(_fontWeight, &QComboBox::currentIndexChanged, this, &EditorWindow::onStyleChanged);

	_fontItalic = new QCheckBox(tr_("Editor.Italic"), panel);

	connect(_fontItalic, &QCheckBox::toggled, this, &EditorWindow::onStyleChanged);

	form->addRow(tr_("Editor.Family"), _fontFamily);
	form->addRow(tr_("Editor.Size"), _fontSize);
	form->addRow(tr_("Editor.Weight"), _fontWeight);
	form->addRow(QString(), _fontItalic);

	outer->addLayout(form);

	// Variable-font axes are discovered from the face and rebuilt whenever the
	// family changes, so a variable font exposes exactly the axes it has.
	auto *axisBox = new QGroupBox(tr_("Editor.Axes"), panel);
	_axisLayout = new QFormLayout(axisBox);

	outer->addWidget(axisBox);
	outer->addStretch(1);

	return panel;
}

QWidget *EditorWindow::buildFillPanel()
{
	auto *panel = new QWidget(this);
	auto *outer = new QVBoxLayout(panel);

	// ---- fill ----
	auto *fillBox = new QGroupBox(tr_("Editor.Fill"), panel);
	auto *fillForm = new QFormLayout(fillBox);

	_fillType = new QComboBox(fillBox);
	_fillType->addItem(tr_("Editor.Fill.Solid"), int(Fill::Type::Solid));
	_fillType->addItem(tr_("Editor.Fill.Linear"), int(Fill::Type::Linear));

	connect(_fillType, &QComboBox::currentIndexChanged, this, &EditorWindow::onStyleChanged);

	_fillColor = new ColorButton(fillBox);
	connect(_fillColor, &ColorButton::colorChanged, this, &EditorWindow::onStyleChanged);

	_gradientEnd = new ColorButton(fillBox);
	connect(_gradientEnd, &ColorButton::colorChanged, this, &EditorWindow::onStyleChanged);

	_gradientAngle = new QDoubleSpinBox(fillBox);
	_gradientAngle->setRange(-360.0, 360.0);
	connect(_gradientAngle, &QDoubleSpinBox::valueChanged, this, &EditorWindow::onStyleChanged);

	_gradientPerGlyph = new QCheckBox(tr_("Editor.Fill.PerGlyph"), fillBox);
	connect(_gradientPerGlyph, &QCheckBox::toggled, this, &EditorWindow::onStyleChanged);

	fillForm->addRow(tr_("Editor.Fill.Type"), _fillType);
	fillForm->addRow(tr_("Editor.Fill.Color"), _fillColor);
	fillForm->addRow(tr_("Editor.Fill.EndColor"), _gradientEnd);
	fillForm->addRow(tr_("Editor.Fill.Angle"), _gradientAngle);
	fillForm->addRow(QString(), _gradientPerGlyph);

	// ---- outline ----
	auto *outlineBox = new QGroupBox(tr_("Editor.Outline"), panel);
	auto *outlineForm = new QFormLayout(outlineBox);

	_outlineEnabled = new QCheckBox(tr_("Editor.Enabled"), outlineBox);
	connect(_outlineEnabled, &QCheckBox::toggled, this, &EditorWindow::onStyleChanged);

	_outlineWidth = new QDoubleSpinBox(outlineBox);
	_outlineWidth->setRange(0.1, 200.0);
	_outlineWidth->setSuffix(" px");
	connect(_outlineWidth, &QDoubleSpinBox::valueChanged, this, &EditorWindow::onStyleChanged);

	_outlineColor = new ColorButton(outlineBox);
	connect(_outlineColor, &ColorButton::colorChanged, this, &EditorWindow::onStyleChanged);

	_outlineJoin = new QComboBox(outlineBox);
	_outlineJoin->addItem(tr_("Editor.Join.Round"), int(LineJoin::Round));
	_outlineJoin->addItem(tr_("Editor.Join.Miter"), int(LineJoin::Miter));
	_outlineJoin->addItem(tr_("Editor.Join.Bevel"), int(LineJoin::Bevel));
	connect(_outlineJoin, &QComboBox::currentIndexChanged, this, &EditorWindow::onStyleChanged);

	outlineForm->addRow(QString(), _outlineEnabled);
	outlineForm->addRow(tr_("Editor.Outline.Width"), _outlineWidth);
	outlineForm->addRow(tr_("Editor.Outline.Color"), _outlineColor);
	outlineForm->addRow(tr_("Editor.Outline.Join"), _outlineJoin);

	// ---- shadow ----
	auto *shadowBox = new QGroupBox(tr_("Editor.Shadow"), panel);
	auto *shadowForm = new QFormLayout(shadowBox);

	_shadowEnabled = new QCheckBox(tr_("Editor.Enabled"), shadowBox);
	connect(_shadowEnabled, &QCheckBox::toggled, this, &EditorWindow::onStyleChanged);

	_shadowX = new QDoubleSpinBox(shadowBox);
	_shadowX->setRange(-200.0, 200.0);
	connect(_shadowX, &QDoubleSpinBox::valueChanged, this, &EditorWindow::onStyleChanged);

	_shadowY = new QDoubleSpinBox(shadowBox);
	_shadowY->setRange(-200.0, 200.0);
	connect(_shadowY, &QDoubleSpinBox::valueChanged, this, &EditorWindow::onStyleChanged);

	_shadowColor = new ColorButton(shadowBox);
	connect(_shadowColor, &ColorButton::colorChanged, this, &EditorWindow::onStyleChanged);

	shadowForm->addRow(QString(), _shadowEnabled);
	shadowForm->addRow(tr_("Editor.Shadow.X"), _shadowX);
	shadowForm->addRow(tr_("Editor.Shadow.Y"), _shadowY);
	shadowForm->addRow(tr_("Editor.Shadow.Color"), _shadowColor);

	// ---- spacing ----
	auto *spacingBox = new QGroupBox(tr_("Editor.Spacing"), panel);
	auto *spacingForm = new QFormLayout(spacingBox);

	_letterSpacing = new QDoubleSpinBox(spacingBox);
	_letterSpacing->setRange(-100.0, 100.0);
	_letterSpacing->setSuffix(" px");
	connect(_letterSpacing, &QDoubleSpinBox::valueChanged, this, &EditorWindow::onStyleChanged);

	spacingForm->addRow(tr_("Editor.LetterSpacing"), _letterSpacing);

	auto *scroll = new QScrollArea(panel);
	auto *inner = new QWidget(scroll);
	auto *innerLayout = new QVBoxLayout(inner);

	innerLayout->addWidget(fillBox);
	innerLayout->addWidget(outlineBox);
	innerLayout->addWidget(shadowBox);
	innerLayout->addWidget(spacingBox);
	innerLayout->addStretch(1);

	scroll->setWidget(inner);
	scroll->setWidgetResizable(true);

	outer->addWidget(scroll);

	return panel;
}

QWidget *EditorWindow::buildLayoutPanel()
{
	auto *panel = new QWidget(this);
	auto *form = new QFormLayout(panel);

	_align = new QComboBox(panel);
	_align->addItem(tr_("Align.Left"), int(HAlign::Left));
	_align->addItem(tr_("Align.Center"), int(HAlign::Center));
	_align->addItem(tr_("Align.Right"), int(HAlign::Right));
	_align->addItem(tr_("Align.Justify"), int(HAlign::Justify));
	connect(_align, &QComboBox::currentIndexChanged, this, &EditorWindow::onLayoutChanged);

	_valign = new QComboBox(panel);
	_valign->addItem(tr_("VAlign.Top"), int(VAlign::Top));
	_valign->addItem(tr_("VAlign.Middle"), int(VAlign::Middle));
	_valign->addItem(tr_("VAlign.Bottom"), int(VAlign::Bottom));
	connect(_valign, &QComboBox::currentIndexChanged, this, &EditorWindow::onLayoutChanged);

	_wrap = new QComboBox(panel);
	_wrap->addItem(tr_("Wrap.None"), int(WrapMode::None));
	_wrap->addItem(tr_("Wrap.Word"), int(WrapMode::Word));
	_wrap->addItem(tr_("Wrap.Character"), int(WrapMode::Character));
	connect(_wrap, &QComboBox::currentIndexChanged, this, &EditorWindow::onLayoutChanged);

	_fixedSize = new QCheckBox(tr_("Editor.FixedSize"), panel);
	connect(_fixedSize, &QCheckBox::toggled, this, &EditorWindow::onLayoutChanged);

	_boxWidth = new QSpinBox(panel);
	_boxWidth->setRange(1, 16384);
	connect(_boxWidth, &QSpinBox::valueChanged, this, &EditorWindow::onLayoutChanged);

	_boxHeight = new QSpinBox(panel);
	_boxHeight->setRange(1, 16384);
	connect(_boxHeight, &QSpinBox::valueChanged, this, &EditorWindow::onLayoutChanged);

	_padding = new QDoubleSpinBox(panel);
	_padding->setRange(0.0, 500.0);
	connect(_padding, &QDoubleSpinBox::valueChanged, this, &EditorWindow::onLayoutChanged);

	_lineHeight = new QDoubleSpinBox(panel);
	_lineHeight->setRange(0.1, 10.0);
	_lineHeight->setSingleStep(0.05);
	connect(_lineHeight, &QDoubleSpinBox::valueChanged, this, &EditorWindow::onLayoutChanged);

	_background = new QCheckBox(tr_("Editor.Enabled"), panel);
	connect(_background, &QCheckBox::toggled, this, &EditorWindow::onLayoutChanged);

	_backgroundColor = new ColorButton(panel);
	connect(_backgroundColor, &ColorButton::colorChanged, this, &EditorWindow::onLayoutChanged);

	form->addRow(tr_("Align"), _align);
	form->addRow(tr_("VAlign"), _valign);
	form->addRow(tr_("Wrap"), _wrap);
	form->addRow(QString(), _fixedSize);
	form->addRow(tr_("Extents.Width"), _boxWidth);
	form->addRow(tr_("Extents.Height"), _boxHeight);
	form->addRow(tr_("Editor.Padding"), _padding);
	form->addRow(tr_("Editor.LineHeight"), _lineHeight);
	form->addRow(tr_("Background"), _background);
	form->addRow(tr_("Background.Color"), _backgroundColor);

	return panel;
}

QWidget *EditorWindow::buildMotionPanel()
{
	auto *panel = new QWidget(this);
	auto *form = new QFormLayout(panel);

	_motion = new QComboBox(panel);
	_motion->addItem(tr_("Motion.None"), int(Motion::None));
	_motion->addItem(tr_("Motion.Fade"), int(Motion::Fade));
	_motion->addItem(tr_("Motion.Slide"), int(Motion::Slide));
	_motion->addItem(tr_("Motion.Pop"), int(Motion::Pop));
	_motion->addItem(tr_("Motion.Typewriter"), int(Motion::Typewriter));
	_motion->addItem(tr_("Motion.Wave"), int(Motion::Wave));
	connect(_motion, &QComboBox::currentIndexChanged, this, &EditorWindow::onMotionChanged);

	_motionOrder = new QComboBox(panel);
	_motionOrder->addItem(tr_("Motion.Order.Together"), int(MotionOrder::Together));
	_motionOrder->addItem(tr_("Motion.Order.Glyph"), int(MotionOrder::PerGlyph));
	_motionOrder->addItem(tr_("Motion.Order.Word"), int(MotionOrder::PerWord));
	_motionOrder->addItem(tr_("Motion.Order.Line"), int(MotionOrder::PerLine));
	connect(_motionOrder, &QComboBox::currentIndexChanged, this, &EditorWindow::onMotionChanged);

	_motionDuration = new QDoubleSpinBox(panel);
	_motionDuration->setRange(0.01, 10.0);
	_motionDuration->setSingleStep(0.05);
	_motionDuration->setSuffix(" s");
	connect(_motionDuration, &QDoubleSpinBox::valueChanged, this, &EditorWindow::onMotionChanged);

	_motionStagger = new QDoubleSpinBox(panel);
	_motionStagger->setRange(0.0, 2.0);
	_motionStagger->setSingleStep(0.005);
	_motionStagger->setDecimals(3);
	_motionStagger->setSuffix(" s");
	connect(_motionStagger, &QDoubleSpinBox::valueChanged, this, &EditorWindow::onMotionChanged);

	_motionParam = new QDoubleSpinBox(panel);
	_motionParam->setRange(-500.0, 500.0);
	connect(_motionParam, &QDoubleSpinBox::valueChanged, this, &EditorWindow::onMotionChanged);

	_scrollEnabled = new QCheckBox(tr_("Editor.Scroll"), panel);
	connect(_scrollEnabled, &QCheckBox::toggled, this, &EditorWindow::onMotionChanged);

	_scrollX = new QDoubleSpinBox(panel);
	_scrollX->setRange(-2000.0, 2000.0);
	_scrollX->setSuffix(" px/s");
	connect(_scrollX, &QDoubleSpinBox::valueChanged, this, &EditorWindow::onMotionChanged);

	_scrollY = new QDoubleSpinBox(panel);
	_scrollY->setRange(-2000.0, 2000.0);
	_scrollY->setSuffix(" px/s");
	connect(_scrollY, &QDoubleSpinBox::valueChanged, this, &EditorWindow::onMotionChanged);

	form->addRow(tr_("Motion"), _motion);
	form->addRow(tr_("Motion.Order"), _motionOrder);
	form->addRow(tr_("Motion.Duration"), _motionDuration);
	form->addRow(tr_("Motion.Stagger"), _motionStagger);
	form->addRow(tr_("Motion.Amount"), _motionParam);
	form->addRow(QString(), _scrollEnabled);
	form->addRow(tr_("Editor.ScrollX"), _scrollX);
	form->addRow(tr_("Editor.ScrollY"), _scrollY);

	return panel;
}

void EditorWindow::rebuildAxisControls()
{
	if (!_axisLayout)
		return;

	while (_axisLayout->rowCount() > 0)
		_axisLayout->removeRow(0);

	_axisSliders.clear();

	const Style &style = styleAtCursor();
	const std::vector<FontAxis> axes = FontManager::instance().axesOf(style.font);

	for (const FontAxis &axis : axes) {
		auto *slider = new QSlider(Qt::Horizontal);

		// Sliders are integral, so axis values are scaled by 100 to keep a
		// useful amount of precision on ranges like slant (-10..0).
		slider->setRange(int(axis.minValue * 100.0f), int(axis.maxValue * 100.0f));

		const auto it = style.font.axes.find(axis.tag);
		const float value = it != style.font.axes.end() ? it->second : axis.defaultValue;

		slider->setValue(int(value * 100.0f));

		const std::string tag = axis.tag;

		connect(slider, &QSlider::valueChanged, this, [this, tag](int raw) {
			if (_updating)
				return;

			applyToSelection([&](Style &s) { s.font.axes[tag] = float(raw) / 100.0f; });
		});

		_axisSliders[axis.tag] = slider;
		_axisLayout->addRow(QString::fromStdString(axis.name), slider);
	}
}

// ---------------------------------------------------------------------------
// Signal handlers
// ---------------------------------------------------------------------------

void EditorWindow::onTextChanged()
{
	if (_updating)
		return;

	_document.setPlainText(_textEdit->toPlainText().toStdString());

	apply();
}

void EditorWindow::onFontFamilyChanged()
{
	if (_updating)
		return;

	const std::string family = _fontFamily->currentText().toStdString();

	applyToSelection([&](Style &style) {
		style.font.family = family;

		// A family change invalidates any axis values from the previous font.
		style.font.axes.clear();
	});

	rebuildAxisControls();
}

void EditorWindow::onStyleChanged()
{
	if (_updating)
		return;

	const int size = _fontSize->value();
	const int weight = indexToWeight(_fontWeight->currentIndex());
	const bool italic = _fontItalic->isChecked();

	const auto fillType = Fill::Type(_fillType->currentData().toInt());
	const Rgba fillColor = _fillColor->color();
	const Rgba gradientEnd = _gradientEnd->color();
	const float angle = float(_gradientAngle->value());
	const bool perGlyph = _gradientPerGlyph->isChecked();

	const bool outline = _outlineEnabled->isChecked();
	const float outlineWidth = float(_outlineWidth->value());
	const Rgba outlineColor = _outlineColor->color();
	const auto join = LineJoin(_outlineJoin->currentData().toInt());

	const bool shadow = _shadowEnabled->isChecked();
	const float shadowX = float(_shadowX->value());
	const float shadowY = float(_shadowY->value());
	const Rgba shadowColor = _shadowColor->color();

	const float letterSpacing = float(_letterSpacing->value());

	applyToSelection([&](Style &style) {
		style.sizePx = float(size);
		style.font.weight = weight;
		style.font.italic = italic;

		style.fill.type = fillType;
		style.fill.color = fillColor;
		style.fill.angleDeg = angle;
		style.fill.perGlyph = perGlyph;

		style.fill.stops.clear();
		style.fill.stops.push_back(GradientStop{0.0f, fillColor});
		style.fill.stops.push_back(GradientStop{1.0f, gradientEnd});

		style.outline.enabled = outline;
		style.outline.widthPx = outlineWidth;
		style.outline.color = outlineColor;
		style.outline.join = join;

		style.shadow.enabled = shadow;
		style.shadow.offsetX = shadowX;
		style.shadow.offsetY = shadowY;
		style.shadow.color = shadowColor;

		style.letterSpacing = letterSpacing;
	});
}

void EditorWindow::onLayoutChanged()
{
	if (_updating)
		return;

	const auto align = HAlign(_align->currentData().toInt());
	const float lineHeight = float(_lineHeight->value());

	for (Block &block : _document.blocks) {
		block.align = align;
		block.lineHeight = lineHeight;
	}

	_document.valign = VAlign(_valign->currentData().toInt());
	_document.wrap = WrapMode(_wrap->currentData().toInt());
	_document.sizeMode = _fixedSize->isChecked() ? SizeMode::Fixed : SizeMode::Auto;
	_document.boxWidth = float(_boxWidth->value());
	_document.boxHeight = float(_boxHeight->value());
	_document.padding = float(_padding->value());
	_document.backgroundEnabled = _background->isChecked();
	_document.backgroundColor = _backgroundColor->color();

	apply();
}

void EditorWindow::onMotionChanged()
{
	if (_updating)
		return;

	_document.motion.motion = Motion(_motion->currentData().toInt());
	_document.motion.order = MotionOrder(_motionOrder->currentData().toInt());
	_document.motion.duration = float(_motionDuration->value());
	_document.motion.stagger = float(_motionStagger->value());
	_document.motion.param = float(_motionParam->value());

	_document.scroll.enabled = _scrollEnabled->isChecked();
	_document.scroll.speedX = float(_scrollX->value());
	_document.scroll.speedY = float(_scrollY->value());

	apply();
}

// ---------------------------------------------------------------------------

void EditorWindow::syncControls()
{
	_updating = true;

	const std::string plain = _document.plainText();

	if (_textEdit->toPlainText().toStdString() != plain) {
		const int cursor = _textEdit->textCursor().position();

		_textEdit->setPlainText(QString::fromStdString(plain));

		QTextCursor restored = _textEdit->textCursor();
		restored.setPosition(std::min(cursor, int(plain.size())));
		_textEdit->setTextCursor(restored);
	}

	const Style &style = styleAtCursor();

	_fontFamily->setCurrentText(QString::fromStdString(style.font.family));
	_fontSize->setValue(int(style.sizePx));
	_fontWeight->setCurrentIndex(weightToIndex(style.font.weight));
	_fontItalic->setChecked(style.font.italic);

	_fillType->setCurrentIndex(_fillType->findData(int(style.fill.type)));
	_fillColor->setColor(style.fill.color);
	_gradientEnd->setColor(style.fill.stops.size() > 1 ? style.fill.stops.back().color : style.fill.color);
	_gradientAngle->setValue(double(style.fill.angleDeg));
	_gradientPerGlyph->setChecked(style.fill.perGlyph);

	_outlineEnabled->setChecked(style.outline.enabled);
	_outlineWidth->setValue(double(style.outline.widthPx));
	_outlineColor->setColor(style.outline.color);
	_outlineJoin->setCurrentIndex(_outlineJoin->findData(int(style.outline.join)));

	_shadowEnabled->setChecked(style.shadow.enabled);
	_shadowX->setValue(double(style.shadow.offsetX));
	_shadowY->setValue(double(style.shadow.offsetY));
	_shadowColor->setColor(style.shadow.color);

	_letterSpacing->setValue(double(style.letterSpacing));

	const Block *firstBlock = _document.blocks.empty() ? nullptr : &_document.blocks.front();

	_align->setCurrentIndex(_align->findData(int(firstBlock ? firstBlock->align : HAlign::Left)));
	_lineHeight->setValue(double(firstBlock ? firstBlock->lineHeight : 1.0f));

	_valign->setCurrentIndex(_valign->findData(int(_document.valign)));
	_wrap->setCurrentIndex(_wrap->findData(int(_document.wrap)));
	_fixedSize->setChecked(_document.sizeMode == SizeMode::Fixed);
	_boxWidth->setValue(int(_document.boxWidth));
	_boxHeight->setValue(int(_document.boxHeight));
	_padding->setValue(double(_document.padding));
	_background->setChecked(_document.backgroundEnabled);
	_backgroundColor->setColor(_document.backgroundColor);

	_motion->setCurrentIndex(_motion->findData(int(_document.motion.motion)));
	_motionOrder->setCurrentIndex(_motionOrder->findData(int(_document.motion.order)));
	_motionDuration->setValue(double(_document.motion.duration));
	_motionStagger->setValue(double(_document.motion.stagger));
	_motionParam->setValue(double(_document.motion.param));

	_scrollEnabled->setChecked(_document.scroll.enabled);
	_scrollX->setValue(double(_document.scroll.speedX));
	_scrollY->setValue(double(_document.scroll.speedY));

	_updating = false;

	rebuildAxisControls();
}

// ---------------------------------------------------------------------------
// editor_bridge implementation
// ---------------------------------------------------------------------------

namespace {

std::map<obs_source_t *, QPointer<EditorWindow>> g_windows;

} // namespace

namespace editor {

void openFor(obs_source_t *source)
{
	if (!source)
		return;

	const auto it = g_windows.find(source);

	if (it != g_windows.end() && it->second) {
		it->second->reload();
		it->second->show();
		it->second->raise();
		it->second->activateWindow();

		return;
	}

	auto *parent = static_cast<QWidget *>(obs_frontend_get_main_window());
	auto *window = new EditorWindow(source, parent);

	g_windows[source] = window;

	// WA_DeleteOnClose means the window frees itself; drop the stale entry so a
	// later open creates a fresh one.
	QObject::connect(window, &QObject::destroyed, [source]() { g_windows.erase(source); });

	window->show();
}

void closeFor(obs_source_t *source)
{
	const auto it = g_windows.find(source);

	if (it == g_windows.end())
		return;

	if (it->second)
		it->second->close();

	g_windows.erase(it);
}

void shutdown()
{
	for (auto &[source, window] : g_windows) {
		UNUSED_PARAMETER(source);

		if (window)
			window->close();
	}

	g_windows.clear();
}

} // namespace editor

} // namespace slugged
