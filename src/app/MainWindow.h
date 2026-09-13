#pragma once

#include "datasearch/core/IndexStorage.h"
#include "datasearch/platform/IPlatformService.h"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QModelIndex>
#include <QPoint>
#include <QPointer>
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
class QCheckBox;
class QPushButton;
class QAction;

class IndexManager;
class ResultsTableModel;
class MonitorDialog;

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
    void requestVisibleSnippets();
    void onResultsContextMenuRequested(const QPoint& pos);
    void onResultDoubleClicked(const QModelIndex& index);
    void onWatcherActivity(const QString& rootLabel, const QString& description);
    void onSourceUnavailable(const QString& rootLabel);
    void onPauseResumeClicked();
    void onExcludeMasksEdited();
    void onAddFolderClicked();
    void showMonitor();
    void onResultsHeaderClicked(int column);

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

    // Result order and the "Фильтры" menu. Sorting is done by the index, over
    // every match, so the rows shown are the first ones in that order. All
    // choices are remembered between sessions.
    void buildFiltersMenu();
    void setSort(datasearch::core::SortField field, bool descending);
    void setFilters(int typeFilter, int dateFilter);
    void syncSortAndFiltersUi();
    QString activeFiltersText() const;  // e.g. "Word · за 30 дней"; empty when none
    // How a path too long for its column is shortened: in the middle, at
    // the start, at the end, or not at all (Qt::ElideNone: the column is
    // made as wide as the longest path shown).
    void setPathElide(Qt::TextElideMode mode);
    void fitPathColumn();

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
    QLabel* elapsedLabel_ = nullptr;       // on the bar, left: time since indexing started
    QLabel* etaLabel_ = nullptr;           // on the bar, right: estimate of time left
    QLabel* statusLabel_ = nullptr;       // search results and one-off notices
    QLabel* indexStatusLabel_ = nullptr;  // what indexing is doing right now
    QPushButton* indexButton_ = nullptr;
    QPushButton* pauseResumeButton_ = nullptr;
    QPushButton* addFolderButton_ = nullptr;
    QPointer<MonitorDialog> monitor_;  // one at a time; deletes itself on close
    QTimer searchDebounce_;
    QCheckBox* wordFormsCheck_ = nullptr;  // off: whole words exactly as typed
    QPushButton* filterChip_ = nullptr;    // shows the active filters; a click clears them

    datasearch::core::SortField sortField_ = datasearch::core::SortField::Path;
    bool sortDescending_ = false;
    bool foldersFirst_ = true;  // by path: subfolders before a folder's own files
    int typeFilter_ = 0;  // index into the type filter list, 0 = all files
    int dateFilter_ = 0;  // index into the date filter list, 0 = any time
    std::vector<QAction*> sortActions_;
    std::vector<QAction*> typeActions_;
    std::vector<QAction*> dateActions_;
    QAction* resetFiltersAction_ = nullptr;
    Qt::TextElideMode pathElide_ = Qt::ElideMiddle;  // read by the path column's delegate
    // Results of anything but the latest search are dropped.
    quint64 searchRequest_ = 0;

    // Excerpts are fetched only for rows on screen, after the results
    // themselves (building one re-reads the whole file's text). The
    // generation ties late-arriving excerpts to the result list they're for.
    QTimer snippetDebounce_;
    QString shownPattern_;
    bool shownExact_ = true;
    quint64 resultsGeneration_ = 0;
    std::vector<bool> snippetRequested_;

    // Indexing progress is polled rather than pushed per file: the display
    // always reflects the file actually in flight (including one that takes
    // minutes), and a fast scan can't flood the event queue.
    QTimer indexStatusTimer_;
    QElapsedTimer clock_;
    qint64 activeElapsedMs_ = 0;     // indexing time shown on the bar, pauses excluded
    qint64 lastElapsedTickMs_ = -1;
    struct RateSample {
        quint64 visited = 0;
        qint64 atMs = -1;
        double filesPerSecond = 0;
    };
    std::map<std::string, RateSample> rates_;
};
