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

#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QString>

#include <algorithm>
#include <initializer_list>
#include <utility>

#include <obs-module.h>

// Small helpers shared by the editor's translation units.

namespace slugged {
namespace ui {

inline QString tr_(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

// Qt measures text in UTF-16 code units; the document in UTF-8 bytes.
inline size_t utf8Offset(const QString &text, int position)
{
	return size_t(text.left(std::clamp(position, 0, int(text.size()))).toUtf8().size());
}

inline int utf16Position(const QString &text, size_t offset)
{
	const QByteArray bytes = text.toUtf8();

	return int(QString::fromUtf8(bytes.left(qsizetype(std::min(offset, size_t(bytes.size()))))).size());
}

inline QColor toQColor(const Rgba &c)
{
	QColor q;
	q.setRgbF(qreal(c.r), qreal(c.g), qreal(c.b), qreal(std::max(c.a, 0.35f)));
	return q;
}

template<typename E> QComboBox *enumCombo(QWidget *parent, std::initializer_list<std::pair<E, const char *>> items)
{
	auto *combo = new QComboBox(parent);

	for (const auto &[value, key] : items)
		combo->addItem(tr_(key), int(value));

	return combo;
}

template<typename E> E comboValue(const QComboBox *combo)
{
	return E(combo->currentData().toInt());
}

inline void setCombo(QComboBox *combo, int value)
{
	const int index = combo->findData(value);

	if (index >= 0)
		combo->setCurrentIndex(index);
}

inline QDoubleSpinBox *spin(QWidget *parent, double lo, double hi, double step, const char *suffix = nullptr,
			    int decimals = 1)
{
	auto *box = new QDoubleSpinBox(parent);

	box->setRange(lo, hi);
	box->setSingleStep(step);
	box->setDecimals(decimals);
	box->setKeyboardTracking(false);

	if (suffix)
		box->setSuffix(QString::fromUtf8(suffix));

	return box;
}

inline constexpr std::pair<int, const char *> kWeights[] = {
	{100, "Editor.Weight.Thin"},    {200, "Editor.Weight.ExtraLight"}, {300, "Editor.Weight.Light"},
	{400, "Editor.Weight.Regular"}, {500, "Editor.Weight.Medium"},     {600, "Editor.Weight.SemiBold"},
	{700, "Editor.Weight.Bold"},    {800, "Editor.Weight.ExtraBold"},  {900, "Editor.Weight.Black"},
};

inline const char *weightKey(int weight)
{
	const char *best = kWeights[3].second;
	int distance = 10000;

	for (const auto &[value, key] : kWeights) {
		const int d = value > weight ? value - weight : weight - value;

		if (d < distance) {
			distance = d;
			best = key;
		}
	}

	return best;
}

inline const char *motionKey(Motion motion)
{
	switch (motion) {
	case Motion::Fade:
		return "Motion.Fade";
	case Motion::Slide:
		return "Motion.Slide";
	case Motion::Pop:
		return "Motion.Pop";
	case Motion::Typewriter:
		return "Motion.Typewriter";
	case Motion::Wave:
		return "Motion.Wave";
	case Motion::Blur:
		return "Motion.Blur";
	case Motion::Spin:
		return "Motion.Spin";
	case Motion::Bounce:
		return "Motion.Bounce";
	case Motion::None:
		break;
	}

	return "Motion.None";
}

} // namespace ui
} // namespace slugged
