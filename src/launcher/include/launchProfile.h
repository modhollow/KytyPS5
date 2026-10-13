#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QString>

namespace LauncherProfile {
template <typename Item, typename Start>
void StartGame(Item& item, Start start) {
	item.SetRunning(true);
	if (!start()) item.SetRunning(false);
}

inline QString Apply(const QString& emulator, QProcessEnvironment* environment) {
	if (environment->value("KYTY_LAUNCH_PROFILE") == "0") return {};
	const auto path = QFileInfo(emulator).absoluteDir().filePath("launch.json");
	if (!QFileInfo::exists(path)) return {};
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return QStringLiteral("Cannot read launch profile %1: %2").arg(path, file.errorString());
	QJsonParseError error;
	const auto      document = QJsonDocument::fromJson(file.readAll(), &error);
	if (error.error != QJsonParseError::NoError)
		return QStringLiteral("Invalid launch profile %1: %2").arg(path, error.errorString());
	if (!document.isObject() || !document.object().value("environment").isObject())
		return QStringLiteral("Launch profile %1 must contain an environment object.").arg(path);
	const auto               values = document.object().value("environment").toObject();
	const QRegularExpression name_pattern(QStringLiteral("\\A[A-Za-z_][A-Za-z0-9_]*\\z"));
	auto                     merged = *environment;
	for (auto value = values.constBegin(); value != values.constEnd(); ++value) {
		if (!name_pattern.match(value.key()).hasMatch() || !value.value().isString() ||
		    value.value().toString().contains(QChar('\0')))
			return QStringLiteral(
			           "Launch profile %1 requires valid environment names and string values.")
			    .arg(path);
		// A shell or test harness can explicitly override package defaults.
		if (!environment->contains(value.key()))
			merged.insert(value.key(), value.value().toString());
	}
	*environment = merged;
	return {};
}
} // namespace LauncherProfile
