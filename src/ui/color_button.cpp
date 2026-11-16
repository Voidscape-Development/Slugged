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

#include "color_button.hpp"

#include <QColorDialog>
#include <QPainter>
#include <QPixmap>

namespace slugged {

ColorButton::ColorButton(QWidget *parent) : QPushButton(parent)
{
	setObjectName("colorButton");
	setIconSize(QSize(20, 20));

	connect(this, &QPushButton::clicked, this, &ColorButton::pick);

	refresh();
}

void ColorButton::setCaption(const QString &caption)
{
	_caption = caption;

	refresh();
}

void ColorButton::setColor(const Rgba &color)
{
	_color = color;

	refresh();
}

void ColorButton::refresh()
{
	// The swatch is the icon, so the button itself keeps the editor's look.
	const qreal ratio = devicePixelRatioF();

	QPixmap swatch(QSize(20, 20) * ratio);
	swatch.setDevicePixelRatio(ratio);
	swatch.fill(Qt::transparent);

	QColor fill;
	fill.setRgbF(qreal(_color.r), qreal(_color.g), qreal(_color.b), 1.0);

	QPainter painter(&swatch);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(QPen(QColor(0x55, 0x5b, 0x6b), 1.0));
	painter.setBrush(fill);
	painter.drawRoundedRect(QRectF(0.5, 0.5, 19.0, 19.0), 5.0, 5.0);
	painter.end();

	setIcon(QIcon(swatch));

	const int opacity = int(_color.a * 100.0f + 0.5f);

	if (_caption.isEmpty())
		setText(QString("%1%").arg(opacity));
	else if (opacity < 100)
		setText(QString("%1 %2%").arg(_caption).arg(opacity));
	else
		setText(_caption);
}

void ColorButton::pick()
{
	QColor initial;

	initial.setRgbF(qreal(_color.r), qreal(_color.g), qreal(_color.b), qreal(_color.a));

	const QColor chosen =
		QColorDialog::getColor(initial, this, tr("Select colour"), QColorDialog::ShowAlphaChannel);

	if (!chosen.isValid())
		return;

	_color.r = float(chosen.redF());
	_color.g = float(chosen.greenF());
	_color.b = float(chosen.blueF());
	_color.a = float(chosen.alphaF());

	refresh();

	emit colorChanged();
}

} // namespace slugged
