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

#include "../core/types.hpp"

#include <QPushButton>

namespace slugged {

// A button that shows its colour and opens a colour picker, with alpha.
class ColorButton : public QPushButton {
	Q_OBJECT

public:
	explicit ColorButton(QWidget *parent = nullptr);

	void setColor(const Rgba &color);
	Rgba color() const { return _color; }

signals:
	void colorChanged();

private:
	void refresh();
	void pick();

	Rgba _color;
};

} // namespace slugged
