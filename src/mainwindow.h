#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QCollator>
#include <QDir>
#include <QFileDialog>
#include <QMessageBox>
#include <QMainWindow>
#include <QProgressBar>
#include <QThread>

#include "aboutdialog.h"
#include "settingsdialog.h"
#include "glvideowidget.h"
#include "zeitengine.h"

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

    Ui::MainWindow *ui;

    AboutDialog about;
    SettingsDialog settings;

    GLVideoWidget *videoWidget;
    QProgressBar *progressbar;

    ZeitEngine *zeitengine;
    QThread engineThread;

    QDir persistent_open_dir;

    /*!
     * \brief Disable other filter actions but the one passed on
     * \param The one single filter action that should not be disabled
     */
    void UncheckOtherFilters(QAction *action);

    void InitializeZeitdiceDirectory();
    void PersistZeitdiceDirectory(const QDir dir);

public:
    explicit MainWindow(QWidget *parent = 0);
    ~MainWindow();
signals:
    void LoadSignal(const QFileInfoList& sequence);
    void CacheSignal();
    void RefreshSignal();
    void PlaySignal();
    void ExportSignal(const QFileInfo file);
public slots:
    void EnableControls(const bool lock);
    void UpdateMessage(const QString text);

    /*!
     * \brief Update the progress bar
     * \param text Text that should be displayed
     * \param current Current step
     * \param total Total steps
     *
     * If current == total == 0, shows a busy indication (unknown progress)
     * If current and total are equal but > 0, hides the progress bar
     */
    void UpdateProgress(const QString text, const int current, const int total);
private slots:
    void on_actionAbout_triggered();
    void on_actionPlay_triggered();
    void on_actionStop_triggered();
    void on_actionLoop_triggered();
    void on_actionCycleFramerates_triggered();
    void on_actionVignette_triggered(bool checked);
    void on_actionBlackWhite_triggered(bool checked);
    void on_actionSepia_triggered(bool checked);
    void on_actionHipstagram_triggered(bool checked);
    void on_actionMovie_triggered();
    void on_actionSettings_triggered();
    void on_actionOpen_triggered();
    void on_actionFlipX_triggered();
    void on_actionFlipY_triggered();
    void on_actionRotateCCW_triggered();
    void on_actionRotateCW_triggered();
};

#endif // MAINWINDOW_H
