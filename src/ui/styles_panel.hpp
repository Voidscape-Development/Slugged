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

class QCheckBox;
class QLabel;
class QLineEdit;
class QVBoxLayout;

namespace slugged {

struct SavedStyle;

// "Styles": looks saved once and applied to any Slugged source.
//
// The panel only lists, names and asks; the editor owns the document, so
// saving and applying are requests it answers.
class StylesPanel : public QWidget {
	Q_OBJECT

public:
	explicit StylesPanel(QWidget *parent = nullptr);

	// Re-reads the saved styles.
	void refresh();

	// Whether applying would style a selection rather than the whole text.
	void setSelectionMode(bool selection);

signals:
	void closeRequested();

	// Save the current look under `name`, replacing a style of that name.
	void saveRequested(const QString &name, bool withMotion);
	void applyRequested(const QString &name);

private:
	QWidget *styleRow(const SavedStyle &style);

	QLineEdit *_name = nullptr;
	QCheckBox *_withMotion = nullptr;
	QLabel *_scope = nullptr;
	QLabel *_empty = nullptr;
	QWidget *_content = nullptr;
	QVBoxLayout *_list = nullptr;
};

} // namespace slugged
