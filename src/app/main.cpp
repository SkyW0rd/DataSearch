#include "MainWindow.h"

#include <QApplication>
#include <QDir>
#include <QLockFile>
#include <QMessageBox>
#include <QStandardPaths>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("DataSearch");
    QApplication::setApplicationName("DataSearch");

    // One copy at a time: two would check and write the same indexes at
    // once, doubling the work and waiting on each other's locks. A lock
    // left by a copy that crashed is recognised as stale and taken over.
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    QLockFile instanceLock(QDir(dataDir).filePath(QStringLiteral("datasearch.lock")));
    if (!instanceLock.tryLock(200)) {
        QMessageBox::information(nullptr, QObject::tr("DataSearch"),
                                 QObject::tr("DataSearch уже запущен — его окно открыто или свёрнуто на панели задач."));
        return 0;
    }

    MainWindow window;
    window.show();

    return app.exec();
}
