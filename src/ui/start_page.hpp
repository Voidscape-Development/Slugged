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

#include <QString>
#include <QWidget>

class QPushButton;

namespace slugged {

// "What are you making?" -- the first thing a new source's editor shows.
//
// Nine cards, each a picture of the finished thing and one sentence about it.
// Choosing one sets up the text, the look and the motion; all of it can be
// changed afterwards, so the choice costs nothing.
class StartPage : public QWidget {
	Q_OBJECT

public:
	explicit StartPage(QWidget *parent = nullptr);

	// Whether there is something to go back to. A brand-new source has
	// nothing, so its page has no way out but a choice.
	void setCancellable(bool cancellable);

signals:
	void chosen(const QString &id);
	void convertRequested();
	void cancelled();

private:
	QPushButton *_cancel = nullptr;
};

} // namespace slugged
