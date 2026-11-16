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

// The editor's side panel: its sections, and keeping them in step with the
// document. The rest of EditorWindow is in editor_window.cpp.

#include "editor_window.hpp"
#include "color_button.hpp"
#include "editor_common.hpp"
#include "styles_panel.hpp"
#include "widgets.hpp"
#include "../obs/settings.hpp"
#include "../text/font_enum.hpp"
#include "../text/font_manager.hpp"

#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

namespace slugged {

using namespace ui;

namespace {

// Speed is one slider: how long a unit takes, with the gap between units
// keeping its usual proportion to it.
constexpr float kPaceFastest = 0.1f;
constexpr float kPaceRange = 20.0f;
constexpr float kStaggerShare = 0.085f;

float paceToDuration(int pace)
{
	return kPaceFastest * std::pow(kPaceRange, float(pace) / 100.0f);
}

int durationToPace(float duration)
{
	const float t = std::log(std::max(duration, kPaceFastest) / kPaceFastest) / std::log(kPaceRange);

	return std::clamp(int(std::lround(t * 100.0f)), 0, 100);
}

// Several controls side by side, sharing the width.
QWidget *line(QWidget *parent, std::initializer_list<QWidget *> widgets)
{
	auto *holder = new QWidget(parent);
	auto *layout = new QHBoxLayout(holder);

	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(8);

	for (QWidget *widget : widgets)
		layout->addWidget(widget, 1);

	return holder;
}

// A label with a switch at the far end.
QWidget *switchRow(QWidget *parent, const QString &label, ui::Switch *&toggle, const QString &detail = QString())
{
	auto *holder = new QWidget(parent);
	auto *layout = new QHBoxLayout(holder);

	layout->setContentsMargins(0, 4, 0, 4);

	auto *text = new QVBoxLayout();
	text->setSpacing(0);

	auto *title = new QLabel(label, holder);
	text->addWidget(title);

	if (!detail.isEmpty())
		text->addWidget(caption(detail, holder));

	toggle = new ui::Switch(holder);
	toggle->setAccessibleName(label);

	layout->addLayout(text, 1);
	layout->addWidget(toggle);

	return holder;
}

// A box of controls that only shows while its switch is on.
QWidget *subBox(QWidget *parent, QVBoxLayout *&layout)
{
	auto *box = new QWidget(parent);

	layout = new QVBoxLayout(box);
	layout->setContentsMargins(0, 0, 0, 6);
	layout->setSpacing(8);

	return box;
}

void markDiffers(QWidget *widget, bool differs)
{
	if (widget->property("differs").toBool() == differs)
		return;

	widget->setProperty("differs", differs);
	widget->style()->unpolish(widget);
	widget->style()->polish(widget);
}

QString shortened(QString text, int limit)
{
	text.replace(QLatin1Char('\n'), QLatin1Char(' '));

	return text.size() > limit ? text.left(limit - 1) + QChar(0x2026) : text;
}

} // namespace

QWidget *EditorWindow::buildInspector()
{
	auto *panel = new QWidget(this);
	panel->setObjectName("sidePanel");
	panel->setAttribute(Qt::WA_StyledBackground);

	auto *outer = new QVBoxLayout(panel);
	outer->setContentsMargins(1, 0, 0, 0);
	outer->setSpacing(0);

	// ---- what the controls below will change ----
	_scopeHeader = new QWidget(panel);
	_scopeHeader->setObjectName("scopeHeader");
	_scopeHeader->setAttribute(Qt::WA_StyledBackground);

	auto *scopeLayout = new QVBoxLayout(_scopeHeader);
	scopeLayout->setContentsMargins(14, 12, 14, 12);
	scopeLayout->setSpacing(2);

	auto *eyebrow = new QLabel(tr_("Editor.Scope.Label").toUpper(), _scopeHeader);
	eyebrow->setObjectName("eyebrow");

	_scopeTitle = new QLabel(_scopeHeader);
	_scopeTitle->setObjectName("scopeTitle");

	_scopeDetail = new QLabel(_scopeHeader);
	_scopeDetail->setObjectName("muted");
	_scopeDetail->setWordWrap(true);

	_clearFormatting = new QPushButton(tr_("Editor.ClearFormatting"), _scopeHeader);
	_clearFormatting->setObjectName("ghost");

	connect(_clearFormatting, &QPushButton::clicked, this, &EditorWindow::clearFormatting);

	scopeLayout->addWidget(eyebrow);
	scopeLayout->addWidget(_scopeTitle);
	scopeLayout->addWidget(_scopeDetail);
	scopeLayout->addSpacing(4);
	scopeLayout->addWidget(_clearFormatting, 0, Qt::AlignLeft);

	outer->addWidget(_scopeHeader);

	// ---- the sections ----
	auto *scroll = new QScrollArea(panel);
	scroll->setWidgetResizable(true);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	auto *content = new QWidget(scroll);
	content->setObjectName("sideContent");

	auto *sections = new QVBoxLayout(content);
	sections->setContentsMargins(0, 0, 0, 0);
	sections->setSpacing(0);

	_textSection = new ui::Section(tr_("Editor.Section.Text"), content);
	_effectsSection = new ui::Section(tr_("Editor.Section.Effects"), content);
	_layoutSection = new ui::Section(tr_("Editor.Section.Layout"), content);
	_motionSection = new ui::Section(tr_("Editor.Section.Motion"), content);
	_moreSection = new ui::Section(tr_("Editor.Section.More"), content);

	buildTextSection();
	buildEffectsSection();
	buildLayoutSection();
	buildMotionSection();
	buildMoreSection();

	// Open on what most people came to change; the rest say what they are
	// set to and wait.
	_layoutSection->setExpanded(false);
	_motionSection->setExpanded(false);
	_moreSection->setExpanded(false);

	_selectionNote = caption(tr_("Editor.Scope.MotionNote"), content);
	_selectionNote->setContentsMargins(14, 12, 14, 12);

	sections->addWidget(_textSection);
	sections->addWidget(_effectsSection);
	sections->addWidget(_layoutSection);
	sections->addWidget(_motionSection);
	sections->addWidget(_moreSection);
	sections->addWidget(_selectionNote);
	sections->addStretch(1);

	scroll->setWidget(content);
	outer->addWidget(scroll, 1);

	return panel;
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

void EditorWindow::buildTextSection()
{
	QWidget *panel = _textSection->bodyWidget();
	QVBoxLayout *body = _textSection->body();

	_fontFamily = new QComboBox(panel);
	_fontFamily->setEditable(true);
	_fontFamily->setInsertPolicy(QComboBox::NoInsert);
	_fontFamily->setAccessibleName(tr_("Font"));
	_fontFamily->completer()->setCompletionMode(QCompleter::PopupCompletion);
	_fontFamily->completer()->setFilterMode(Qt::MatchContains);
	_fontFamily->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	_fontFamily->setMinimumContentsLength(12);

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

	_fontSize = new QSpinBox(panel);
	_fontSize->setRange(1, 2000);
	_fontSize->setPrefix(tr_("Editor.Size") + "  ");
	_fontSize->setKeyboardTracking(false);
	_fontSize->setAccessibleName(tr_("Editor.Size"));

	connect(_fontSize, &QSpinBox::valueChanged, this, [this](int v) {
		StyleOverride d;
		d.size = float(v);
		applyStyle(d);
	});

	_fontWeight = new QComboBox(panel);
	_fontWeight->setAccessibleName(tr_("Editor.Weight"));

	for (const auto &[weight, key] : kWeights)
		_fontWeight->addItem(tr_(key), weight);

	connect(_fontWeight, &QComboBox::activated, this, [this]() {
		StyleOverride d;
		d.weight = _fontWeight->currentData().toInt();
		applyStyle(d);
		syncControls();
	});

	auto toggle = [this, panel](const QString &letter, const char *nameKey, const char *css, auto setter) {
		auto *button = new QPushButton(letter, panel);

		button->setObjectName("toggle");
		button->setCheckable(true);
		button->setStyleSheet(QString::fromUtf8(css));
		button->setToolTip(tr_(nameKey));
		button->setAccessibleName(tr_(nameKey));

		connect(button, &QPushButton::clicked, this, [this, setter](bool on) {
			StyleOverride d;
			setter(d, on);
			applyStyle(d);
			syncControls();
		});

		return button;
	};

	_fontBold = toggle(QStringLiteral("B"), "Editor.Bold", "font-weight: 800;",
			   [](StyleOverride &d, bool on) { d.weight = on ? 700 : 400; });
	_fontItalic = toggle(QStringLiteral("I"), "Editor.Italic", "font-style: italic; font-family: Georgia, serif;",
			     [](StyleOverride &d, bool on) { d.italic = on; });
	_fontUnderline = toggle(QStringLiteral("U"), "Editor.Underline", "text-decoration: underline;",
				[](StyleOverride &d, bool on) { d.underline = on; });
	_fontStrikeout = toggle(QStringLiteral("S"), "Editor.Strikeout", "text-decoration: line-through;",
				[](StyleOverride &d, bool on) { d.strikeout = on; });

	auto colorField = [this, panel](const char *captionKey, auto setter) {
		auto *button = new ColorButton(panel);

		button->setCaption(tr_(captionKey));

		connect(button, &ColorButton::colorChanged, this, [this, button, setter]() {
			StyleOverride d;
			setter(d, button->color());
			applyStyle(d);
			syncControls();
		});

		return button;
	};

	_fillColor = colorField("Color", [](StyleOverride &d, Rgba c) { d.color = c; });

	auto *marks = new QWidget(panel);
	auto *marksLayout = new QHBoxLayout(marks);
	marksLayout->setContentsMargins(0, 0, 0, 0);
	marksLayout->setSpacing(4);
	marksLayout->addWidget(_fontBold);
	marksLayout->addWidget(_fontItalic);
	marksLayout->addWidget(_fontUnderline);
	marksLayout->addWidget(_fontStrikeout);
	marksLayout->addStretch(1);
	marksLayout->addWidget(_fillColor);

	_fillMode = new ui::Segmented(panel);
	_fillMode->setAccessibleName(tr_("Fill"));
	_fillMode->addOption(tr_("Editor.Fill.Solid"), 0);
	_fillMode->addOption(tr_("Editor.Fill.Gradient"), 1);

	connect(_fillMode, &ui::Segmented::chosen, this, [this](int value) {
		StyleOverride d;

		// Turning the gradient back on returns to the kind it was.
		d.fillType = value == 0 ? FillType::Solid : comboValue<FillType>(_gradientType);
		applyStyle(d);
		syncControls();
	});

	// ---- gradient ----
	QVBoxLayout *gradient = nullptr;
	_gradientBox = subBox(panel, gradient);

	_fillColor2 = colorField("Editor.Fill.End", [](StyleOverride &d, Rgba c) { d.color2 = c; });

	_gradientType = enumCombo<FillType>(_gradientBox,
					    {{FillType::Linear, "Fill.Linear"}, {FillType::Radial, "Fill.Radial"}});
	_gradientType->setAccessibleName(tr_("Fill"));

	connect(_gradientType, &QComboBox::activated, this, [this]() {
		StyleOverride d;
		d.fillType = comboValue<FillType>(_gradientType);
		applyStyle(d);
		syncControls();
	});

	_gradientAngle = spin(_gradientBox, -180.0, 180.0, 5.0, "\xC2\xB0");

	connect(_gradientAngle, &QDoubleSpinBox::valueChanged, this, [this](double v) {
		StyleOverride d;
		d.gradientAngle = float(v);
		applyStyle(d);
	});

	_gradientSpan = enumCombo<GradientSpan>(_gradientBox, {{GradientSpan::Text, "Fill.Span.Text"},
							       {GradientSpan::Character, "Fill.Span.Character"}});

	connect(_gradientSpan, &QComboBox::activated, this, [this]() {
		StyleOverride d;
		d.gradientSpan = comboValue<GradientSpan>(_gradientSpan);
		applyStyle(d);
	});

	_gradientAngleRow = row(tr_("Fill.Angle"), _gradientAngle, _gradientBox);

	gradient->addWidget(line(_gradientBox, {_fillColor2, _gradientType}));
	gradient->addWidget(_gradientAngleRow);
	gradient->addWidget(row(tr_("Fill.Span"), _gradientSpan, _gradientBox));

	_differsLegend = caption(tr_("Editor.Differs"), panel);

	body->addWidget(_fontFamily);
	body->addWidget(line(panel, {_fontSize, _fontWeight}));
	body->addWidget(marks);
	body->addWidget(_fillMode);
	body->addWidget(_gradientBox);
	body->addWidget(_differsLegend);
}

// ---------------------------------------------------------------------------
// Effects
// ---------------------------------------------------------------------------

void EditorWindow::buildEffectsSection()
{
	QWidget *panel = _effectsSection->bodyWidget();
	QVBoxLayout *body = _effectsSection->body();

	body->setSpacing(2);

	auto styleSwitch = [this](ui::Switch *toggle, auto setter) {
		connect(toggle, &ui::Switch::clicked, this, [this, setter](bool on) {
			StyleOverride d;
			setter(d, on);
			applyStyle(d);
			syncControls();
		});
	};

	auto styleColor = [this](QWidget *parent, const char *captionKey, auto setter) {
		auto *button = new ColorButton(parent);

		button->setCaption(tr_(captionKey));

		connect(button, &ColorButton::colorChanged, this, [this, button, setter]() {
			StyleOverride d;
			setter(d, button->color());
			applyStyle(d);
		});

		return button;
	};

	auto styleFloat = [this](QWidget *parent, double lo, double hi, double step, const QString &prefix,
				 auto setter) {
		auto *box = spin(parent, lo, hi, step);

		box->setPrefix(prefix + "  ");
		box->setAccessibleName(prefix);

		connect(box, &QDoubleSpinBox::valueChanged, this, [this, setter](double v) {
			StyleOverride d;
			setter(d, float(v));
			applyStyle(d);
		});

		return box;
	};

	// ---- outline ----
	body->addWidget(switchRow(panel, tr_("Outline"), _outlineEnabled));
	styleSwitch(_outlineEnabled, [](StyleOverride &d, bool on) { d.outline = on; });

	QVBoxLayout *outline = nullptr;
	_outlineBox = subBox(panel, outline);

	_outlineColor = styleColor(_outlineBox, "Color", [](StyleOverride &d, Rgba c) { d.outlineColor = c; });
	_outlineWidth = styleFloat(_outlineBox, 0.5, 100.0, 0.5, tr_("Editor.Width"),
				   [](StyleOverride &d, float v) { d.outlineWidth = v; });

	_outlineJoin = enumCombo<LineJoin>(_outlineBox, {{LineJoin::Round, "Outline.Join.Round"},
							 {LineJoin::Miter, "Outline.Join.Miter"},
							 {LineJoin::Bevel, "Outline.Join.Bevel"}});
	_outlineJoin->setAccessibleName(tr_("Outline.Join"));
	_outlineJoin->setToolTip(tr_("Outline.Join"));

	connect(_outlineJoin, &QComboBox::activated, this, [this]() {
		StyleOverride d;
		d.outlineJoin = comboValue<LineJoin>(_outlineJoin);
		applyStyle(d);
	});

	outline->addWidget(line(_outlineBox, {_outlineColor, _outlineWidth}));
	outline->addWidget(row(tr_("Outline.Join"), _outlineJoin, _outlineBox));

	body->addWidget(_outlineBox);

	// ---- shadow ----
	body->addWidget(switchRow(panel, tr_("Shadow"), _shadowEnabled));
	styleSwitch(_shadowEnabled, [](StyleOverride &d, bool on) { d.shadow = on; });

	QVBoxLayout *shadow = nullptr;
	_shadowBox = subBox(panel, shadow);

	_shadowColor = styleColor(_shadowBox, "Color", [](StyleOverride &d, Rgba c) { d.shadowColor = c; });
	_shadowX = styleFloat(_shadowBox, -500.0, 500.0, 1.0, tr_("Editor.Shadow.Right"),
			      [](StyleOverride &d, float v) { d.shadowX = v; });
	_shadowY = styleFloat(_shadowBox, -500.0, 500.0, 1.0, tr_("Editor.Shadow.Down"),
			      [](StyleOverride &d, float v) { d.shadowY = v; });

	shadow->addWidget(_shadowColor);
	shadow->addWidget(line(_shadowBox, {_shadowX, _shadowY}));

	body->addWidget(_shadowBox);

	// ---- background: the whole source's, whatever is selected ----
	body->addWidget(switchRow(panel, tr_("Editor.BackgroundBox"), _background));

	connect(_background, &ui::Switch::clicked, this,
		[this](bool on) { applyDocument([on](Document &d) { d.background.enabled = on; }); });

	QVBoxLayout *background = nullptr;
	_backgroundBox = subBox(panel, background);

	_backgroundColor = new ColorButton(_backgroundBox);
	_backgroundColor->setCaption(tr_("Color"));

	connect(_backgroundColor, &ColorButton::colorChanged, this,
		[this]() { applyDocument([this](Document &d) { d.background.color = _backgroundColor->color(); }); });

	_backgroundRadius = spin(_backgroundBox, 0.0, 500.0, 1.0);
	_backgroundRadius->setPrefix(tr_("Editor.Corners") + "  ");
	_backgroundRadius->setAccessibleName(tr_("Background.Radius"));

	connect(_backgroundRadius, &QDoubleSpinBox::valueChanged, this,
		[this](double v) { applyDocument([v](Document &d) { d.background.radius = float(v); }); });

	_padding = spin(_backgroundBox, 0.0, 1000.0, 1.0);
	_padding->setAccessibleName(tr_("Padding"));

	connect(_padding, &QDoubleSpinBox::valueChanged, this,
		[this](double v) { applyDocument([v](Document &d) { d.padding = float(v); }); });

	background->addWidget(line(_backgroundBox, {_backgroundColor, _backgroundRadius}));
	background->addWidget(row(tr_("Editor.Padding"), _padding, _backgroundBox));

	body->addWidget(_backgroundBox);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void EditorWindow::buildLayoutSection()
{
	QWidget *panel = _layoutSection->bodyWidget();
	QVBoxLayout *body = _layoutSection->body();

	_align = new ui::Segmented(panel);
	_align->setAccessibleName(tr_("Align"));
	_align->addOption(icon(Glyph::AlignStart), tr_("Align.Start"), int(HAlign::Start));
	_align->addOption(icon(Glyph::AlignCenter), tr_("Align.Center"), int(HAlign::Center));
	_align->addOption(icon(Glyph::AlignEnd), tr_("Align.End"), int(HAlign::End));
	_align->addOption(icon(Glyph::AlignJustify), tr_("Align.Justify"), int(HAlign::Justify));

	connect(_align, &ui::Segmented::chosen, this, [this](int value) {
		ParagraphOverride d;
		d.align = HAlign(value);
		applyParagraph(d);
		syncVisibility();
	});

	_alignHint = caption(tr_("Editor.Align.Hint"), panel);

	_sizeMode = new ui::Segmented(panel);
	_sizeMode->setAccessibleName(tr_("Editor.Box"));
	_sizeMode->addOption(tr_("Editor.Size.Auto"), int(SizeMode::Auto));
	_sizeMode->addOption(tr_("Editor.Size.Fixed"), int(SizeMode::Fixed));

	connect(_sizeMode, &ui::Segmented::chosen, this,
		[this](int value) { applyDocument([value](Document &d) { d.sizeMode = SizeMode(value); }); });

	// ---- the fixed box ----
	QVBoxLayout *box = nullptr;
	_boxControls = subBox(panel, box);

	auto dimension = [this](const char *key, auto setter) {
		auto *field = new QSpinBox(_boxControls);

		field->setRange(1, 16384);
		field->setKeyboardTracking(false);
		field->setPrefix(tr_(key) + "  ");
		field->setAccessibleName(tr_(key));

		connect(field, &QSpinBox::valueChanged, this,
			[this, setter](int v) { applyDocument([&](Document &d) { setter(d, float(v)); }); });

		return field;
	};

	_boxWidth = dimension("Extents.Width", [](Document &d, float v) { d.boxWidth = v; });
	_boxHeight = dimension("Extents.Height", [](Document &d, float v) { d.boxHeight = v; });

	_wrap = enumCombo<WrapMode>(_boxControls, {{WrapMode::None, "Wrap.None"},
						   {WrapMode::Word, "Wrap.Word"},
						   {WrapMode::Character, "Wrap.Character"}});

	connect(_wrap, &QComboBox::activated, this,
		[this]() { applyDocument([this](Document &d) { d.wrap = comboValue<WrapMode>(_wrap); }); });

	_valign = new ui::Segmented(_boxControls);
	_valign->setAccessibleName(tr_("VAlign"));
	_valign->addOption(tr_("VAlign.Top"), int(VAlign::Top));
	_valign->addOption(tr_("VAlign.Middle"), int(VAlign::Middle));
	_valign->addOption(tr_("VAlign.Bottom"), int(VAlign::Bottom));

	connect(_valign, &ui::Segmented::chosen, this,
		[this](int value) { applyDocument([value](Document &d) { d.valign = VAlign(value); }); });

	box->addWidget(line(_boxControls, {_boxWidth, _boxHeight}));
	box->addWidget(row(tr_("Wrap"), _wrap, _boxControls));
	box->addWidget(_valign);

	_lineHeight = spin(panel, 0.3, 5.0, 0.05, nullptr, 2);

	connect(_lineHeight, &QDoubleSpinBox::valueChanged, this, [this](double v) {
		ParagraphOverride d;
		d.lineHeight = float(v);
		applyParagraph(d);
	});

	body->addWidget(_align);
	body->addWidget(_alignHint);
	body->addWidget(_sizeMode);
	body->addWidget(_boxControls);
	body->addWidget(row(tr_("LineHeight"), _lineHeight, panel));
}

// ---------------------------------------------------------------------------
// Motion
// ---------------------------------------------------------------------------

void EditorWindow::buildMotionSection()
{
	QWidget *panel = _motionSection->bodyWidget();
	QVBoxLayout *body = _motionSection->body();

	// ---- the nine ----
	auto *grid = new QGridLayout();
	grid->setSpacing(6);

	const std::pair<Motion, const char *> tiles[] = {
		{Motion::None, "Motion.None"},
		{Motion::Fade, "Editor.Motion.Fade"},
		{Motion::Slide, "Editor.Motion.Slide"},
		{Motion::Pop, "Editor.Motion.Pop"},
		{Motion::Typewriter, "Motion.Typewriter"},
		{Motion::Wave, "Motion.Wave"},
		{Motion::Blur, "Editor.Motion.Blur"},
		{Motion::Spin, "Editor.Motion.Spin"},
		{Motion::Bounce, "Editor.Motion.Bounce"},
	};

	int index = 0;

	for (const auto &[motion, key] : tiles) {
		auto *tile = new QPushButton(tr_(key), panel);

		tile->setObjectName("motionTile");
		tile->setCheckable(true);
		tile->installEventFilter(this);

		const Motion value = motion;

		connect(tile, &QPushButton::clicked, this, [this, value]() {
			_pendingPreview = -1;
			_previewDelay->stop();

			applyDocument([value](Document &d) {
				d.motion.motion = value;

				// A value meant for another preset is replaced by this
				// preset's own default: 48 px is a slide, not a scale.
				const MotionParamSpec amount = motionParamSpec(value);

				if (amount.label && (d.motion.param < amount.min || d.motion.param > amount.max))
					d.motion.param = amount.defaultValue;
			});

			syncControls();
		});

		_motionTiles[int(motion)] = tile;
		grid->addWidget(tile, index / 3, index % 3);
		index++;
	}

	body->addLayout(grid);
	body->addWidget(caption(tr_("Editor.Motion.Hover"), panel));

	// ---- everything that only matters once something moves ----
	QVBoxLayout *details = nullptr;
	_motionDetails = subBox(panel, details);

	auto motionFloat = [this](double lo, double hi, double step, const char *suffix, int decimals, auto setter) {
		auto *box = spin(_motionDetails, lo, hi, step, suffix, decimals);

		connect(box, &QDoubleSpinBox::valueChanged, this, [this, setter](double v) {
			applyDocument([&](Document &d) { setter(d.motion, float(v)); });

			// The slider and the exact numbers are two views of the same thing.
			if (!_updating) {
				_updating = true;
				_motionPace->setValue(durationToPace(_document.motion.duration));
				_updating = false;
			}
		});

		return box;
	};

	_motionTrigger =
		enumCombo<MotionTrigger>(_motionDetails, {{MotionTrigger::TextChange, "Motion.Replay.TextChange"},
							  {MotionTrigger::Show, "Motion.Replay.Show"},
							  {MotionTrigger::Activate, "Motion.Replay.Activate"},
							  {MotionTrigger::Loop, "Motion.Replay.Loop"},
							  {MotionTrigger::Never, "Motion.Replay.Never"}});

	connect(_motionTrigger, &QComboBox::activated, this, [this]() {
		applyDocument([this](Document &d) { d.motion.trigger = comboValue<MotionTrigger>(_motionTrigger); });
	});

	_motionTriggerRow = new QWidget(_motionDetails);

	auto *triggerLayout = new QVBoxLayout(_motionTriggerRow);
	triggerLayout->setContentsMargins(0, 0, 0, 0);
	triggerLayout->setSpacing(4);

	auto *triggerLabel = new QLabel(tr_("Editor.Motion.Play"), _motionTriggerRow);
	triggerLabel->setObjectName("muted");
	triggerLabel->setBuddy(_motionTrigger);

	_motionChangedOnly = new QCheckBox(tr_("Editor.Motion.ChangedOnly"), _motionTriggerRow);

	connect(_motionChangedOnly, &QCheckBox::clicked, this, [this](bool on) {
		applyDocument([on](Document &d) { d.motion.scope = on ? MotionScope::Changed : MotionScope::All; });
	});

	_motionExit = new QCheckBox(tr_("Motion.Exit"), _motionTriggerRow);

	connect(_motionExit, &QCheckBox::clicked, this,
		[this](bool on) { applyDocument([on](Document &d) { d.motion.exit = on; }); });

	_motionLoopInterval =
		motionFloat(0.1, 3600.0, 0.5, " s", 1, [](MotionSpec &m, float v) { m.loopInterval = v; });
	_motionLoopRow = row(tr_("Motion.Replay.Interval"), _motionLoopInterval, _motionTriggerRow);

	triggerLayout->addWidget(triggerLabel);
	triggerLayout->addWidget(_motionTrigger);
	triggerLayout->addWidget(_motionChangedOnly);
	triggerLayout->addWidget(_motionExit);
	triggerLayout->addWidget(_motionLoopRow);

	auto *orderLabel = new QLabel(tr_("Editor.Motion.Move"), _motionDetails);
	orderLabel->setObjectName("muted");

	_motionOrder = new ui::Segmented(_motionDetails);
	_motionOrder->setAccessibleName(tr_("Editor.Motion.Move"));
	_motionOrder->addOption(tr_("Editor.Motion.Together"), int(MotionOrder::Together));
	_motionOrder->addOption(tr_("Editor.Motion.Letters"), int(MotionOrder::PerCharacter));
	_motionOrder->addOption(tr_("Editor.Motion.Words"), int(MotionOrder::PerWord));
	_motionOrder->addOption(tr_("Editor.Motion.Lines"), int(MotionOrder::PerLine));

	connect(_motionOrder, &ui::Segmented::chosen, this,
		[this](int value) { applyDocument([value](Document &d) { d.motion.order = MotionOrder(value); }); });

	_motionDirection =
		enumCombo<MotionDirection>(_motionDetails, {{MotionDirection::Up, "Motion.Direction.Up"},
							    {MotionDirection::Down, "Motion.Direction.Down"},
							    {MotionDirection::Left, "Motion.Direction.Left"},
							    {MotionDirection::Right, "Motion.Direction.Right"}});

	connect(_motionDirection, &QComboBox::activated, this, [this]() {
		applyDocument(
			[this](Document &d) { d.motion.direction = comboValue<MotionDirection>(_motionDirection); });
	});

	_motionDirectionRow = row(tr_("Motion.Direction"), _motionDirection, _motionDetails);

	// ---- speed ----
	_motionPaceRow = new QWidget(_motionDetails);

	auto *paceLayout = new QVBoxLayout(_motionPaceRow);
	paceLayout->setContentsMargins(0, 0, 0, 0);
	paceLayout->setSpacing(2);

	auto *paceLabels = new QHBoxLayout();
	auto *fast = new QLabel(tr_("Editor.Motion.Snappy"), _motionPaceRow);
	auto *slow = new QLabel(tr_("Editor.Motion.Relaxed"), _motionPaceRow);

	fast->setObjectName("muted");
	slow->setObjectName("muted");

	paceLabels->addWidget(fast);
	paceLabels->addStretch(1);
	paceLabels->addWidget(slow);

	_motionPace = new QSlider(Qt::Horizontal, _motionPaceRow);
	_motionPace->setRange(0, 100);
	_motionPace->setAccessibleName(tr_("Editor.Motion.Speed"));

	connect(_motionPace, &QSlider::valueChanged, this, [this](int pace) {
		if (_updating)
			return;

		applyDocument([pace](Document &d) {
			d.motion.duration = paceToDuration(pace);
			d.motion.stagger = d.motion.duration * kStaggerShare;
		});

		_updating = true;
		_motionDuration->setValue(double(_document.motion.duration));
		_motionStagger->setValue(double(_document.motion.stagger));
		_updating = false;
	});

	paceLayout->addWidget(_motionPace);
	paceLayout->addLayout(paceLabels);

	// ---- the exact numbers, for those who want them ----
	_fineTuneToggle = new QPushButton(tr_("Editor.Motion.FineTune"), _motionDetails);
	_fineTuneToggle->setObjectName("link");
	_fineTuneToggle->setCheckable(true);
	_fineTuneToggle->setCursor(Qt::PointingHandCursor);

	connect(_fineTuneToggle, &QPushButton::clicked, this, [this]() { syncVisibility(); });

	QVBoxLayout *fine = nullptr;
	_fineTune = subBox(_motionDetails, fine);

	_motionDuration = motionFloat(0.01, 10.0, 0.05, " s", 2, [](MotionSpec &m, float v) { m.duration = v; });
	_motionStagger = motionFloat(0.0, 2.0, 0.005, " s", 3, [](MotionSpec &m, float v) { m.stagger = v; });
	_motionParam = motionFloat(-1000.0, 1000.0, 1.0, nullptr, 2, [](MotionSpec &m, float v) { m.param = v; });
	_motionSpeed = motionFloat(0.05, 5.0, 0.05, " Hz", 2, [](MotionSpec &m, float v) { m.speed = v; });

	_motionDurationRow = row(tr_("Motion.Duration"), _motionDuration, _fineTune);
	_motionStaggerRow = row(tr_("Motion.Stagger"), _motionStagger, _fineTune);
	_motionParamRow = row(tr_("Motion.Amount"), _motionParam, _fineTune);
	_motionSpeedRow = row(tr_("Motion.Speed"), _motionSpeed, _fineTune);

	_motionParamLabel = _motionParamRow->findChild<QLabel *>();

	fine->addWidget(_motionDurationRow);
	fine->addWidget(_motionStaggerRow);
	fine->addWidget(_motionParamRow);
	fine->addWidget(_motionSpeedRow);

	details->addWidget(_motionTriggerRow);
	details->addWidget(orderLabel);
	details->addWidget(_motionOrder);
	details->addWidget(_motionDirectionRow);
	details->addWidget(_motionPaceRow);
	details->addWidget(_fineTuneToggle, 0, Qt::AlignLeft);
	details->addWidget(_fineTune);

	body->addWidget(_motionDetails);

	// ---- scrolling ----
	body->addWidget(switchRow(panel, tr_("Editor.Scroll"), _scrollEnabled, tr_("Editor.Scroll.About")));

	connect(_scrollEnabled, &ui::Switch::clicked, this, [this](bool on) {
		applyDocument([on](Document &d) { d.scroll.enabled = on; });
		syncEditMode();
	});

	QVBoxLayout *scroll = nullptr;
	_scrollBox = subBox(panel, scroll);

	auto scrollFloat = [this](const char *key, auto setter) {
		auto *box = spin(_scrollBox, -4000.0, 4000.0, 5.0);

		box->setToolTip(tr_(key));
		box->setAccessibleName(tr_(key));

		connect(box, &QDoubleSpinBox::valueChanged, this,
			[this, setter](double v) { applyDocument([&](Document &d) { setter(d.scroll, float(v)); }); });

		return box;
	};

	_scrollX = scrollFloat("Scroll.X", [](ScrollSpec &s, float v) { s.speedX = v; });
	_scrollY = scrollFloat("Scroll.Y", [](ScrollSpec &s, float v) { s.speedY = v; });

	_scrollLoop = new QCheckBox(tr_("Scroll.Loop"), _scrollBox);

	connect(_scrollLoop, &QCheckBox::clicked, this,
		[this](bool on) { applyDocument([on](Document &d) { d.scroll.loop = on; }); });

	_scrollGap = spin(_scrollBox, 0.0, 4000.0, 4.0, " px");

	connect(_scrollGap, &QDoubleSpinBox::valueChanged, this,
		[this](double v) { applyDocument([v](Document &d) { d.scroll.gap = float(v); }); });

	_scrollGapRow = row(tr_("Scroll.Gap"), _scrollGap, _scrollBox);

	scroll->addWidget(row(tr_("Editor.Scroll.Across"), _scrollX, _scrollBox));
	scroll->addWidget(row(tr_("Editor.Scroll.Down"), _scrollY, _scrollBox));
	scroll->addWidget(caption(tr_("Editor.Scroll.Hint"), _scrollBox));
	scroll->addWidget(_scrollLoop);
	scroll->addWidget(_scrollGapRow);

	body->addWidget(_scrollBox);
}

// ---------------------------------------------------------------------------
// More
// ---------------------------------------------------------------------------

void EditorWindow::buildMoreSection()
{
	QWidget *panel = _moreSection->bodyWidget();
	QVBoxLayout *body = _moreSection->body();

	auto heading = [](const QString &text, QWidget *parent) {
		auto *label = new QLabel(text.toUpper(), parent);

		label->setObjectName("groupLabel");

		return label;
	};

	// ---- per character ----
	auto styleFloat = [this, panel](auto setter) {
		auto *box = spin(panel, -200.0, 200.0, 0.5, " px");

		connect(box, &QDoubleSpinBox::valueChanged, this, [this, setter](double v) {
			StyleOverride d;
			setter(d, float(v));
			applyStyle(d);
		});

		return box;
	};

	_letterSpacing = styleFloat([](StyleOverride &d, float v) { d.letterSpacing = v; });
	_wordSpacing = styleFloat([](StyleOverride &d, float v) { d.wordSpacing = v; });

	_tabularDigits = new QCheckBox(tr_("TabularDigits"), panel);
	_tabularDigits->setToolTip(tr_("TabularDigits.Hint"));

	connect(_tabularDigits, &QCheckBox::clicked, this, [this](bool on) {
		StyleOverride d;
		d.tabularDigits = on;
		applyStyle(d);
	});

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

	body->addWidget(heading(tr_("Editor.More.Type"), panel));
	body->addWidget(row(tr_("LetterSpacing"), _letterSpacing, panel));
	body->addWidget(row(tr_("Editor.WordSpacing"), _wordSpacing, panel));
	body->addWidget(_tabularDigits);
	body->addWidget(row(tr_("Editor.Features"), _features, panel));
	body->addWidget(row(tr_("Editor.Language"), _language, panel));

	// Variable-font axes are read from the font, so each font exposes exactly
	// the axes it has.
	_axisBox = new QWidget(panel);

	auto *axisOuter = new QVBoxLayout(_axisBox);
	axisOuter->setContentsMargins(0, 0, 0, 0);
	axisOuter->addWidget(heading(tr_("Editor.Axes"), _axisBox));

	_axisLayout = new QFormLayout();
	axisOuter->addLayout(_axisLayout);

	auto *rescan = new QPushButton(tr_("Editor.RescanFonts"), panel);
	rescan->setObjectName("link");
	rescan->setCursor(Qt::PointingHandCursor);

	connect(rescan, &QPushButton::clicked, this, [this]() {
		FontManager::instance().refresh();
		_axisFontKey.clear();
		populateFamilies();
		syncControls();
	});

	body->addWidget(_axisBox);
	body->addWidget(rescan, 0, Qt::AlignLeft);

	// ---- the whole text ----
	_wholeTextBox = new QWidget(panel);

	auto *whole = new QVBoxLayout(_wholeTextBox);
	whole->setContentsMargins(0, 0, 0, 0);
	whole->setSpacing(8);

	_opacity = new QSpinBox(_wholeTextBox);
	_opacity->setRange(0, 100);
	_opacity->setSuffix(" %");
	_opacity->setKeyboardTracking(false);

	connect(_opacity, &QSpinBox::valueChanged, this,
		[this](int v) { applyDocument([v](Document &d) { d.opacity = float(v) / 100.0f; }); });

	_direction = enumCombo<Direction>(_wholeTextBox, {{Direction::Auto, "Direction.Auto"},
							  {Direction::LTR, "Direction.LTR"},
							  {Direction::RTL, "Direction.RTL"}});

	connect(_direction, &QComboBox::activated, this, [this]() {
		ParagraphOverride d;
		d.direction = comboValue<Direction>(_direction);
		applyParagraph(d);
	});

	auto paragraphFloat = [this](auto setter) {
		auto *box = spin(_wholeTextBox, 0.0, 1000.0, 1.0, " px", 2);

		connect(box, &QDoubleSpinBox::valueChanged, this, [this, setter](double v) {
			ParagraphOverride d;
			setter(d, float(v));
			applyParagraph(d);
		});

		return box;
	};

	_spaceBefore = paragraphFloat([](ParagraphOverride &d, float v) { d.spaceBefore = v; });
	_spaceAfter = paragraphFloat([](ParagraphOverride &d, float v) { d.spaceAfter = v; });

	whole->addWidget(heading(tr_("Editor.More.Whole"), _wholeTextBox));
	whole->addWidget(row(tr_("Opacity"), _opacity, _wholeTextBox));
	whole->addWidget(row(tr_("Direction"), _direction, _wholeTextBox));
	whole->addWidget(row(tr_("Editor.SpaceBefore"), _spaceBefore, _wholeTextBox));
	whole->addWidget(row(tr_("Editor.SpaceAfter"), _spaceAfter, _wholeTextBox));

	// ---- where the text comes from ----
	whole->addWidget(heading(tr_("Mode"), _wholeTextBox));

	_textFrom = new ui::Segmented(_wholeTextBox);
	_textFrom->setAccessibleName(tr_("Mode"));
	_textFrom->addOption(tr_("Mode.Text"), 0);
	_textFrom->addOption(tr_("Mode.File"), 1);

	connect(_textFrom, &ui::Segmented::chosen, this, [this](int value) {
		writeSettings([value](obs_data_t *data) { obs_data_set_string(data, "mode", value ? "file" : "text"); },
			      true);
	});

	QVBoxLayout *file = nullptr;
	_fileBox = subBox(_wholeTextBox, file);

	auto pathField = [this](QWidget *parent, const char *key, const char *filterKey, const char *nameKey,
				QLineEdit *&edit) {
		auto *holder = new QWidget(parent);
		auto *layout = new QHBoxLayout(holder);

		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(6);

		edit = new QLineEdit(holder);
		edit->setAccessibleName(tr_(nameKey));
		edit->setPlaceholderText(tr_(nameKey));

		auto *browse = new QPushButton(tr_("Editor.Browse"), holder);
		browse->setObjectName("ghost");

		QLineEdit *field = edit;

		auto store = [this, key, field]() {
			if (_updating)
				return;

			const std::string path = field->text().trimmed().toStdString();

			writeSettings([key, path](obs_data_t *data) { obs_data_set_string(data, key, path.c_str()); },
				      false);
		};

		connect(edit, &QLineEdit::editingFinished, this, store);

		connect(browse, &QPushButton::clicked, this, [this, field, filterKey, nameKey, store]() {
			const QString picked =
				QFileDialog::getOpenFileName(this, tr_(nameKey), field->text(), tr_(filterKey));

			if (picked.isEmpty())
				return;

			field->setText(picked);
			store();
		});

		layout->addWidget(edit, 1);
		layout->addWidget(browse);

		return holder;
	};

	file->addWidget(pathField(_fileBox, "file", "File.Filter", "File", _filePath));

	_chatlog = new QCheckBox(tr_("Chatlog"), _fileBox);

	connect(_chatlog, &QCheckBox::clicked, this, [this](bool on) {
		writeSettings([on](obs_data_t *data) { obs_data_set_bool(data, "chatlog", on); }, false);
		syncVisibility();
	});

	_chatlogLines = new QSpinBox(_fileBox);
	_chatlogLines->setRange(1, 1000);
	_chatlogLines->setKeyboardTracking(false);
	_chatlogLines->setPrefix(tr_("Chatlog.Lines") + "  ");
	_chatlogLines->setAccessibleName(tr_("Chatlog.Lines"));

	connect(_chatlogLines, &QSpinBox::valueChanged, this, [this](int v) {
		if (_updating)
			return;

		writeSettings([v](obs_data_t *data) { obs_data_set_int(data, "chatlog_lines", v); }, false);
	});

	file->addWidget(_chatlog);
	file->addWidget(_chatlogLines);

	whole->addWidget(_textFrom);
	whole->addWidget(_fileBox);

	// ---- for scripts and bots ----
	whole->addWidget(heading(tr_("Editor.More.Scripts"), _wholeTextBox));

	_markupBox = new QCheckBox(tr_("Markup"), _wholeTextBox);
	_markupBox->setToolTip(tr_("Markup.Hint"));

	connect(_markupBox, &QCheckBox::clicked, this, [this](bool on) {
		// Switching mode keeps the words; inline styling only exists in the
		// mode it was made in.
		_markup = on;

		writeSettings(
			[this, on](obs_data_t *data) {
				obs_data_set_bool(data, "markup", on);
				settings::save(data, _document, on);
			},
			true);
	});

	whole->addWidget(_markupBox);
	whole->addWidget(caption(tr_("Editor.Markup.About"), _wholeTextBox));
	whole->addWidget(
		pathField(_wholeTextBox, "variables_file", "Variables.File.Filter", "Variables.File", _variablesFile));

	auto *restart = new QPushButton(tr_("Editor.StartOver"), _wholeTextBox);
	restart->setObjectName("link");
	restart->setCursor(Qt::PointingHandCursor);

	connect(restart, &QPushButton::clicked, this, [this]() { showStartPage(true); });

	whole->addWidget(restart, 0, Qt::AlignLeft);

	body->addWidget(_wholeTextBox);
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

	_axisBox->setVisible(!_axisSliders.empty());
}

// ---------------------------------------------------------------------------
// State to screen
// ---------------------------------------------------------------------------

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

	syncScope(selection, start, end);

	// ---- text ----
	_fontFamily->setCurrentText(QString::fromStdString(style.font.family));
	_fontSize->setValue(int(std::lround(style.sizePx)));

	int nearest = 0;

	for (int i = 0; i < _fontWeight->count(); i++)
		if (std::abs(_fontWeight->itemData(i).toInt() - style.font.weight) <
		    std::abs(_fontWeight->itemData(nearest).toInt() - style.font.weight))
			nearest = i;

	_fontWeight->setCurrentIndex(nearest);
	_fontBold->setChecked(style.font.weight >= 600);
	_fontItalic->setChecked(style.font.italic);
	_fontUnderline->setChecked(style.underline);
	_fontStrikeout->setChecked(style.strikeout);

	_fillColor->setColor(style.fill.color);
	_fillColor2->setColor(style.fill.color2);
	_fillMode->setValue(style.fill.type == FillType::Solid ? 0 : 1);
	_gradientAngle->setValue(double(style.fill.angleDeg));
	setCombo(_gradientSpan, int(style.fill.span));

	if (style.fill.type != FillType::Solid)
		setCombo(_gradientType, int(style.fill.type));

	_quickSize->setText(QString::number(int(std::lround(style.sizePx))));
	_quickBold->setChecked(style.font.weight >= 600);
	_quickItalic->setChecked(style.font.italic);
	_quickColor->setColor(style.fill.color);

	// With a selection, outline what it has that the rest of the text does not.
	const StyleOverride differs = selection ? StyleOverride::diff(_document.base, style) : StyleOverride{};

	markDiffers(_fontFamily, differs.family.has_value());
	markDiffers(_fontSize, differs.size.has_value());
	markDiffers(_fontWeight, differs.weight.has_value());
	markDiffers(_fillColor, differs.color.has_value());

	_differsLegend->setVisible(differs.family || differs.size || differs.weight || differs.color);

	// ---- effects ----
	_outlineEnabled->setChecked(style.outline.enabled);
	_outlineWidth->setValue(double(style.outline.widthPx));
	_outlineColor->setColor(style.outline.color);
	setCombo(_outlineJoin, int(style.outline.join));

	_shadowEnabled->setChecked(style.shadow.enabled);
	_shadowX->setValue(double(style.shadow.offsetX));
	_shadowY->setValue(double(style.shadow.offsetY));
	_shadowColor->setColor(style.shadow.color);

	_background->setChecked(_document.background.enabled);
	_backgroundColor->setColor(_document.background.color);
	_backgroundRadius->setValue(double(_document.background.radius));
	_padding->setValue(double(_document.padding));

	// ---- layout ----
	_align->setValue(int(paragraph.align));
	_sizeMode->setValue(int(_document.sizeMode));
	_boxWidth->setValue(int(std::lround(_document.boxWidth)));
	_boxHeight->setValue(int(std::lround(_document.boxHeight)));
	setCombo(_wrap, int(_document.wrap));
	_valign->setValue(int(_document.valign));
	_lineHeight->setValue(double(paragraph.lineHeight));

	// ---- motion ----
	const MotionSpec &m = _document.motion;

	for (const auto &[motion, tile] : _motionTiles)
		tile->setChecked(motion == int(m.motion));

	setCombo(_motionTrigger, int(m.trigger));
	_motionChangedOnly->setChecked(m.scope == MotionScope::Changed);
	_motionExit->setChecked(m.exit);
	_motionLoopInterval->setValue(double(m.loopInterval));
	_motionOrder->setValue(int(m.order));
	setCombo(_motionDirection, int(m.direction));
	_motionPace->setValue(durationToPace(m.duration));
	_motionDuration->setValue(double(m.duration));
	_motionStagger->setValue(double(m.stagger));
	_motionSpeed->setValue(double(m.speed));

	const MotionParamSpec amount = motionParamSpec(m.motion);

	if (amount.label) {
		if (_motionParamLabel)
			_motionParamLabel->setText(tr_(amount.label));

		_motionParam->setRange(double(amount.min), double(amount.max));
		_motionParam->setSingleStep(double(amount.step));
		_motionParam->setValue(double(m.param));
	}

	_scrollEnabled->setChecked(_document.scroll.enabled);
	_scrollX->setValue(double(_document.scroll.speedX));
	_scrollY->setValue(double(_document.scroll.speedY));
	_scrollLoop->setChecked(_document.scroll.loop);
	_scrollGap->setValue(double(_document.scroll.gap));

	// ---- more ----
	_letterSpacing->setValue(double(style.letterSpacing));
	_wordSpacing->setValue(double(style.wordSpacing));
	_tabularDigits->setChecked(style.tabularDigits);
	_features->setText(QString::fromStdString(style.features));
	_language->setText(QString::fromStdString(style.language));

	_opacity->setValue(std::clamp(int(std::lround(_document.opacity * 100.0f)), 0, 100));
	setCombo(_direction, int(paragraph.direction));
	_spaceBefore->setValue(double(paragraph.spaceBefore));
	_spaceAfter->setValue(double(paragraph.spaceAfter));

	_updating = false;

	syncVisibility();
	syncSummaries();
	rebuildAxisControls();
	syncOverlay();
}

void EditorWindow::syncScope(bool selection, size_t start, size_t end)
{
	if (_scopeHeader->property("selection").toBool() != selection ||
	    !_scopeHeader->property("selection").isValid()) {
		_scopeHeader->setProperty("selection", selection);

		// The header and its labels are styled by this property.
		for (QWidget *widget : {_scopeHeader, static_cast<QWidget *>(_scopeDetail)}) {
			widget->style()->unpolish(widget);
			widget->style()->polish(widget);
		}

		for (QLabel *label : _scopeHeader->findChildren<QLabel *>()) {
			label->style()->unpolish(label);
			label->style()->polish(label);
		}
	}

	if (_fileMode) {
		_scopeTitle->setText(tr_("Editor.Scope.All"));
		_scopeDetail->setText(tr_("Editor.Scope.File"));
	} else if (_markup) {
		_scopeTitle->setText(tr_("Editor.Scope.All"));
		_scopeDetail->setText(tr_("Editor.Scope.Markup"));
	} else if (selection) {
		const std::string plain = _document.plainText();
		const QString quoted = QString::fromStdString(plain.substr(start, end - start));

		_scopeTitle->setText(tr_("Editor.Scope.Selection"));
		_scopeDetail->setText(QChar(0x201C) + shortened(quoted, 42) + QChar(0x201D));
	} else {
		_scopeTitle->setText(tr_("Editor.Scope.All"));
		_scopeDetail->setText(tr_("Editor.Scope.All.Hint"));
	}

	_clearFormatting->setVisible(selection);

	_context->setCurrentIndex(selection ? 1 : 0);
	_stylesPanel->setSelectionMode(selection);
}

void EditorWindow::syncVisibility()
{
	size_t start = 0;
	size_t end = 0;

	const bool selection = selectionRange(start, end);
	const Style style = selection ? _document.styleAt(start + 1) : _document.base;

	_gradientBox->setVisible(style.fill.type != FillType::Solid);
	_gradientAngleRow->setVisible(style.fill.type == FillType::Linear);

	_outlineBox->setVisible(style.outline.enabled);
	_shadowBox->setVisible(style.shadow.enabled);
	_backgroundBox->setVisible(_document.background.enabled);

	const bool fixed = _document.sizeMode == SizeMode::Fixed;

	_boxControls->setVisible(fixed);

	// Alignment has nothing to align within on one auto-sized line; say so
	// rather than leave a control that visibly does nothing.
	_alignHint->setVisible(!fixed && _document.blocks.size() < 2);

	const MotionSpec &m = _document.motion;

	const bool animated = m.motion != Motion::None;
	const bool timed = motionIsTransient(m.motion);
	const bool onText = timed && m.trigger == MotionTrigger::TextChange;
	const bool staggered = animated && m.order != MotionOrder::Together;

	_motionDetails->setVisible(animated);
	_motionTriggerRow->setVisible(timed);
	_motionChangedOnly->setVisible(onText);
	_motionExit->setVisible(onText);
	_motionLoopRow->setVisible(timed && m.trigger == MotionTrigger::Loop);
	_motionDirectionRow->setVisible(motionHasDirection(m.motion));
	_motionPaceRow->setVisible(timed);

	_fineTune->setVisible(_fineTuneToggle->isChecked());
	_fineTuneToggle->setIcon(
		icon(_fineTuneToggle->isChecked() ? Glyph::ChevronDown : Glyph::ChevronRight, color::accent));
	_motionDurationRow->setVisible(timed);
	_motionStaggerRow->setVisible(staggered);
	_motionParamRow->setVisible(motionParamSpec(m.motion).label != nullptr);
	_motionSpeedRow->setVisible(m.motion == Motion::Wave);

	_scrollBox->setVisible(_document.scroll.enabled);
	_scrollGapRow->setVisible(_document.scroll.loop);

	// Motion, and the settings of the text as a whole, are not something a
	// selection has.
	_motionSection->setVisible(!selection);
	_wholeTextBox->setVisible(!selection);
	_selectionNote->setVisible(selection);

	_fileBox->setVisible(_fileMode);
	_chatlogLines->setVisible(_chatlog->isChecked());
}

void EditorWindow::syncSummaries()
{
	size_t start = 0;
	size_t end = 0;

	const bool selection = selectionRange(start, end);
	const Style style = selection ? _document.styleAt(start + 1) : _document.base;
	const ParagraphStyle paragraph = selection ? _document.paragraphAt(start) : _document.paragraph;

	// ---- text ----
	_textSection->setSummary(QStringLiteral("%1, %2, %3")
					 .arg(QString::fromStdString(style.font.family))
					 .arg(int(std::lround(style.sizePx)))
					 .arg(tr_(weightKey(style.font.weight)).toLower()));

	// ---- effects ----
	QStringList effects;

	if (style.outline.enabled)
		effects << tr_("Outline");

	if (style.shadow.enabled)
		effects << tr_("Shadow");

	if (_document.background.enabled)
		effects << tr_("Editor.BackgroundBox");

	_effectsSection->setSummary(effects.isEmpty() ? tr_("Editor.Summary.None") : effects.join(", "));

	// ---- layout ----
	const char *alignKey = "Align.Start";

	switch (paragraph.align) {
	case HAlign::Center:
		alignKey = "Align.Center";
		break;
	case HAlign::End:
		alignKey = "Align.End";
		break;
	case HAlign::Justify:
		alignKey = "Align.Justify";
		break;
	case HAlign::Start:
		break;
	}

	const QString size = _document.sizeMode == SizeMode::Fixed ? QStringLiteral("%1 \u00d7 %2")
									     .arg(int(std::lround(_document.boxWidth)))
									     .arg(int(std::lround(_document.boxHeight)))
								   : tr_("Editor.Size.Auto").toLower();

	_layoutSection->setSummary(tr_(alignKey) + ", " + size);

	// ---- motion ----
	const MotionSpec &m = _document.motion;

	QStringList motion;

	if (m.motion != Motion::None) {
		QString name = tr_(motionKey(m.motion));

		if (motionIsTransient(m.motion)) {
			const char *when = "Editor.Summary.Never";

			switch (m.trigger) {
			case MotionTrigger::TextChange:
				when = "Editor.Summary.TextChange";
				break;
			case MotionTrigger::Show:
				when = "Editor.Summary.Show";
				break;
			case MotionTrigger::Activate:
				when = "Editor.Summary.Activate";
				break;
			case MotionTrigger::Loop:
				when = "Editor.Summary.Loop";
				break;
			case MotionTrigger::Never:
				break;
			}

			name += ", " + tr_(when);
		}

		motion << name;
	}

	if (_document.scroll.enabled)
		motion << tr_("Editor.Summary.Scroll");

	_motionSection->setSummary(motion.isEmpty() ? tr_("Motion.None") : motion.join(", "));

	_moreSection->setSummary(tr_(selection ? "Editor.Summary.More.Selection" : "Editor.Summary.More"));
}

} // namespace slugged
