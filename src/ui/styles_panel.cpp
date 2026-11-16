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

#include "styles_panel.hpp"
#include "editor_common.hpp"
#include "widgets.hpp"
#include "../obs/saved_styles.hpp"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <obs-module.h>

namespace slugged {

namespace {

using ui::motionKey;
using ui::tr_;
using ui::weightKey;

QColor toQColor(const Rgba &c)
{
	QColor q;
	q.setRgbF(qreal(c.r), qreal(c.g), qreal(c.b), qreal(c.a));
	return q;
}

// One line saying what a style is made of.
QString describe(const SavedStyle &style)
{
	QStringList parts;

	parts << QString::fromStdString(style.style.font.family);
	parts << tr_(weightKey(style.style.font.weight)).toLower();

	if (style.style.outline.enabled)
		parts << tr_("Outline").toLower();

	if (style.style.shadow.enabled)
		parts << tr_("Shadow").toLower();

	if (style.background.enabled)
		parts << tr_("Background").toLower();

	if (style.hasMotion && style.motion.motion != Motion::None)
		parts << tr_(motionKey(style.motion.motion)).toLower();

	return parts.join(", ");
}

// "Aa" in the style, as near as Qt's own text drawing gets to it.
QPixmap sample(const SavedStyle &style, QSize size, qreal ratio)
{
	QPixmap pixmap(size * ratio);
	pixmap.setDevicePixelRatio(ratio);
	pixmap.fill(Qt::transparent);

	QPainter p(&pixmap);
	p.setRenderHint(QPainter::Antialiasing);

	const QRectF area(QPointF(0, 0), QSizeF(size));

	p.setPen(Qt::NoPen);
	p.setBrush(ui::color::ground);
	p.drawRoundedRect(area, 6, 6);

	if (style.background.enabled) {
		p.setBrush(toQColor(style.background.color));
		p.drawRoundedRect(area.adjusted(8, 8, -8, -8), std::min(style.background.radius, 8.0f),
				  std::min(style.background.radius, 8.0f));
	}

	QFont font(QString::fromStdString(style.style.font.family));
	font.setPixelSize(22);
	font.setWeight(QFont::Weight(std::clamp(style.style.font.weight, 100, 900)));
	font.setItalic(style.style.font.italic);
	font.setUnderline(style.style.underline);

	const QString text = QStringLiteral("Aa");
	const QFontMetricsF metrics(font);
	const QRectF bounds = metrics.boundingRect(text);

	QPainterPath path;
	path.addText(area.center().x() - bounds.width() / 2.0 - bounds.left(),
		     area.center().y() + (metrics.ascent() - metrics.descent()) / 2.0, font, text);

	if (style.style.shadow.enabled)
		p.fillPath(path.translated(1.5, 1.5), toQColor(style.style.shadow.color));

	if (style.style.outline.enabled) {
		p.setPen(QPen(toQColor(style.style.outline.color), 3.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		p.setBrush(Qt::NoBrush);
		p.drawPath(path);
	}

	if (style.style.fill.type == FillType::Solid) {
		p.fillPath(path, toQColor(style.style.fill.color));
	} else {
		QLinearGradient ramp(area.topLeft(), area.bottomLeft());
		ramp.setColorAt(0.0, toQColor(style.style.fill.color));
		ramp.setColorAt(1.0, toQColor(style.style.fill.color2));
		p.fillPath(path, ramp);
	}

	return pixmap;
}

} // namespace

StylesPanel::StylesPanel(QWidget *parent) : QWidget(parent)
{
	setObjectName("stylesPanel");
	setAttribute(Qt::WA_StyledBackground);

	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(0);

	// ---- header ----
	auto *header = new QWidget(this);
	auto *headerLayout = new QHBoxLayout(header);
	headerLayout->setContentsMargins(14, 10, 8, 10);

	auto *titles = new QVBoxLayout();
	titles->setSpacing(0);

	auto *title = new QLabel(tr_("Styles.Title"), header);
	title->setObjectName("panelTitle");

	auto *lead = new QLabel(tr_("Styles.Lead"), header);
	lead->setObjectName("muted");
	lead->setWordWrap(true);

	titles->addWidget(title);
	titles->addWidget(lead);

	auto *close = new QPushButton(header);
	close->setObjectName("flatIcon");
	close->setIcon(ui::icon(ui::Glyph::Close));
	close->setToolTip(tr_("Editor.Close"));
	close->setAccessibleName(tr_("Editor.Close"));

	connect(close, &QPushButton::clicked, this, &StylesPanel::closeRequested);

	headerLayout->addLayout(titles, 1);
	headerLayout->addWidget(close, 0, Qt::AlignTop);

	outer->addWidget(header);

	// ---- save ----
	auto *save = new QWidget(this);
	save->setObjectName("section");
	save->setAttribute(Qt::WA_StyledBackground);

	auto *saveLayout = new QVBoxLayout(save);
	saveLayout->setContentsMargins(14, 4, 14, 14);
	saveLayout->setSpacing(8);

	auto *saveTitle = new QLabel(tr_("Styles.Save.Title"), save);
	saveTitle->setStyleSheet("font-weight: 600;");

	_name = new QLineEdit(save);
	_name->setPlaceholderText(tr_("Styles.Save.Name"));
	_name->setAccessibleName(tr_("Styles.Save.Name"));

	_withMotion = new QCheckBox(tr_("Styles.Save.Motion"), save);
	_withMotion->setChecked(true);

	auto *button = new QPushButton(tr_("Styles.Save.Button"), save);
	button->setObjectName("primary");

	auto commit = [this]() {
		const QString name = _name->text().trimmed();

		if (name.isEmpty()) {
			_name->setFocus();
			return;
		}

		emit saveRequested(name, _withMotion->isChecked());

		_name->clear();
	};

	connect(button, &QPushButton::clicked, this, commit);
	connect(_name, &QLineEdit::returnPressed, this, commit);

	saveLayout->addWidget(saveTitle);
	saveLayout->addWidget(_name);
	saveLayout->addWidget(_withMotion);
	saveLayout->addWidget(button);

	outer->addWidget(save);

	// ---- apply ----
	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	auto *content = new QWidget(scroll);
	content->setObjectName("panelContent");

	_content = content;

	auto *contentLayout = new QVBoxLayout(content);
	contentLayout->setContentsMargins(14, 12, 14, 12);
	contentLayout->setSpacing(8);

	auto *applyTitle = new QLabel(tr_("Styles.Apply.Title"), content);
	applyTitle->setStyleSheet("font-weight: 600;");

	_scope = ui::caption(QString(), content);
	_empty = ui::caption(tr_("Styles.Empty"), content);

	_list = new QVBoxLayout();
	_list->setSpacing(8);

	contentLayout->addWidget(applyTitle);
	contentLayout->addWidget(_scope);
	contentLayout->addLayout(_list);
	contentLayout->addWidget(_empty);
	contentLayout->addStretch(1);

	scroll->setWidget(content);
	outer->addWidget(scroll, 1);

	auto *footer = ui::caption(tr_("Styles.Footer"), this);
	footer->setContentsMargins(14, 8, 14, 10);

	outer->addWidget(footer);

	setSelectionMode(false);
	refresh();
}

void StylesPanel::setSelectionMode(bool selection)
{
	_scope->setText(tr_(selection ? "Styles.Apply.Selection" : "Styles.Apply.All"));
}

QWidget *StylesPanel::styleRow(const SavedStyle &style)
{
	const QString name = QString::fromStdString(style.name);
	const bool hadMotion = style.hasMotion;

	auto *button = new QPushButton(_content);
	button->setObjectName("styleRow");
	button->setCursor(Qt::PointingHandCursor);
	button->setAccessibleName(tr_("Styles.Apply.Name").arg(name));
	button->setAccessibleDescription(describe(style));
	button->setContextMenuPolicy(Qt::CustomContextMenu);

	auto *layout = new QHBoxLayout(button);
	layout->setContentsMargins(7, 7, 10, 7);
	layout->setSpacing(10);

	auto *picture = new QLabel(button);
	picture->setPixmap(sample(style, QSize(84, 44), devicePixelRatioF()));
	picture->setFixedSize(84, 44);
	picture->setAttribute(Qt::WA_TransparentForMouseEvents);

	auto *text = new QVBoxLayout();
	text->setSpacing(0);

	auto *title = new QLabel(name, button);
	title->setStyleSheet("font-weight: 600;");
	title->setAttribute(Qt::WA_TransparentForMouseEvents);

	auto *about = new QLabel(describe(style), button);
	about->setObjectName("caption");
	about->setAttribute(Qt::WA_TransparentForMouseEvents);
	about->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

	text->addStretch(1);
	text->addWidget(title);
	text->addWidget(about);
	text->addStretch(1);

	layout->addWidget(picture);
	layout->addLayout(text, 1);

	connect(button, &QPushButton::clicked, this, [this, name]() { emit applyRequested(name); });

	connect(button, &QWidget::customContextMenuRequested, this, [this, button, name, hadMotion](const QPoint &at) {
		QMenu menu(button);

		QAction *update = menu.addAction(tr_("Styles.Menu.Update"));
		QAction *rename = menu.addAction(tr_("Styles.Menu.Rename"));
		QAction *remove = menu.addAction(tr_("Styles.Menu.Delete"));

		QAction *picked = menu.exec(button->mapToGlobal(at));

		if (picked == update) {
			emit saveRequested(name, hadMotion);
		} else if (picked == rename) {
			bool ok = false;
			const QString next = QInputDialog::getText(this, tr_("Styles.Menu.Rename"),
								   tr_("Styles.Save.Name"), QLineEdit::Normal, name,
								   &ok)
						     .trimmed();

			if (ok && !next.isEmpty() && next != name)
				styles::rename(name.toStdString(), next.toStdString());

			refresh();
		} else if (picked == remove) {
			styles::remove(name.toStdString());
			refresh();
		}
	});

	return button;
}

void StylesPanel::refresh()
{
	while (QLayoutItem *item = _list->takeAt(0)) {
		if (QWidget *widget = item->widget()) {
			// Out of sight at once; deletion waits for the event loop.
			widget->hide();
			widget->deleteLater();
		}

		delete item;
	}

	const std::vector<SavedStyle> saved = styles::all();

	for (const SavedStyle &style : saved)
		_list->addWidget(styleRow(style));

	_empty->setVisible(saved.empty());

	if (!saved.empty())
		_list->addWidget(ui::caption(tr_("Styles.Menu.Hint"), _content));
}

} // namespace slugged
