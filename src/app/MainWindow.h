#pragma once

#include "datasearch/platform/IPlatformService.h"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QModelIndex>
#include <QPoint>
#include <QTimer>

#include <map>
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
    void onIndexFinished(const QString& rootLabel, bool cancelled);
    void updateIndexingStatus();
    void onResultsContextMenuRequested(const QPoint& pos);
    void onResultDoubleClicked(const QModelIndex& index);
    void onWatcherActivity(const QString& rootLabel, const QString& description);
    void onSourceUnavailable(const QString& rootLabel);
    void onPauseResumeClicked();
    void onExcludeMasksEdited();
    void onAddFolderClicked();

private:
    void populateVolumes();
    void addSourceItem(const QString& root, const QString& text, bool checked);
    QStringList checkedRoots() const;
    void openRow(int row);
    void showRowInFolder(int row);
    void copyRowPath(int row);
    // Right-hand status text, shortened with an ellipsis; full text in the tooltip.
    void showNotice(const QString& text);

    std::vector<std::string> parseExcludeMasks() const;

    std::unique_ptr<datasearch::platform::IPlatformService> platform_;
    IndexManager* indexManager_ = nullptr;

    QListWidget* volumeList_ = nullptr;
    QLineEdit* searchEdit_ = nullptr;
    QLineEdit* excludeMasksEdit_ = nullptr;
    QTableView* resultsView_ = nullptr;
    ResultsTableModel* resultsModel_ = nullptr;
    QWidget* progressRow_ = nullptr;
    QProgressBar* busyBar_ = nullptr;      // animates while indexing is actually working
    QProgressBar* progressBar_ = nullptr;  // share of files done
    QLabel* statusLabel_ = nullptr;       // search results and one-off notices
    QLabel* indexStatusLabel_ = nullptr;  // what indexing is doing right now
    QPushButton* indexButton_ = nullptr;
    QPushButton* pauseResumeButton_ = nullptr;
    QPushButton* addFolderButton_ = nullptr;
    QTimer searchDebounce_;

    // Indexing progress is polled rather than pushed per file: the display
    // always reflects the file actually in flight (including one that takes
    // minutes), and a fast scan can't flood the event queue.
    QTimer indexStatusTimer_;
    QElapsedTimer clock_;
    struct RateSample {
        quint64 visited = 0;
        qint64 atMs = -1;
        double filesPerSecond = 0;
    };
    std::map<std::string, RateSample> rates_;
};
