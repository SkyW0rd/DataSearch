#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QTimer>

#include <string>
#include <vector>

class IndexManager;
class QComboBox;
class QLabel;
class QProgressBar;

// Live view of what indexing is doing and where its time goes, refreshed
// twice a second: progress, speed, a per-stage time breakdown (walking,
// reading, parsing, writing...), data volumes, and a plain-words hint at the
// bottleneck. Modeless — searching and pausing keep working while it's open.
class MonitorDialog : public QDialog {
    Q_OBJECT
public:
    explicit MonitorDialog(IndexManager* manager, QWidget* parent = nullptr);

private:
    void refresh();

    IndexManager* manager_;
    QComboBox* source_ = nullptr;
    bool userChoseSource_ = false;  // until then, follow whichever source is working
    QLabel* phase_ = nullptr;
    QLabel* files_ = nullptr;
    QLabel* speed_ = nullptr;
    QLabel* activeTime_ = nullptr;

    struct StageRow {
        QLabel* name = nullptr;
        QProgressBar* bar = nullptr;
        QLabel* value = nullptr;
    };
    std::vector<StageRow> stages_;

    QLabel* readVolume_ = nullptr;
    QLabel* textVolume_ = nullptr;
    QLabel* indexSize_ = nullptr;
    QLabel* heavy_ = nullptr;
    QLabel* problems_ = nullptr;
    QLabel* advice_ = nullptr;

    QTimer timer_;
    QElapsedTimer clock_;

    // Current speed: files/s over ~1 s windows, for the selected source.
    std::string sampledRoot_;
    quint64 sampledVisited_ = 0;
    qint64 sampledAtMs_ = -1;
    double filesPerSecond_ = 0;
};
