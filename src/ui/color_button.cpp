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

namespace slugged {

ColorButton::ColorButton(QWidget *parent) : QPushButton(parent)
{
	setAutoFillBackground(true);
	setFlat(true);
	setMinimumWidth(72);

	connect(this, &QPushButton::clicked, this, &ColorButton::pick);

	refresh();
}

void ColorButton::setColor(const Rgba &color)
{
	_color = color;

	refresh();
}

void ColorButton::refresh()
{
	const int r = int(_color.r * 255.0f + 0.5f);
	const int g = int(_color.g * 255.0f + 0.5f);
	const int b = int(_color.b * 255.0f + 0.5f);

	// Pick a readable label colour from the swatch's own luminance.
	const float luma = 0.2126f * _color.r + 0.7152f * _color.g + 0.0722f * _color.b;
	const char *text = luma > 0.55f ? "#000000" : "#FFFFFF";

	setStyleSheet(QString("QPushButton { background-color: rgb(%1,%2,%3); color: %4; "
			      "border: 1px solid palette(mid); padding: 4px; }")
			      .arg(r)
			      .arg(g)
			      .arg(b)
			      .arg(text));

	setText(QString("%1%").arg(int(_color.a * 100.0f + 0.5f)));
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
