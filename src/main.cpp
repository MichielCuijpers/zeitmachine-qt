#include "mainwindow.h"
#include <QApplication>
#include <QtPlugin>
#include <QScreen>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);

    MainWindow w;
    w.show();

    return application.exec();
}
