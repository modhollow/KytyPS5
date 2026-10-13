#include "configurationPersistence.h"

#include <QCoreApplication>
#include <QMap>
#include <QTemporaryDir>
#include <QVariant>

#include <cstdio>

namespace {
int g_failures = 0;
void Check(bool ok, const char* message) {
	if (!ok) {
		std::printf("FAIL: %s\n", message);
		++g_failures;
	}
}
struct Configuration {
	QString game_path;
	int     known = 0;
	void WriteSettings(QSettings* settings) const {
		settings->setValue("game_path", game_path);
		settings->setValue("known", known);
	}
};
} // namespace

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	QTemporaryDir    directory;
	Check(directory.isValid(), "temporary fixture directory");
	const auto path = directory.filePath("Kyty.ini");
	{
		QSettings settings(path, QSettings::IniFormat);
		settings.setValue("Launcher/unchanged", "outside groups");
		settings.setValue("GlobalConfiguration/controller_color", "#112233");
		settings.setValue("GlobalConfiguration/audio_input_device", "future microphone");
		settings.setValue("GlobalConfiguration/skip_notice_screen", true);
		settings.setValue("GlobalConfiguration/nested/future", QByteArray("opaque"));
		settings.setValue("GlobalConfiguration/known", 1);
		settings.beginWriteArray("GameConfigurations");
		const QStringList games {"z", "a", "removed"};
		for (int i = 0; i < games.size(); ++i) {
			settings.setArrayIndex(i);
			settings.setValue("game_path", games[i]);
			settings.setValue("known", 2);
			settings.setValue("future", QStringList {games[i], "unknown setting"});
		}
		settings.endArray();
		Configuration global {"", 7}, a {"a", 8}, z {"z", 9};
		const QMap<QString, Configuration*> custom {{"a", &a}, {"z", &z}};
		LauncherSettings::WriteConfigurations(settings, "GlobalConfiguration", "GameConfigurations", global, custom);
		settings.sync();
		Check(settings.status() == QSettings::NoError, "fixture synced");
	}
	{
		QSettings settings(path, QSettings::IniFormat);
		Check(settings.value("GlobalConfiguration/controller_color").toString() == "#112233", "unknown controller color survives");
		Check(settings.value("GlobalConfiguration/audio_input_device").toString() == "future microphone", "unknown input device survives");
		Check(settings.value("GlobalConfiguration/skip_notice_screen").toBool(), "unknown boolean survives");
		Check(settings.value("GlobalConfiguration/nested/future").toByteArray() == "opaque", "unknown nested value survives");
		Check(settings.value("GlobalConfiguration/known").toInt() == 7, "known global value updated");
		Check(settings.value("Launcher/unchanged").toString() == "outside groups", "unrelated group unchanged");
		const int count = settings.beginReadArray("GameConfigurations");
		Check(count == 2, "removed game no longer in array");
		for (int i = 0; i < count; ++i) {
			settings.setArrayIndex(i);
			const auto expected = i == 0 ? QString("a") : QString("z");
			Check(settings.value("game_path").toString() == expected, "games reordered by map identity");
			Check(settings.value("future").toStringList() == QStringList {expected, "unknown setting"}, "unknown setting follows game, not old array index");
			Check(settings.value("known").toInt() == (i == 0 ? 8 : 9), "known game setting updated");
		}
		settings.endArray();
		Check(!settings.contains("GameConfigurations/3/game_path"), "no stale tail record");
		Configuration global {"", 10};
		const QMap<QString, Configuration*> empty;
		LauncherSettings::WriteConfigurations(settings, "GlobalConfiguration", "GameConfigurations", global, empty);
		Check(settings.beginReadArray("GameConfigurations") == 0, "clearing custom settings leaves an empty array");
		settings.endArray();
		Check(!settings.contains("GameConfigurations/1/future"), "empty array retains no removed game values");
		Configuration new_game {"new", 11};
		const QMap<QString, Configuration*> added {{"new", &new_game}};
		LauncherSettings::WriteConfigurations(settings, "GlobalConfiguration", "GameConfigurations", global, added);
		Check(settings.beginReadArray("GameConfigurations") == 1, "new game added after clearing");
		settings.setArrayIndex(0);
		Check(settings.value("game_path").toString() == "new" && !settings.contains("future"), "new game cannot inherit deleted game values");
		settings.endArray();
	}
	const auto migrated_path = directory.filePath("legacy.ini");
	const QString absolute_game = directory.filePath("Games/Legacy");
	{
		QSettings settings(migrated_path, QSettings::IniFormat);
		settings.beginWriteArray("GameConfigurations", 1);
		settings.setArrayIndex(0);
		settings.setValue("game_path", "Legacy");
		settings.setValue("future", QByteArray("legacy opaque value"));
		settings.endArray();
		Configuration global {"", 1}, legacy {"Legacy", 12};
		QMap<QString, Configuration*> custom {{"Legacy", &legacy}};
		QMap<QString, QString> aliases;
		Check(LauncherSettings::FindCustomInfo(custom, absolute_game, "Legacy", aliases) == &legacy, "actual legacy migration helper retains the existing configuration");
		Check(custom.contains(absolute_game) && legacy.game_path == absolute_game, "legacy identity becomes absolute before saving");
		LauncherSettings::WriteConfigurations(settings, "GlobalConfiguration", "GameConfigurations", global, custom, aliases);
		settings.sync();
	}
	{
		QSettings settings(migrated_path, QSettings::IniFormat);
		Check(settings.beginReadArray("GameConfigurations") == 1, "migrated record reloaded");
		settings.setArrayIndex(0);
		Check(settings.value("game_path").toString() == absolute_game, "new absolute identity saved");
		Check(settings.value("future").toByteArray() == "legacy opaque value", "legacy migration preserves unknown values after disk reload");
		settings.endArray();
	}
	std::printf("LauncherSettingsPersistenceTests: %d failures\n", g_failures);
	return g_failures ? 1 : 0;
}
