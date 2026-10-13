#include "launchProfile.h"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>

namespace {
int g_failures = 0;
void Check(bool ok, const char* message) {
	if (!ok) {
		std::printf("FAIL: %s\n", message);
		++g_failures;
	}
}
struct Item {
	bool running = false;
	void SetRunning(bool value) { running = value; }
};
void Write(const QString& path, const QByteArray& contents) {
	QFile file(path);
	Check(file.open(QIODevice::WriteOnly), "open profile fixture");
	Check(file.write(contents) == contents.size(), "write profile fixture");
}
} // namespace

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	if (app.arguments().contains("--child")) {
		if (app.arguments().contains("--hold")) QThread::msleep(150);
		const auto value = qgetenv("KYTY_VULKAN_RECORDING");
		std::printf("recording=%s\n", value.constData());
		return 0;
	}
	QTemporaryDir directory;
	Check(directory.isValid(), "temporary fixture directory");
	const auto emulator = directory.filePath("kyty_emulator.exe");
	const auto path     = directory.filePath("launch.json");
	auto inherited = QProcessEnvironment::systemEnvironment();
	inherited.remove("KYTY_VULKAN_RECORDING");
	inherited.remove("KYTY_LAUNCH_PROFILE");
	inherited.insert("KYTY_PIPELINE_LATE_BIND", "2");
	const auto parent_recording = qgetenv("KYTY_VULKAN_RECORDING");
	Write(path, R"({"checkpoint":"fixture","environment":{"KYTY_VULKAN_RECORDING":"1","KYTY_PIPELINE_LATE_BIND":"1"},"command":["ignored"]})");
	auto environment = inherited;
	Check(LauncherProfile::Apply(emulator, &environment).isEmpty(), "valid profile accepted");
	Check(environment.value("KYTY_VULKAN_RECORDING") == "1", "package recording default reaches child environment");
	Check(environment.value("KYTY_PIPELINE_LATE_BIND") == "2", "inherited explicit override wins");
	Check(qgetenv("KYTY_VULKAN_RECORDING") == parent_recording, "parent environment unchanged");
	QProcess child;
	child.setProgram(QCoreApplication::applicationFilePath());
	child.setArguments({"--child"});
	child.setProcessEnvironment(environment);
	child.start();
	Check(child.waitForStarted(5000) && child.waitForFinished(5000), "fixture child completed");
	Check(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0, "fixture child exited normally");
	Check(child.readAllStandardOutput().contains("recording=1"), "real QProcess receives profile default");
	Item item;
	Write(path, "{");
	environment = inherited;
	LauncherProfile::StartGame(item, [&] { return LauncherProfile::Apply(emulator, &environment).isEmpty(); });
	Check(!item.running, "rejected profile clears the running state");
	Write(path, R"({"environment":{"KYTY_VULKAN_RECORDING":"1"}})");
	child.setArguments({"--child", "--hold"});
	QObject::connect(&child, &QProcess::finished, [&] { item.SetRunning(false); });
	LauncherProfile::StartGame(item, [&] {
		if (!LauncherProfile::Apply(emulator, &environment).isEmpty()) return false;
		child.setProcessEnvironment(environment);
		child.start();
		return child.waitForStarted(5000);
	});
	Check(item.running, "valid retry becomes running");
	Check(child.waitForFinished(5000) && !item.running, "retry exits and clears running state");
	environment = inherited;
	environment.insert("KYTY_LAUNCH_PROFILE", "0");
	const auto disabled = environment;
	Check(LauncherProfile::Apply(emulator, &environment).isEmpty() && environment == disabled, "explicit disable preserves inherited environment");
	for (const auto& invalid: {QByteArray("{"), QByteArray("[]"), QByteArray("{\"environment\":null}"), QByteArray("{\"environment\":{\"A_GOOD\":\"value\",\"Z_BAD\":2}}"), QByteArray("{\"environment\":{\"BAD=NAME\":\"value\"}}"), QByteArray("{\"environment\":{\"GOOD\":\"a\\u0000b\"}}"), QByteArray("{\"environment\":{\"BAD\\n\":\"value\"}}")}) {
		Write(path, invalid);
		environment = inherited;
		Check(!LauncherProfile::Apply(emulator, &environment).isEmpty(), "malformed profile returns actionable error");
		Check(environment == inherited, "malformed profile cannot partially alter environment");
	}
	Check(QFile::remove(path), "remove temporary profile");
	environment = inherited;
	Check(LauncherProfile::Apply(emulator, &environment).isEmpty() && environment == inherited, "missing profile retains old launch behavior");
	std::printf("LauncherLaunchProfileTests: %d failures\n", g_failures);
	return g_failures ? 1 : 0;
}
