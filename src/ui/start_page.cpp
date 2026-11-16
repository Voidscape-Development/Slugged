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

#include "start_page.hpp"
#include "widgets.hpp"
#include "../obs/start_points.hpp"

#include <cmath>

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <obs-module.h>

namespace slugged {

namespace {

QString tr_(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QFont sized(const QFont &base, int pixels, int weight = QFont::Bold)
{
	QFont font = base;

	font.setPixelSize(pixels);
	font.setWeight(QFont::Weight(weight));

	return font;
}

// A small picture of what each starting point makes. These are drawings, not
// renders: they only have to be recognisable at a glance.
QPixmap preview(const QString &id, const QFont &base, QSize size, qreal ratio)
{
	QPixmap pixmap(size * ratio);
	pixmap.setDevicePixelRatio(ratio);
	pixmap.fill(QColor(0x1a, 0x1b, 0x20));

	QPainter p(&pixmap);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::TextAntialiasing);

	const QRectF area(QPointF(0, 0), QSizeF(size));
	const QColor light = ui::color::text;
	const QColor accent = ui::color::accent;
	const QColor muted = ui::color::muted;

	auto centred = [&](const QString &text, const QFont &font, const QColor &colour, qreal dy = 0.0) {
		p.setFont(font);
		p.setPen(colour);
		p.drawText(area.translated(0, dy), Qt::AlignCenter, text);
	};

	if (id == "plain") {
		p.setFont(sized(base, 20, QFont::Medium));

		const QString text = tr_("Start.Plain.Sample");
		const qreal width = p.fontMetrics().horizontalAdvance(text);
		const qreal x = (area.width() - width) / 2.0;

		p.fillRect(QRectF(x - 6, area.center().y() - 14, 2, 28), light);
		p.setPen(muted);
		p.drawText(QRectF(x, 0, width + 4, area.height()), Qt::AlignVCenter | Qt::AlignLeft, text);
	} else if (id == "title") {
		const QString first = tr_("Start.Title.Line1");
		const QString second = tr_("Start.Title.Line2");

		const QFont big = sized(base, 18);
		const QFont small = sized(base, 11, QFont::Normal);
		const qreal width = std::max(QFontMetricsF(big).horizontalAdvance(first),
					     QFontMetricsF(small).horizontalAdvance(second)) +
				    28;
		const QRectF plate((area.width() - width) / 2.0, area.center().y() - 26, width, 52);

		p.setPen(Qt::NoPen);
		p.setBrush(light);
		p.drawRoundedRect(plate, 7, 7);

		p.setPen(ui::color::ground);
		p.setFont(big);
		p.drawText(plate.adjusted(14, 7, -14, -22), Qt::AlignLeft | Qt::AlignVCenter, first);
		p.setFont(small);
		p.drawText(plate.adjusted(14, 30, -14, -6), Qt::AlignLeft | Qt::AlignVCenter, second);
	} else if (id == "clock") {
		centred("14:07", sized(base, 40), light);
	} else if (id == "countdown") {
		QFont lead = sized(base, 10, QFont::DemiBold);
		lead.setLetterSpacing(QFont::AbsoluteSpacing, 2.0);

		centred(tr_("Start.Countdown.Line1"), lead, accent, -26);
		centred("05:00", sized(base, 38), light, 8);
	} else if (id == "timer") {
		centred("00:48:10", sized(base, 32), light);
	} else if (id == "ticker") {
		const QRectF bar(0, area.center().y() - 16, area.width(), 32);

		p.fillRect(bar, light);
		p.setClipRect(bar);
		p.setPen(ui::color::ground);
		p.setFont(sized(base, 14, QFont::DemiBold));

		const QString text = tr_("Start.Ticker.Line1");

		p.drawText(bar.adjusted(-40, 0, 400, 0), Qt::AlignVCenter | Qt::AlignLeft,
			   text + QStringLiteral("   \u00b7   ") + text);
		p.setClipping(false);
	} else if (id == "credits") {
		centred(tr_("Start.Credits.Line1"), sized(base, 11, QFont::Normal), muted, -30);
		centred(tr_("Start.Credits.Line2"), sized(base, 16), accent, -6);
		centred(tr_("Start.Credits.Line3"), sized(base, 12, QFont::Normal), light, 16);
		centred(tr_("Start.Credits.Line4"), sized(base, 12, QFont::Normal), muted, 34);
	} else if (id == "bot") {
		const QFont font = sized(base, 18);
		const QString lead = tr_("Start.Bot.Line1") + " ";
		const QString value = tr_("Start.Bot.Sample");
		const QFontMetricsF metrics(font);
		const qreal leadWidth = metrics.horizontalAdvance(lead);
		const qreal valueWidth = metrics.horizontalAdvance(value);
		const qreal x = (area.width() - leadWidth - valueWidth) / 2.0;

		p.setFont(font);
		p.setPen(light);
		p.drawText(QRectF(x, 0, leadWidth + 2, area.height()), Qt::AlignVCenter | Qt::AlignLeft, lead);
		p.setPen(accent);
		p.drawText(QRectF(x + leadWidth, 0, valueWidth + 2, area.height()), Qt::AlignVCenter | Qt::AlignLeft,
			   value);

		p.setPen(QPen(accent, 1.5, Qt::DotLine));
		p.drawLine(QPointF(x + leadWidth, area.center().y() + 14),
			   QPointF(x + leadWidth + valueWidth, area.center().y() + 14));
	} else if (id == "file") {
		QFont mono = sized(base, 12, QFont::Normal);
		mono.setFamilies({"ui-monospace", "Menlo", "Consolas", "DejaVu Sans Mono", "monospace"});
		mono.setStyleHint(QFont::Monospace);

		const QString name = QStringLiteral("now-playing.txt");
		const qreal width = QFontMetricsF(mono).horizontalAdvance(name) + 20;
		const QRectF chip((area.width() - width) / 2.0, area.center().y() - 30, width, 24);

		p.setPen(QPen(ui::color::strongBorder, 1.0));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(chip, 5, 5);
		p.setPen(light);
		p.setFont(mono);
		p.drawText(chip, Qt::AlignCenter, name);

		centred(tr_("Start.File.Sample"), sized(base, 15), light, 16);
	} else if (id == "counter") {
		const QFont font = sized(base, 24);
		const QString lead = tr_("Start.Counter.Line1") + " ";
		const QString value = QStringLiteral("17");
		const QFontMetricsF metrics(font);
		const qreal leadWidth = metrics.horizontalAdvance(lead);
		const qreal x = (area.width() - leadWidth - metrics.horizontalAdvance(value)) / 2.0;

		p.setFont(font);
		p.setPen(light);
		p.drawText(QRectF(x, 0, leadWidth + 2, area.height()), Qt::AlignVCenter | Qt::AlignLeft, lead);
		p.setPen(accent);
		p.drawText(QRectF(x + leadWidth, 0, 80, area.height()), Qt::AlignVCenter | Qt::AlignLeft, value);

		// The hotkeys that step it.
		const QFont keys = sized(base, 10, QFont::DemiBold);
		p.setFont(keys);

		const QStringList labels = {QStringLiteral("−"), QStringLiteral("+")};

		for (int i = 0; i < 2; i++) {
			const QRectF key(area.width() - 58 + i * 26, area.height() - 26, 20, 18);

			p.setPen(QPen(ui::color::strongBorder, 1.0));
			p.setBrush(Qt::NoBrush);
			p.drawRoundedRect(key, 4, 4);
			p.setPen(muted);
			p.drawText(key, Qt::AlignCenter, labels[i]);
		}
	} else if (id == "messages") {
		const QRectF plate(area.width() * 0.12, area.center().y() - 20, area.width() * 0.76, 40);

		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0x2a, 0x2c, 0x34));
		p.drawRoundedRect(plate, 8, 8);

		p.setFont(sized(base, 15, QFont::DemiBold));
		p.setPen(light);
		p.drawText(plate, Qt::AlignCenter, tr_("Start.Messages.Line1"));

		// Dots for the lines taking turns.
		p.setPen(Qt::NoPen);

		for (int i = 0; i < 3; i++) {
			p.setBrush(i == 0 ? accent : muted);
			p.drawEllipse(QPointF(area.center().x() - 12 + i * 12, plate.bottom() + 14), 3, 3);
		}
	} else if (id == "neon") {
		const QFont font = sized(base, 34, QFont::Black);
		const QColor pink(0xff, 0x2e, 0x9e);

		p.setFont(font);

		// A cheap glow: the word drawn faintly at offsets around itself.
		for (int ring = 3; ring >= 1; ring--) {
			QColor halo = pink;
			halo.setAlphaF(0.12 * (4 - ring));
			p.setPen(halo);

			for (int a = 0; a < 12; a++) {
				const qreal angle = a * 3.14159265 / 6.0;
				p.drawText(area.translated(std::cos(angle) * ring * 2.0, std::sin(angle) * ring * 2.0),
					   Qt::AlignCenter, tr_("Start.Neon.Line1"));
			}
		}

		p.setPen(QColor(0xff, 0xee, 0xfa));
		p.drawText(area, Qt::AlignCenter, tr_("Start.Neon.Line1"));
	}

	return pixmap;
}

} // namespace

StartPage::StartPage(QWidget *parent) : QWidget(parent)
{
	setObjectName("startPage");
	setAttribute(Qt::WA_StyledBackground);

	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);

	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);

	auto *page = new QWidget(scroll);
	page->setObjectName("startPage");

	auto *centre = new QHBoxLayout(page);
	centre->setContentsMargins(24, 28, 24, 24);

	auto *column = new QWidget(page);
	column->setMaximumWidth(1000);

	auto *layout = new QVBoxLayout(column);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(18);

	auto *headRow = new QHBoxLayout();
	auto *headText = new QVBoxLayout();
	headText->setSpacing(4);

	auto *title = new QLabel(tr_("Start.Title.Heading"), column);
	title->setObjectName("pageTitle");

	auto *lead = new QLabel(tr_("Start.Lead"), column);
	lead->setObjectName("muted");
	lead->setWordWrap(true);

	headText->addWidget(title);
	headText->addWidget(lead);

	_cancel = new QPushButton(tr_("Start.Cancel"), column);
	_cancel->setObjectName("ghost");
	_cancel->hide();

	connect(_cancel, &QPushButton::clicked, this, &StartPage::cancelled);

	headRow->addLayout(headText, 1);
	headRow->addWidget(_cancel, 0, Qt::AlignTop);

	layout->addLayout(headRow);

	auto *grid = new QGridLayout();
	grid->setSpacing(14);

	const QSize previewSize(300, 100);
	int index = 0;

	for (const StartPoint &point : startPoints()) {
		const QString id = QString::fromUtf8(point.id);

		auto *card = new QPushButton(column);
		card->setObjectName("card");
		card->setCursor(Qt::PointingHandCursor);
		card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		card->setAccessibleName(tr_(point.name));
		card->setAccessibleDescription(tr_(point.description));

		auto *cardLayout = new QVBoxLayout(card);
		cardLayout->setContentsMargins(1, 1, 1, 12);
		cardLayout->setSpacing(4);

		auto *picture = new QLabel(card);
		picture->setPixmap(preview(id, font(), previewSize, devicePixelRatioF()));
		picture->setScaledContents(true);
		picture->setFixedHeight(previewSize.height());
		picture->setAttribute(Qt::WA_TransparentForMouseEvents);

		auto *name = new QLabel(tr_(point.name), card);
		name->setStyleSheet("font-weight: 600; font-size: 14px; padding: 6px 12px 0 12px;");
		name->setAttribute(Qt::WA_TransparentForMouseEvents);

		auto *about = new QLabel(tr_(point.description), card);
		about->setObjectName("caption");
		about->setWordWrap(true);
		about->setStyleSheet("padding: 0 12px;");
		about->setMinimumHeight(34);
		about->setAlignment(Qt::AlignTop | Qt::AlignLeft);
		about->setAttribute(Qt::WA_TransparentForMouseEvents);

		cardLayout->addWidget(picture);
		cardLayout->addWidget(name);
		cardLayout->addWidget(about);

		connect(card, &QPushButton::clicked, this, [this, id]() { emit chosen(id); });

		grid->addWidget(card, index / 3, index % 3);
		index++;
	}

	layout->addLayout(grid);

	auto *notice = new QFrame(column);
	notice->setObjectName("notice");

	auto *noticeLayout = new QHBoxLayout(notice);
	noticeLayout->setContentsMargins(14, 10, 10, 10);

	auto *noticeText = new QLabel(tr_("Start.Convert"), notice);
	noticeText->setWordWrap(true);

	auto *convert = new QPushButton(tr_("Start.Convert.Button"), notice);
	convert->setObjectName("ghost");

	connect(convert, &QPushButton::clicked, this, &StartPage::convertRequested);

	noticeLayout->addWidget(noticeText, 1);
	noticeLayout->addWidget(convert);

	layout->addWidget(notice);
	layout->addStretch(1);

	centre->addStretch(1);
	centre->addWidget(column, 100);
	centre->addStretch(1);

	scroll->setWidget(page);
	outer->addWidget(scroll);
}

void StartPage::setCancellable(bool cancellable)
{
	_cancel->setVisible(cancellable);
}

} // namespace slugged
