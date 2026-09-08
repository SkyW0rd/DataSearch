#pragma once

#include "datasearch/platform/IPlatformService.h"

#include <QMainWindow>
#include <QModelIndex>
#include <QPoint>
#include <QTimer>

#include <memory>
#include <string>
#include <vector>

class QLineEdit;
class QListWidget;
class QTableView;
class QProgressBar;
class QLabel;
class QPushButton;

class IndexManager;
class ResultsTableModel;

// Главное окно (ТЗ п.7.1): строка поиска, панель выбора дисков, таблица результатов.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void onSearchTextChanged(const QString& text);
    void runSearch();
    void onIndexSelectedClicked();
    void onIndexProgress(quint64 filesIndexed, const QString& currentPath, const QString& rootLabel);
    void onIndexFinished(const QString& rootLabel, bool cancelled);
    void onResultsContextMenuRequested(const QPoint& pos);
    void onResultDoubleClicked(const QModelIndex& index);
    void onWatcherActivity(const QString& rootLabel, const QString& description);
    void onSourceUnavailable(const QString& rootLabel);
    void onPauseResumeClicked();
    void onExcludeMasksEdited();

private:
    void populateVolumes();
    QStringList checkedRoots() const;
    void openRow(int row);
    void showRowInFolder(int row);
    void copyRowPath(int row);

    std::vector<std::string> parseExcludeMasks() const;

    std::unique_ptr<datasearch::platform::IPlatformService> platform_;
    IndexManager* indexManager_ = nullptr;

    QListWidget* volumeList_ = nullptr;
    QLineEdit* searchEdit_ = nullptr;
    QLineEdit* excludeMasksEdit_ = nullptr;
    QTableView* resultsView_ = nullptr;
    ResultsTableModel* resultsModel_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QPushButton* indexButton_ = nullptr;
    QPushButton* pauseResumeButton_ = nullptr;
    QTimer searchDebounce_;
};
