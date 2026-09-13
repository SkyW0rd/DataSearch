#pragma once

#include "IndexManager.h"

#include <QDialog>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class QGridLayout;
class QLabel;
class QPushButton;
class QTableWidget;

// Files whose text couldn't be read: how many there are for each reason,
// reading them again (a whole reason's worth, all of them, or picked ones)
// and — when wanted — their paths, to open a file or show it in its folder.
// Such files are in the index by name and skipped by startup checks until
// they change; this is where the user can make the app try again.
// Modeless; follows the index through IndexManager::problemFilesChanged.
class ProblemFilesDialog : public QDialog {
    Q_OBJECT
public:
    ProblemFilesDialog(IndexManager* manager, datasearch::platform::IPlatformService* platform,
                       QWidget* parent = nullptr);

private:
    void reload();
    void rebuildSummary();
    void fillTable();
    void showList(int reason);  // -1: every file
    void retry(const std::vector<std::size_t>& indexes);
    std::vector<std::size_t> selectedIndexes() const;
    void openSelected();
    void showSelectedInFolder();

    IndexManager* manager_;
    datasearch::platform::IPlatformService* platform_;
    std::vector<IndexManager::ProblemFileEntry> files_;

    QLabel* intro_ = nullptr;
    QGridLayout* summary_ = nullptr;
    QWidget* listBox_ = nullptr;
    QLabel* listTitle_ = nullptr;
    QTableWidget* table_ = nullptr;
    QPushButton* retrySelected_ = nullptr;
    QLabel* status_ = nullptr;
    int shownReason_ = -1;

    // A retry in flight: the files asked for and when. It's over once each
    // of them is either off the list (read fine) or was tried again since.
    std::vector<std::pair<std::string, std::string>> retrying_;
    std::int64_t retryStartedAt_ = 0;
};
