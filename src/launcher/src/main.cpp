#include "launcherTheme.h"
#include "mainDialog.h"

#include <QApplication>
#include <QArgument>
#include <QObject>

int main(int argc, char* argv[]) {
	QApplication a(argc, argv);
	LauncherTheme::Initialize(a);

	MainDialog w;

	QObject::connect(&a, &QApplication::aboutToQuit, &w, &MainDialog::Quit);

	w.emit Start();

	w.show();

	return QApplication::exec();
}
