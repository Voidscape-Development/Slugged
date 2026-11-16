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

#include "../core/variables.hpp"

#include <QString>
#include <QWidget>

#include <vector>

#include <obs.h>

class QLabel;
class QLineEdit;
class QVBoxLayout;

namespace slugged {

// "Insert a live value": every token the text can hold, as a list of what each
// one says right now rather than as a paragraph of braces to memorise.
//
// It is also where a source's own values are kept: the ones a bot or script
// fills in, both for this source alone and shared by every Slugged source.
class InsertPanel : public QWidget {
	Q_OBJECT

public:
	explicit InsertPanel(obs_source_t *source, QWidget *parent = nullptr);

	// Shows `values` as this source's own; the shared ones are read afresh.
	void setSourceVariables(const VariableMap &values);

	void focusSearch();

	// Stops reading the source. Safe to call more than once.
	void detach() { _source = nullptr; }

signals:
	// `token` is ready to be put into the text as it is.
	void insertRequested(const QString &token);
	void closeRequested();

	// This source's values were edited here.
	void sourceVariablesChanged(const slugged::VariableMap &values);

protected:
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	void timerEvent(QTimerEvent *event) override;

private:
	struct Entry {
		QWidget *row = nullptr;
		QWidget *editor = nullptr;
		QLabel *value = nullptr;
		QLineEdit *argument = nullptr;
		QLabel *group = nullptr;
		QString label;
		QString pattern;
	};

	void addGroup(const QString &title);
	void addBuiltin(const char *labelKey, const char *pattern, const char *argumentLabelKey = nullptr,
			const char *argumentDefault = nullptr);
	void rebuildOwnValues();
	void addOwnGroup(const QString &title, const VariableMap &values, bool shared);
	void refreshValues();
	void applyFilter(const QString &query);
	QString tokenOf(const Entry &entry) const;

	obs_source_t *_source = nullptr;

	QLineEdit *_search = nullptr;
	QVBoxLayout *_list = nullptr;
	QWidget *_ownHolder = nullptr;
	QVBoxLayout *_ownLayout = nullptr;
	QLabel *_currentGroup = nullptr;

	std::vector<Entry> _entries;
	VariableMap _sourceVariables;

	int _timer = 0;
};

} // namespace slugged
