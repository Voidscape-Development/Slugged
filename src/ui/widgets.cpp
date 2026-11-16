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

#include "widgets.hpp"

#include <QButtonGroup>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace slugged {
namespace ui {

namespace {

QPainterPath glyphPath(Glyph glyph, bool &filled)
{
	// All on a 24 x 24 grid.
	QPainterPath p;

	filled = false;

	switch (glyph) {
	case Glyph::Undo:
		p.moveTo(9, 14);
		p.lineTo(4, 9);
		p.lineTo(9, 4);
		p.moveTo(4, 9);
		p.lineTo(14, 9);
		p.arcTo(QRectF(8, 9, 12, 12), 90, -180);
		p.lineTo(11, 21);
		break;
	case Glyph::Redo:
		p.moveTo(15, 14);
		p.lineTo(20, 9);
		p.lineTo(15, 4);
		p.moveTo(20, 9);
		p.lineTo(10, 9);
		p.arcTo(QRectF(4, 9, 12, 12), 90, 180);
		p.lineTo(13, 21);
		break;
	case Glyph::Plus:
		p.moveTo(12, 5);
		p.lineTo(12, 19);
		p.moveTo(5, 12);
		p.lineTo(19, 12);
		break;
	case Glyph::Close:
	case Glyph::Remove:
		p.moveTo(6, 6);
		p.lineTo(18, 18);
		p.moveTo(18, 6);
		p.lineTo(6, 18);
		break;
	case Glyph::Play:
		filled = true;
		p.moveTo(7, 4);
		p.lineTo(20, 12);
		p.lineTo(7, 20);
		p.closeSubpath();
		break;
	case Glyph::Pause:
		p.moveTo(8, 5);
		p.lineTo(8, 19);
		p.moveTo(16, 5);
		p.lineTo(16, 19);
		break;
	case Glyph::Reset:
		p.moveTo(4, 4);
		p.lineTo(4, 9);
		p.lineTo(9, 9);
		p.moveTo(4.6, 9);
		p.arcTo(QRectF(3, 3, 18, 18), 160, -320);
		break;
	case Glyph::ChevronDown:
		p.moveTo(6, 9);
		p.lineTo(12, 15);
		p.lineTo(18, 9);
		break;
	case Glyph::ChevronRight:
		p.moveTo(9, 6);
		p.lineTo(15, 12);
		p.lineTo(9, 18);
		break;
	case Glyph::Search:
		p.addEllipse(QRectF(4, 4, 14, 14));
		p.moveTo(21, 21);
		p.lineTo(16.7, 16.7);
		break;
	case Glyph::AlignStart:
		p.moveTo(4, 6);
		p.lineTo(20, 6);
		p.moveTo(4, 12);
		p.lineTo(14, 12);
		p.moveTo(4, 18);
		p.lineTo(18, 18);
		break;
	case Glyph::AlignCenter:
		p.moveTo(4, 6);
		p.lineTo(20, 6);
		p.moveTo(7, 12);
		p.lineTo(17, 12);
		p.moveTo(5, 18);
		p.lineTo(19, 18);
		break;
	case Glyph::AlignEnd:
		p.moveTo(4, 6);
		p.lineTo(20, 6);
		p.moveTo(10, 12);
		p.lineTo(20, 12);
		p.moveTo(6, 18);
		p.lineTo(20, 18);
		break;
	case Glyph::AlignJustify:
		p.moveTo(4, 6);
		p.lineTo(20, 6);
		p.moveTo(4, 12);
		p.lineTo(20, 12);
		p.moveTo(4, 18);
		p.lineTo(20, 18);
		break;
	}

	return p;
}

QPixmap glyphPixmap(Glyph glyph, const QColor &tint, int size, qreal ratio)
{
	QPixmap pixmap(QSize(size, size) * ratio);
	pixmap.setDevicePixelRatio(ratio);
	pixmap.fill(Qt::transparent);

	bool filled = false;
	const QPainterPath path = glyphPath(glyph, filled);

	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.scale(size / 24.0, size / 24.0);

	if (filled) {
		painter.setPen(Qt::NoPen);
		painter.setBrush(tint);
	} else {
		painter.setPen(QPen(tint, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		painter.setBrush(Qt::NoBrush);
	}

	painter.drawPath(path);

	return pixmap;
}

// Style sheets can only take images by path, so the two the sheet needs -- a
// combo box's arrow and a check mark -- are drawn once into the temp folder.
QString sheetImage(const char *name, Glyph glyph, const QColor &tint)
{
	const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/slugged-editor";
	const QString path = dir + "/" + name + ".png";

	if (!QFile::exists(path)) {
		QDir().mkpath(dir);
		glyphPixmap(glyph, tint, 28, 1.0).save(path, "PNG");
	}

	return path;
}

QPixmap checkPixmap()
{
	QPixmap pixmap(28, 28);
	pixmap.fill(Qt::transparent);

	QPainterPath path;
	path.moveTo(6, 15);
	path.lineTo(12, 21);
	path.lineTo(23, 8);

	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(QPen(color::onAccent, 3.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	painter.drawPath(path);

	return pixmap;
}

QString checkImage()
{
	const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/slugged-editor";
	const QString path = dir + "/check.png";

	if (!QFile::exists(path)) {
		QDir().mkpath(dir);
		checkPixmap().save(path, "PNG");
	}

	return path;
}

} // namespace

QIcon icon(Glyph glyph, const QColor &tint)
{
	QIcon out;

	out.addPixmap(glyphPixmap(glyph, tint, 24, 1.0));
	out.addPixmap(glyphPixmap(glyph, tint, 24, 2.0));

	return out;
}

QString styleSheet()
{
	static const char *const sheet = R"(
QWidget { color: #ecedf1; font-size: 13px; }
#editorRoot, #startPage, #canvasColumn { background: #15161a; }
#topBar, #bottomBar, #sidePanel, #sidePanel QScrollArea, #sideContent, #insertPanel, #stylesPanel,
#panelContent { background: #1d1f25; }
#topBar { border-bottom: 1px solid #353945; }
#bottomBar, #contextBar { border-top: 1px solid #353945; }
#contextBar { background: #23262d; }
#sidePanel, #insertPanel, #stylesPanel { border-left: 1px solid #353945; }

QLabel { background: transparent; }
QLabel#brand { color: #f5c84b; font-size: 15px; font-weight: 700; }
QLabel#muted, QLabel#caption { color: #a3a8b5; font-size: 12px; }
QLabel#eyebrow { color: #a3a8b5; font-size: 11px; font-weight: 600; }
QLabel#panelTitle, QLabel#scopeTitle { font-size: 15px; font-weight: 700; }
QLabel#pageTitle { font-size: 26px; font-weight: 700; }
QLabel#groupLabel { color: #a3a8b5; font-size: 11px; font-weight: 600; padding-top: 8px; }
QLabel#timerValue { font-weight: 600; }

#scopeHeader { background: #1d1f25; border-bottom: 1px solid #353945; }
#scopeHeader[selection="true"] { background: #2b3352; }
#scopeHeader[selection="true"] QLabel#eyebrow, #scopeHeader[selection="true"] QLabel#muted { color: #c9d2f5; }

QPushButton { background: #282b33; border: 1px solid #353945; border-radius: 7px; padding: 0 12px;
              min-height: 30px; }
QPushButton:hover { border-color: #555b6b; }
QPushButton:pressed { background: #353945; }
QPushButton:disabled { color: #6f7483; }
QPushButton:focus { border-color: #8f9bd0; }
QPushButton:checked { background: #f5c84b; border-color: #f5c84b; color: #1a1500; font-weight: 600; }
QPushButton#primary { background: #f5c84b; border-color: #f5c84b; color: #1a1500; font-weight: 700;
                      padding: 0 18px; }
QPushButton#primary:hover { background: #ffd866; }
QPushButton#ghost { background: transparent; }
QPushButton#link { background: transparent; border: none; color: #f5c84b; padding: 0; text-align: left; }
QPushButton#link:hover { color: #ffe08a; }
QPushButton#iconButton { padding: 0; min-width: 30px; max-width: 30px; background: transparent; }
QPushButton#iconButton:checked { background: #f5c84b; }
QPushButton#flatIcon { padding: 0; min-width: 28px; max-width: 28px; background: transparent; border: none; }
QPushButton#flatIcon:hover { background: #282b33; }
QPushButton#toggle { padding: 0; min-width: 32px; max-width: 32px; }
QPushButton#nameButton { background: transparent; border: 1px solid transparent; font-weight: 600;
                         padding: 0 8px; }
QPushButton#nameButton:hover { border-color: #353945; }
QPushButton#colorButton { text-align: left; padding: 0 10px 0 6px; }

QPushButton#sectionHeader { background: transparent; border: none; border-radius: 0; min-height: 40px;
                            padding: 0 14px; text-align: left; font-weight: 600; }
QPushButton#sectionHeader:hover { background: #23262d; }
#section { border-bottom: 1px solid #353945; }

#segmented { background: #15161a; border-radius: 9px; }
#segmented QPushButton { background: transparent; border: none; border-radius: 6px; color: #a3a8b5;
                         min-height: 28px; padding: 0 6px; }
#segmented QPushButton:hover { color: #ecedf1; }
#segmented QPushButton:checked { background: #353945; color: #ecedf1; font-weight: 600; }

QPushButton#motionTile { min-height: 36px; padding: 0 4px; }

QPushButton#listRow { background: transparent; border: none; border-radius: 7px; min-height: 36px;
                      padding: 0 10px; text-align: left; }
QPushButton#listRow:hover { background: #282b33; }

#card { background: #1d1f25; border: 1px solid #353945; border-radius: 11px; padding: 0; min-height: 190px; }
#card:hover { border-color: #f5c84b; }
#card:focus { border-color: #8f9bd0; }
#notice { background: #1d1f25; border: 1px solid #353945; border-radius: 11px; }
#expanded { background: #282b33; border: 1px solid #f5c84b; border-radius: 9px; }
#styleRow { background: #282b33; border: 1px solid #353945; border-radius: 9px; padding: 0; min-height: 58px;
            max-height: 58px; }
#styleRow:hover { border-color: #f5c84b; }

QLineEdit, QAbstractSpinBox, QComboBox, QPlainTextEdit {
    background: #282b33; border: 1px solid #353945; border-radius: 7px; padding: 0 8px; min-height: 30px;
    selection-background-color: #3d4a7a; selection-color: #ffffff; }
QPlainTextEdit { padding: 6px 8px; background: #1a1b20; }
QLineEdit:focus, QAbstractSpinBox:focus, QComboBox:focus, QPlainTextEdit:focus { border-color: #8f9bd0; }
QLineEdit[differs="true"], QAbstractSpinBox[differs="true"], QComboBox[differs="true"],
QPushButton[differs="true"] { border-color: #8f9bd0; }
QAbstractSpinBox::up-button, QAbstractSpinBox::down-button { width: 0; height: 0; border: none; }
QComboBox::drop-down { border: none; width: 24px; }
QComboBox::down-arrow { image: url(%ARROW%); width: 12px; height: 12px; }
QComboBox QAbstractItemView { background: #23262d; border: 1px solid #555b6b; outline: 0;
    selection-background-color: #3d4a7a; selection-color: #ffffff; }

QCheckBox { spacing: 8px; min-height: 28px; background: transparent; }
QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid #555b6b; border-radius: 4px;
                       background: #282b33; }
QCheckBox::indicator:checked { background: #f5c84b; border-color: #f5c84b; image: url(%CHECK%); }

QSlider::groove:horizontal { height: 4px; background: #353945; border-radius: 2px; }
QSlider::sub-page:horizontal { background: #f5c84b; border-radius: 2px; }
QSlider::handle:horizontal { width: 14px; height: 14px; margin: -5px 0; border-radius: 7px;
                             background: #ecedf1; }

QScrollArea { border: none; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: #353945; border-radius: 4px; min-height: 28px; margin: 2px; }
QScrollBar::handle:vertical:hover { background: #555b6b; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }

QMenu { background: #23262d; border: 1px solid #555b6b; padding: 4px; }
QMenu::item { padding: 6px 18px; border-radius: 5px; }
QMenu::item:selected { background: #3d4a7a; }
QToolTip { background: #23262d; color: #ecedf1; border: 1px solid #555b6b; padding: 4px 6px; }
)";

	QString out = QString::fromUtf8(sheet);

	out.replace("%ARROW%", sheetImage("arrow", Glyph::ChevronDown, color::muted));
	out.replace("%CHECK%", checkImage());

	return out;
}

// ---- Switch ------------------------------------------------------------------

Switch::Switch(QWidget *parent) : QAbstractButton(parent)
{
	setCheckable(true);
	setCursor(Qt::PointingHandCursor);
	setFocusPolicy(Qt::TabFocus);
}

QSize Switch::sizeHint() const
{
	return QSize(40, 22);
}

void Switch::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);

	const QRectF track(0.5, (height() - 22) / 2.0 + 0.5, 39.0, 21.0);

	painter.setPen(hasFocus() ? QPen(QColor(0x8f, 0x9b, 0xd0), 1.0) : Qt::NoPen);
	painter.setBrush(isChecked() ? color::accent : color::strongBorder);
	painter.drawRoundedRect(track, 10.5, 10.5);

	const qreal x = isChecked() ? track.right() - 18.5 : track.left() + 2.5;

	painter.setPen(Qt::NoPen);
	painter.setBrush(isChecked() ? color::ground : color::text);
	painter.drawEllipse(QRectF(x, track.top() + 2.5, 16.0, 16.0));
}

// ---- Segmented ---------------------------------------------------------------

Segmented::Segmented(QWidget *parent) : QFrame(parent)
{
	setObjectName("segmented");

	_layout = new QHBoxLayout(this);
	_layout->setContentsMargins(3, 3, 3, 3);
	_layout->setSpacing(3);

	_group = new QButtonGroup(this);
	_group->setExclusive(true);

	connect(_group, &QButtonGroup::idClicked, this, [this](int id) { emit chosen(id); });
}

void Segmented::add(QPushButton *button, int value)
{
	button->setCheckable(true);
	button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

	_group->addButton(button, value);
	_layout->addWidget(button);
}

void Segmented::addOption(const QString &label, int value)
{
	add(new QPushButton(label, this), value);
}

void Segmented::addOption(const QIcon &glyph, const QString &tooltip, int value)
{
	auto *button = new QPushButton(this);

	button->setIcon(glyph);
	button->setToolTip(tooltip);
	button->setAccessibleName(tooltip);

	add(button, value);
}

int Segmented::value() const
{
	return _group->checkedId();
}

void Segmented::setValue(int value)
{
	if (QAbstractButton *button = _group->button(value)) {
		button->setChecked(true);
		return;
	}

	// Nothing matches: show nothing as chosen rather than the wrong thing.
	if (QAbstractButton *checked = _group->checkedButton()) {
		_group->setExclusive(false);
		checked->setChecked(false);
		_group->setExclusive(true);
	}
}

// ---- Section -----------------------------------------------------------------

Section::Section(const QString &title, QWidget *parent) : QWidget(parent)
{
	setObjectName("section");
	setAttribute(Qt::WA_StyledBackground);

	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 1);
	outer->setSpacing(0);

	_header = new QPushButton(title, this);
	_header->setObjectName("sectionHeader");
	_header->setIconSize(QSize(14, 14));
	_header->setCursor(Qt::PointingHandCursor);

	// The summary sits inside the header button, so the whole row is one
	// click target.
	auto *headerLayout = new QHBoxLayout(_header);
	headerLayout->setContentsMargins(120, 0, 14, 0);

	_summary = new QLabel(_header);
	_summary->setObjectName("muted");
	_summary->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	_summary->setAttribute(Qt::WA_TransparentForMouseEvents);
	_summary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

	headerLayout->addWidget(_summary, 1);

	_body = new QWidget(this);
	_bodyLayout = new QVBoxLayout(_body);
	_bodyLayout->setContentsMargins(14, 2, 14, 14);
	_bodyLayout->setSpacing(8);

	outer->addWidget(_header);
	outer->addWidget(_body);

	connect(_header, &QPushButton::clicked, this, [this]() {
		setExpanded(!_expanded);
		emit toggled(_expanded);
	});

	setExpanded(true);
}

void Section::setSummary(const QString &summary)
{
	_summary->setText(summary);
	_summary->setToolTip(summary);
}

void Section::setExpanded(bool expanded)
{
	_expanded = expanded;

	_body->setVisible(expanded);
	_summary->setVisible(!expanded);
	_header->setIcon(icon(expanded ? Glyph::ChevronDown : Glyph::ChevronRight));
}

// ---- helpers -----------------------------------------------------------------

QWidget *row(const QString &label, QWidget *control, QWidget *parent)
{
	auto *holder = new QWidget(parent);
	auto *layout = new QHBoxLayout(holder);

	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(8);

	auto *text = new QLabel(label, holder);
	text->setObjectName("muted");
	text->setBuddy(control);

	layout->addWidget(text, 2);
	layout->addWidget(control, 3);

	return holder;
}

QLabel *caption(const QString &text, QWidget *parent)
{
	auto *label = new QLabel(text, parent);

	label->setObjectName("caption");
	label->setWordWrap(true);

	return label;
}

QPushButton *iconButton(Glyph glyph, const QString &tooltip, QWidget *parent)
{
	auto *button = new QPushButton(parent);

	button->setObjectName("iconButton");
	button->setIcon(icon(glyph));
	button->setIconSize(QSize(16, 16));
	button->setToolTip(tooltip);
	button->setAccessibleName(tooltip);

	return button;
}

} // namespace ui
} // namespace slugged
