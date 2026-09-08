#include "MainWindow.h"

#include <QApplication>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("DataSearch");
    QApplication::setApplicationName("DataSearch");

    MainWindow window;
    window.show();

    return app.exec();
}
