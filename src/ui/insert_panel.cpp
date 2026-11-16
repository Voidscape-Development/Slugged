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

#include "insert_panel.hpp"
#include "widgets.hpp"
#include "../obs/settings.hpp"
#include "../obs/slugged_source.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTimerEvent>
#include <QVBoxLayout>

#include <obs-module.h>

namespace slugged {

namespace {

QString tr_(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

// A list row: the name on the left, what it says now on the right, the whole
// row one button.
QPushButton *listRow(const QString &label, QLabel *&value, QWidget *parent)
{
	auto *button = new QPushButton(parent);
	button->setObjectName("listRow");
	button->setCursor(Qt::PointingHandCursor);
	button->setAccessibleName(label);

	auto *layout = new QHBoxLayout(button);
	layout->setContentsMargins(10, 0, 10, 0);

	auto *name = new QLabel(label, button);
	name->setAttribute(Qt::WA_TransparentForMouseEvents);

	value = new QLabel(button);
	value->setObjectName("muted");
	value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	value->setAttribute(Qt::WA_TransparentForMouseEvents);
	value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

	layout->addWidget(name);
	layout->addWidget(value, 1);

	return button;
}

} // namespace

InsertPanel::InsertPanel(obs_source_t *source, QWidget *parent) : QWidget(parent), _source(source)
{
	setObjectName("insertPanel");
	setAttribute(Qt::WA_StyledBackground);

	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(0);

	// ---- header ----
	auto *header = new QWidget(this);
	auto *headerLayout = new QVBoxLayout(header);
	headerLayout->setContentsMargins(14, 10, 8, 12);
	headerLayout->setSpacing(8);

	auto *titleRow = new QHBoxLayout();
	auto *titles = new QVBoxLayout();
	titles->setSpacing(0);

	auto *title = new QLabel(tr_("Insert.Title"), header);
	title->setObjectName("panelTitle");

	auto *lead = new QLabel(tr_("Insert.Lead"), header);
	lead->setObjectName("muted");
	lead->setWordWrap(true);

	titles->addWidget(title);
	titles->addWidget(lead);

	auto *close = new QPushButton(header);
	close->setObjectName("flatIcon");
	close->setIcon(ui::icon(ui::Glyph::Close));
	close->setToolTip(tr_("Editor.Close"));
	close->setAccessibleName(tr_("Editor.Close"));

	connect(close, &QPushButton::clicked, this, &InsertPanel::closeRequested);

	titleRow->addLayout(titles, 1);
	titleRow->addWidget(close, 0, Qt::AlignTop);

	_search = new QLineEdit(header);
	_search->setPlaceholderText(tr_("Insert.Search"));
	_search->setAccessibleName(tr_("Insert.Search.Name"));
	_search->setClearButtonEnabled(true);
	_search->addAction(ui::icon(ui::Glyph::Search, ui::color::muted), QLineEdit::LeadingPosition);

	connect(_search, &QLineEdit::textChanged, this, &InsertPanel::applyFilter);

	headerLayout->addLayout(titleRow);
	headerLayout->addWidget(_search);

	outer->addWidget(header);

	// ---- list ----
	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	auto *content = new QWidget(scroll);
	content->setObjectName("panelContent");

	_list = new QVBoxLayout(content);
	_list->setContentsMargins(6, 0, 6, 10);
	_list->setSpacing(2);

	addGroup(tr_("Insert.Group.Timers"));
	addBuiltin("Insert.Countdown", "{countdown:%1}", "Insert.Countdown.Arg", "5:00");
	addBuiltin("Insert.Stopwatch", "{timer}");
	addBuiltin("Insert.Until", "{until:%1}", "Insert.Until.Arg", "20:00");

	addGroup(tr_("Insert.Group.Time"));
	addBuiltin("Insert.Time", "{time}");
	addBuiltin("Insert.Time12", "{time12}");
	addBuiltin("Insert.Seconds", "{seconds}");
	addBuiltin("Insert.Date", "{date}");
	addBuiltin("Insert.DateLong", "{date_long}");
	addBuiltin("Insert.Weekday", "{weekday}");
	addBuiltin("Insert.Month", "{month}");
	addBuiltin("Insert.Year", "{year}");
	addBuiltin("Insert.Strftime", "{strftime:%1}", "Insert.Strftime.Arg", "%H:%M");

	addGroup(tr_("Insert.Group.Obs"));
	addBuiltin("Insert.StreamTime", "{stream_time}");
	addBuiltin("Insert.RecordTime", "{record_time}");
	addBuiltin("Insert.Scene", "{scene}");
	addBuiltin("Insert.PreviewScene", "{preview_scene}");
	addBuiltin("Insert.SourceName", "{source}");
	addBuiltin("Insert.Uptime", "{uptime}");
	addBuiltin("Insert.Fps", "{fps}");
	addBuiltin("Insert.Dropped", "{dropped_frames}");
	addBuiltin("Insert.DroppedPercent", "{dropped_percent}");
	addBuiltin("Insert.Cpu", "{cpu}");
	addBuiltin("Insert.Bitrate", "{bitrate}");

	_ownHolder = new QWidget(content);
	_ownLayout = new QVBoxLayout(_ownHolder);
	_ownLayout->setContentsMargins(0, 0, 0, 0);
	_ownLayout->setSpacing(2);

	_list->addWidget(_ownHolder);
	_list->addStretch(1);

	scroll->setWidget(content);
	outer->addWidget(scroll, 1);

	// ---- footer ----
	auto *footer = new QWidget(this);
	auto *footerLayout = new QVBoxLayout(footer);
	footerLayout->setContentsMargins(14, 8, 14, 10);
	footerLayout->setSpacing(2);

	auto *brace = new QPushButton(tr_("Insert.Brace"), footer);
	brace->setObjectName("link");
	brace->setCursor(Qt::PointingHandCursor);

	// "{{" is how the text says a real brace.
	connect(brace, &QPushButton::clicked, this, [this]() { emit insertRequested(QStringLiteral("{{")); });

	footerLayout->addWidget(ui::caption(tr_("Insert.Shortcut"), footer));
	footerLayout->addWidget(brace);

	outer->addWidget(footer);

	rebuildOwnValues();
}

void InsertPanel::addGroup(const QString &title)
{
	_currentGroup = new QLabel(title.toUpper(), this);
	_currentGroup->setObjectName("groupLabel");
	_currentGroup->setContentsMargins(10, 0, 0, 0);

	_list->addWidget(_currentGroup);
}

void InsertPanel::addBuiltin(const char *labelKey, const char *pattern, const char *argumentLabelKey,
			     const char *argumentDefault)
{
	Entry entry;

	entry.label = tr_(labelKey);
	entry.pattern = QString::fromUtf8(pattern);
	entry.group = _currentGroup;

	auto *button = listRow(entry.label, entry.value, this);

	entry.row = button;
	_list->addWidget(button);

	const size_t index = _entries.size();

	if (argumentLabelKey) {
		// A value that needs to be told something opens a line for it.
		auto *editor = new QFrame(this);
		editor->setObjectName("expanded");

		auto *layout = new QHBoxLayout(editor);
		layout->setContentsMargins(10, 8, 8, 8);

		auto *label = new QLabel(tr_(argumentLabelKey), editor);
		label->setObjectName("muted");

		entry.argument = new QLineEdit(QString::fromUtf8(argumentDefault), editor);
		label->setBuddy(entry.argument);

		auto *insert = new QPushButton(tr_("Insert.Button"), editor);
		insert->setObjectName("primary");

		layout->addWidget(label);
		layout->addWidget(entry.argument, 1);
		layout->addWidget(insert);

		editor->hide();
		entry.editor = editor;
		_list->addWidget(editor);

		connect(button, &QPushButton::clicked, this, [this, index]() {
			Entry &e = _entries[index];

			e.editor->setVisible(!e.editor->isVisible());

			if (e.editor->isVisible()) {
				e.argument->setFocus();
				e.argument->selectAll();
			}
		});

		auto commit = [this, index]() {
			emit insertRequested(tokenOf(_entries[index]));
		};

		connect(insert, &QPushButton::clicked, this, commit);
		connect(entry.argument, &QLineEdit::returnPressed, this, commit);
		connect(entry.argument, &QLineEdit::textChanged, this, [this]() { refreshValues(); });
	} else {
		connect(button, &QPushButton::clicked, this,
			[this, index]() { emit insertRequested(tokenOf(_entries[index])); });
	}

	_entries.push_back(entry);
}

QString InsertPanel::tokenOf(const Entry &entry) const
{
	if (!entry.argument)
		return entry.pattern;

	// A closing brace would end the token early.
	QString argument = entry.argument->text().trimmed();
	argument.remove('}');

	return entry.pattern.arg(argument);
}

void InsertPanel::setSourceVariables(const VariableMap &values)
{
	if (values == _sourceVariables && _ownLayout->count() > 0)
		return;

	_sourceVariables = values;

	rebuildOwnValues();
}

void InsertPanel::addOwnGroup(const QString &title, const VariableMap &values, bool shared)
{
	auto *group = new QLabel(title.toUpper(), _ownHolder);
	group->setObjectName("groupLabel");
	group->setContentsMargins(10, 0, 0, 0);

	_ownLayout->addWidget(group);

	auto write = [this, shared](const VariableMap &next) {
		if (shared) {
			// Not part of any source: written straight through and saved
			// with the module's own config.
			GlobalVariables::replace(next);
			settings::saveGlobalVariables();
		} else {
			_sourceVariables = next;
			emit sourceVariablesChanged(next);
		}

		// Rebuilding deletes the widget whose signal this is.
		QMetaObject::invokeMethod(this, [this]() { rebuildOwnValues(); }, Qt::QueuedConnection);
	};

	for (const auto &entry : values) {
		// Plain copies: the lambdas below capture them, and a structured
		// binding cannot be captured under OpenMP.
		const std::string name = entry.first;
		const std::string value = entry.second;

		auto *line = new QWidget(_ownHolder);
		auto *layout = new QHBoxLayout(line);
		layout->setContentsMargins(0, 0, 4, 0);
		layout->setSpacing(4);

		const QString qname = QString::fromStdString(name);

		auto *insert = new QPushButton(qname, line);
		insert->setObjectName("listRow");
		insert->setCursor(Qt::PointingHandCursor);
		insert->setToolTip(tr_("Insert.Own.InsertHint").arg(qname));

		auto *edit = new QLineEdit(QString::fromStdString(value), line);
		edit->setAccessibleName(tr_("Insert.Own.ValueOf").arg(qname));

		auto *remove = new QPushButton(line);
		remove->setObjectName("flatIcon");
		remove->setIcon(ui::icon(ui::Glyph::Remove, ui::color::muted));
		remove->setIconSize(QSize(12, 12));
		remove->setToolTip(tr_("Insert.Own.Remove").arg(qname));
		remove->setAccessibleName(tr_("Insert.Own.Remove").arg(qname));

		layout->addWidget(insert, 1);
		layout->addWidget(edit, 1);
		layout->addWidget(remove);

		connect(insert, &QPushButton::clicked, this,
			[this, qname]() { emit insertRequested("{" + qname + "}"); });

		connect(edit, &QLineEdit::editingFinished, this, [values, name, edit, write]() {
			const std::string text = edit->text().toStdString();

			if (values.at(name) == text)
				return;

			VariableMap next = values;
			next[name] = text;
			write(next);
		});

		connect(remove, &QPushButton::clicked, this, [values, name, write]() {
			VariableMap next = values;
			next.erase(name);
			write(next);
		});

		_ownLayout->addWidget(line);
	}

	// ---- a new one ----
	auto *adder = new QWidget(_ownHolder);
	auto *adderLayout = new QHBoxLayout(adder);
	adderLayout->setContentsMargins(10, 2, 4, 0);
	adderLayout->setSpacing(4);

	auto *name = new QLineEdit(adder);
	name->setPlaceholderText(tr_("Insert.Own.NewName"));
	name->setAccessibleName(tr_("Insert.Own.NewName"));

	auto *value = new QLineEdit(adder);
	value->setPlaceholderText(tr_("Insert.Own.NewValue"));
	value->setAccessibleName(tr_("Insert.Own.NewValue"));

	auto *add = new QPushButton(adder);
	add->setObjectName("flatIcon");
	add->setIcon(ui::icon(ui::Glyph::Plus, ui::color::accent));
	add->setToolTip(tr_("Insert.Own.Add"));
	add->setAccessibleName(tr_("Insert.Own.Add"));

	adderLayout->addWidget(name, 1);
	adderLayout->addWidget(value, 1);
	adderLayout->addWidget(add);

	auto create = [values, name, value, write]() {
		const std::string key = normaliseVariableName(name->text().toStdString());

		if (key.empty())
			return;

		VariableMap next = values;
		next[key] = value->text().toStdString();
		write(next);
	};

	connect(add, &QPushButton::clicked, this, create);
	connect(name, &QLineEdit::returnPressed, this, create);
	connect(value, &QLineEdit::returnPressed, this, create);

	_ownLayout->addWidget(adder);
}

void InsertPanel::rebuildOwnValues()
{
	while (QLayoutItem *item = _ownLayout->takeAt(0)) {
		if (QWidget *widget = item->widget()) {
			// Out of sight at once; deletion waits for the event loop.
			widget->hide();
			widget->deleteLater();
		}

		delete item;
	}

	addOwnGroup(tr_("Insert.Group.Source"), _sourceVariables, false);
	_ownLayout->addWidget(ui::caption(tr_("Insert.Group.Source.Hint"), _ownHolder));

	addOwnGroup(tr_("Insert.Group.Shared"), GlobalVariables::snapshot(), true);
	_ownLayout->addWidget(ui::caption(tr_("Insert.Group.Shared.Hint"), _ownHolder));

	for (int i = 0; i < _ownLayout->count(); i++) {
		if (auto *label = qobject_cast<QLabel *>(_ownLayout->itemAt(i)->widget())) {
			if (label->objectName() == "caption")
				label->setContentsMargins(10, 0, 6, 0);
		}
	}
}

void InsertPanel::refreshValues()
{
	if (!_source)
		return;

	for (Entry &entry : _entries) {
		const QString token = tokenOf(entry);
		const QString shown = QString::fromStdString(expandFor(_source, token.toStdString()));

		// A token that expands to itself has no value to show: an argument
		// that does not parse, say.
		entry.value->setText(shown == token ? tr_("Insert.NoValue") : shown);
	}
}

void InsertPanel::applyFilter(const QString &query)
{
	const QString needle = query.trimmed();

	QLabel *group = nullptr;
	bool any = false;

	auto closeGroup = [&]() {
		if (group)
			group->setVisible(any);
	};

	for (Entry &entry : _entries) {
		if (entry.group != group) {
			closeGroup();
			group = entry.group;
			any = false;
		}

		const bool match = needle.isEmpty() || entry.label.contains(needle, Qt::CaseInsensitive) ||
				   entry.pattern.contains(needle, Qt::CaseInsensitive);

		entry.row->setVisible(match);

		if (entry.editor && !match)
			entry.editor->hide();

		any = any || match;
	}

	closeGroup();
}

void InsertPanel::focusSearch()
{
	_search->setFocus();
	_search->selectAll();
}

void InsertPanel::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);

	rebuildOwnValues();
	refreshValues();

	if (!_timer)
		_timer = startTimer(500);
}

void InsertPanel::hideEvent(QHideEvent *event)
{
	QWidget::hideEvent(event);

	if (_timer) {
		killTimer(_timer);
		_timer = 0;
	}
}

void InsertPanel::timerEvent(QTimerEvent *event)
{
	if (event->timerId() == _timer)
		refreshValues();
	else
		QWidget::timerEvent(event);
}

} // namespace slugged
