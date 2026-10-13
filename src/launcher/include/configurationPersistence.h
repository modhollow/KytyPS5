#pragma once

#include <QMap>
#include <QSettings>
#include <QString>
#include <QVariant>

namespace LauncherSettings {

template <typename Configurations>
auto FindCustomInfo(Configurations& custom, const QString& game_path,
                    const QString& legacy_game_path, QMap<QString, QString>& identity_aliases) ->
    typename Configurations::mapped_type {
	auto found = custom.find(game_path);
	if (found != custom.end()) return found.value();
	found = custom.find(legacy_game_path);
	if (found == custom.end()) return nullptr;
	auto* info = found.value();
	custom.erase(found);
	identity_aliases.insert(game_path, legacy_game_path);
	info->game_path = game_path;
	custom.insert(game_path, info);
	return info;
}

// Shared by the launcher and the isolated ini regression.
template <typename Configuration, typename Configurations>
void WriteConfigurations(QSettings& settings, const QString& global_group,
                         const QString& array_group, const Configuration& global,
                         const Configurations&         custom,
                         const QMap<QString, QString>& identity_aliases = {}) {
	// Overwrite known global keys without deleting fields another launcher version owns.
	settings.beginGroup(global_group);
	global.WriteSettings(&settings);
	settings.endGroup();
	// Array indexes change when games are reordered/removed. Associate retained
	// values with game_path before rebuilding the array, never with the old index.
	QMap<QString, QVariantMap> retained;
	const int                  count = settings.beginReadArray(array_group);
	for (int index = 0; index < count; ++index) {
		settings.setArrayIndex(index);
		const auto identity = settings.value("game_path").toString();
		if (identity.isEmpty()) continue;
		auto& values = retained[identity];
		for (const auto& key: settings.allKeys())
			values.insert(key, settings.value(key));
	}
	settings.endArray();
	settings.remove(array_group);
	settings.beginWriteArray(array_group, static_cast<int>(custom.size()));
	int i = 0;
	for (auto it = custom.constBegin(); it != custom.constEnd(); ++it) {
		settings.setArrayIndex(i++);
		// ScanGameDirectory can migrate a legacy relative key before the first save.
		// Prefer the current on-disk identity on later saves; use the exact recorded
		// alias only when that identity has not been written yet.
		const auto identity =
		    retained.contains(it.key()) ? it.key() : identity_aliases.value(it.key(), it.key());
		const auto values = retained.value(identity);
		for (auto value = values.constBegin(); value != values.constEnd(); ++value)
			settings.setValue(value.key(), value.value());
		it.value()->WriteSettings(&settings);
	}
	settings.endArray();
}

} // namespace LauncherSettings
