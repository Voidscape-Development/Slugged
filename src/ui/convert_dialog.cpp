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

#include "../obs/editor_bridge.hpp"
#include "../obs/migrate.hpp"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <obs-frontend-api.h>
#include <obs-module.h>

namespace slugged {

namespace {

QString tr_(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

// Lists every GDI+ and FreeType text source and replaces the chosen ones with
// Slugged sources in place: same name, same position and transform in every
// scene, same filters. Converting an overlay is then one dialog rather than
// rebuilding each text element by hand.
void showConvertDialog()
{
	auto *parent = static_cast<QWidget *>(obs_frontend_get_main_window());

	const std::vector<std::string> names = migrate::legacySourceNames();

	if (names.empty()) {
		QMessageBox::information(parent, tr_("Convert.Title"), tr_("Convert.None"));
		return;
	}

	QDialog dialog(parent);
	dialog.setWindowTitle(tr_("Convert.Title"));

	auto *layout = new QVBoxLayout(&dialog);

	auto *intro = new QLabel(tr_("Convert.Intro"), &dialog);
	intro->setWordWrap(true);

	auto *list = new QListWidget(&dialog);

	for (const std::string &name : names) {
		auto *item = new QListWidgetItem(QString::fromStdString(name), list);

		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		item->setCheckState(Qt::Checked);
	}

	auto *deleteOriginals = new QCheckBox(tr_("Convert.DeleteOriginals"), &dialog);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	buttons->button(QDialogButtonBox::Ok)->setText(tr_("Convert.Apply"));

	QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

	layout->addWidget(intro);
	layout->addWidget(list, 1);
	layout->addWidget(deleteOriginals);
	layout->addWidget(buttons);

	if (dialog.exec() != QDialog::Accepted)
		return;

	migrate::ConvertOptions options;
	options.deleteOriginal = deleteOriginals->isChecked();

	int converted = 0;

	for (int i = 0; i < list->count(); i++) {
		const QListWidgetItem *item = list->item(i);

		if (item->checkState() != Qt::Checked)
			continue;

		obs_source_t *legacy = obs_get_source_by_name(item->text().toUtf8().constData());

		if (!legacy)
			continue;

		if (obs_source_t *replacement = migrate::convert(legacy, options)) {
			converted++;
			obs_source_release(replacement);
		}

		obs_source_release(legacy);
	}

	QMessageBox::information(parent, tr_("Convert.Title"), tr_("Convert.Done").arg(converted));
}

} // namespace

namespace editor {

void registerTools()
{
	obs_frontend_add_tools_menu_item(obs_module_text("Convert.Menu"), [](void *) { showConvertDialog(); }, nullptr);
}

} // namespace editor

} // namespace slugged
