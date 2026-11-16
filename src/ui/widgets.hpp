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

#include <QAbstractButton>
#include <QColor>
#include <QFrame>
#include <QIcon>
#include <QString>
#include <QWidget>

class QButtonGroup;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QVBoxLayout;

// The small pieces the editor's panels are built from, and the look they share.

namespace slugged {
namespace ui {

// The editor's palette. One accent, used for "this is on" and "do this next".
namespace color {
inline const QColor ground{0x15, 0x16, 0x1a};
inline const QColor panel{0x1d, 0x1f, 0x25};
inline const QColor field{0x28, 0x2b, 0x33};
inline const QColor border{0x35, 0x39, 0x45};
inline const QColor strongBorder{0x55, 0x5b, 0x6b};
inline const QColor text{0xec, 0xed, 0xf1};
inline const QColor muted{0xa3, 0xa8, 0xb5};
inline const QColor accent{0xf5, 0xc8, 0x4b};
inline const QColor onAccent{0x1a, 0x15, 0x00};
} // namespace color

// The style sheet for the whole editor window.
QString styleSheet();

enum class Glyph {
	Undo,
	Redo,
	Plus,
	Close,
	Play,
	Pause,
	Reset,
	ChevronDown,
	ChevronRight,
	Search,
	AlignStart,
	AlignCenter,
	AlignEnd,
	AlignJustify,
	Remove,
};

// Line icons drawn in code, so they follow the palette and need no image files.
QIcon icon(Glyph glyph, const QColor &tint = color::text);

// An on/off switch, for settings that reveal more settings when on.
class Switch : public QAbstractButton {
	Q_OBJECT

public:
	explicit Switch(QWidget *parent = nullptr);

	QSize sizeHint() const override;

protected:
	void paintEvent(QPaintEvent *event) override;
};

// One of a few exclusive choices, shown side by side.
class Segmented : public QFrame {
	Q_OBJECT

public:
	explicit Segmented(QWidget *parent = nullptr);

	void addOption(const QString &label, int value);
	void addOption(const QIcon &glyph, const QString &tooltip, int value);

	int value() const;

	// Does not emit chosen().
	void setValue(int value);

signals:
	void chosen(int value);

private:
	void add(QPushButton *button, int value);

	QHBoxLayout *_layout = nullptr;
	QButtonGroup *_group = nullptr;
};

// A titled part of the side panel that folds away. Folded, it shows a one-line
// summary of what is inside, so the panel reads as a description of the text
// even with everything closed.
class Section : public QWidget {
	Q_OBJECT

public:
	explicit Section(const QString &title, QWidget *parent = nullptr);

	// Where the section's controls go.
	QVBoxLayout *body() const { return _bodyLayout; }
	QWidget *bodyWidget() const { return _body; }

	void setSummary(const QString &summary);

	bool isExpanded() const { return _expanded; }
	void setExpanded(bool expanded);

signals:
	void toggled(bool expanded);

private:
	QPushButton *_header = nullptr;
	QLabel *_summary = nullptr;
	QWidget *_body = nullptr;
	QVBoxLayout *_bodyLayout = nullptr;
	bool _expanded = true;
};

// A label and a control on one line, the label on the left.
QWidget *row(const QString &label, QWidget *control, QWidget *parent);

// A small grey line of explanation.
QLabel *caption(const QString &text, QWidget *parent);

// An icon-only button with a tooltip that doubles as its accessible name.
QPushButton *iconButton(Glyph glyph, const QString &tooltip, QWidget *parent);

} // namespace ui
} // namespace slugged
