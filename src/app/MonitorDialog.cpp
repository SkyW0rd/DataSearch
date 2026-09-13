#include "MonitorDialog.h"

#include "Format.h"
#include "IndexManager.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <array>

using datasearch::core::IndexerStatus;
using datasearch::core::IndexPhase;
using namespace display;

namespace {

QString phaseName(const IndexerStatus& s) {
    if (s.phase == IndexPhase::Finished) return s.cancelled ? QObject::tr("Остановлено") : QObject::tr("Готово");
    if (s.phase == IndexPhase::Idle) return QObject::tr("Не запускалась");
    if (s.cancelRequested) return QObject::tr("Остановка");
    if (s.paused) return QObject::tr("На паузе");
    if (s.pauseRequested) return QObject::tr("Ставится на паузу (дожидаемся текущего файла)");
    switch (s.phase) {
        case IndexPhase::Upgrading: return QObject::tr("Обновление индекса для точного поиска");
        case IndexPhase::Counting: return QObject::tr("Подсчёт файлов");
        case IndexPhase::Indexing:
            if (s.processingHeavy) return QObject::tr("Крупные файлы");
            return s.reconcile ? QObject::tr("Проверка изменений с прошлого запуска") : QObject::tr("Индексация");
        case IndexPhase::Finishing: return QObject::tr("Сохранение индекса");
        default: return {};
    }
}

QString seconds(double s) {
    return s < 60 ? ru().toString(s, 'f', 1) + QObject::tr(" с") : formatDuration(static_cast<qint64>(s + 0.5));
}

// The biggest share of the time, in plain words, and what would help.
QString adviceFor(const IndexerStatus::Timing& t) {
    const double walk = t.counting + t.walking;
    const double read = t.reading;
    const double parse = t.parsing;
    const double write = t.writing + t.saving;
    const double total = walk + read + parse + write + t.upgrading;
    if (total < 3) return QObject::tr("Данных пока мало — выводы появятся после нескольких секунд индексации.");

    const std::array<std::pair<double, int>, 4> shares{{{read, 0}, {parse, 1}, {write, 2}, {walk, 3}}};
    const auto top = *std::max_element(shares.begin(), shares.end());
    const int percent = static_cast<int>(100 * top.first / total + 0.5);
    if (top.first / total < 0.4) {
        return QObject::tr("Время распределено равномерно — явного узкого места нет.");
    }
    switch (top.second) {
        case 0: {
            const double mbps = read > 0 ? static_cast<double>(t.bytesRead) / 1048576.0 / read : 0;
            QString text = QObject::tr("Больше всего времени (%1%) уходит на чтение файлов с диска — %2 МБ/с. ")
                               .arg(percent)
                               .arg(ru().toString(mbps, 'f', 1));
#if defined(Q_OS_WIN)
            text += QObject::tr(
                "На SSD это обычно проверка каждого открываемого файла Защитником Windows. Добавьте в "
                "исключения процесс datasearch_app.exe и папку с индексами (кнопка ниже) — индексация "
                "ускорится в разы.");
#else
            text += QObject::tr("Возможно, диск медленный или подключён по USB или сети.");
#endif
            return text;
        }
        case 1:
            return QObject::tr(
                       "Больше всего времени (%1%) уходит на разбор документов (PDF, DOCX, XLSX) — это работа "
                       "процессора; документы уже разбираются в нескольких потоках одновременно.")
                .arg(percent);
        case 2:
            return QObject::tr(
                       "Больше всего времени (%1%) уходит на запись в индекс — обработку текста базой данных. "
                       "Ускорит отказ от дополнительных индексов начал слов.")
                .arg(percent);
        default:
            return QObject::tr(
                       "Больше всего времени (%1%) уходит на обход папок — много мелких файлов и папок. "
                       "Ускорит чтение журнала изменений NTFS вместо обхода.")
                .arg(percent);
    }
}

} // namespace

MonitorDialog::MonitorDialog(IndexManager* manager, QWidget* parent) : QDialog(parent), manager_(manager) {
    setWindowTitle(tr("Мониторинг индексации"));
    setModal(false);
    resize(620, 560);

    auto* layout = new QVBoxLayout(this);

    auto* sourceRow = new QHBoxLayout();
    sourceRow->addWidget(new QLabel(tr("Источник:"), this));
    source_ = new QComboBox(this);
    connect(source_, &QComboBox::activated, this, [this] {
        userChoseSource_ = true;
        refresh();
    });
    sourceRow->addWidget(source_, 1);
    layout->addLayout(sourceRow);

    auto* progressBox = new QGroupBox(tr("Ход"), this);
    auto* progressForm = new QFormLayout(progressBox);
    phase_ = new QLabel(progressBox);
    files_ = new QLabel(progressBox);
    speed_ = new QLabel(progressBox);
    activeTime_ = new QLabel(progressBox);
    threads_ = new QLabel(progressBox);
    progressForm->addRow(tr("Этап:"), phase_);
    progressForm->addRow(tr("Файлы:"), files_);
    progressForm->addRow(tr("Скорость:"), speed_);
    progressForm->addRow(tr("Время работы:"), activeTime_);
    progressForm->addRow(tr("Потоки чтения:"), threads_);
    layout->addWidget(progressBox);

    auto* timeBox = new QGroupBox(tr("Куда уходит время (сумма по всем потокам, без пауз)"), this);
    auto* timeGrid = new QGridLayout(timeBox);
    const QStringList stageNames = {tr("Подсчёт файлов"),         tr("Обход папок"),
                                    tr("Чтение файлов с диска"),  tr("Разбор документов"),
                                    tr("Запись в индекс"),        tr("Сохранение на диск"),
                                    tr("Обновление точного индекса")};
    for (int i = 0; i < stageNames.size(); ++i) {
        StageRow row;
        row.name = new QLabel(stageNames[i], timeBox);
        row.bar = new QProgressBar(timeBox);
        row.bar->setRange(0, 1000);
        row.bar->setTextVisible(false);
        row.bar->setMaximumHeight(12);
        row.value = new QLabel(timeBox);
        row.value->setMinimumWidth(110);
        row.value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        timeGrid->addWidget(row.name, i, 0);
        timeGrid->addWidget(row.bar, i, 1);
        timeGrid->addWidget(row.value, i, 2);
        stages_.push_back(row);
    }
    timeGrid->setColumnStretch(1, 1);
    layout->addWidget(timeBox);

    auto* volumeBox = new QGroupBox(tr("Объёмы"), this);
    auto* volumeForm = new QFormLayout(volumeBox);
    readVolume_ = new QLabel(volumeBox);
    textVolume_ = new QLabel(volumeBox);
    indexSize_ = new QLabel(volumeBox);
    heavy_ = new QLabel(volumeBox);
    problems_ = new QLabel(volumeBox);
    volumeForm->addRow(tr("Прочитано с диска:"), readVolume_);
    volumeForm->addRow(tr("Извлечено текста:"), textVolume_);
    volumeForm->addRow(tr("Размер индекса на диске:"), indexSize_);
    volumeForm->addRow(tr("Крупные файлы:"), heavy_);
    volumeForm->addRow(tr("Проблемы:"), problems_);
    layout->addWidget(volumeBox);

    advice_ = new QLabel(this);
    advice_->setWordWrap(true);
    advice_->setMargin(8);
    advice_->setStyleSheet(QStringLiteral("QLabel { border: 1px solid palette(mid); border-radius: 6px; }"));
    layout->addWidget(advice_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    // Qt's own translations aren't shipped with the app, so a standard
    // button would read "Close".
    buttons->button(QDialogButtonBox::Close)->setText(tr("Закрыть"));
    auto* openFolder = buttons->addButton(tr("Открыть папку с индексами"), QDialogButtonBox::ActionRole);
    connect(openFolder, &QPushButton::clicked, this,
            [] { QDesktopServices::openUrl(QUrl::fromLocalFile(IndexManager::indexDirectory())); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);

    clock_.start();
    timer_.setInterval(500);
    connect(&timer_, &QTimer::timeout, this, &MonitorDialog::refresh);
    timer_.start();
    refresh();
}

void MonitorDialog::refresh() {
    const auto statuses = manager_->indexingStatus();

    // Keep the list of sources current; pick the one that's working (or the
    // latest to finish) until the user chooses one.
    QStringList roots;
    for (const auto& entry : statuses) roots << QString::fromStdString(entry.root);
    QStringList shown;
    for (int i = 0; i < source_->count(); ++i) shown << source_->itemText(i);
    if (roots != shown) {
        const QString previous = source_->currentText();
        source_->clear();
        source_->addItems(roots);
        const int keep = roots.indexOf(previous);
        if (keep >= 0) source_->setCurrentIndex(keep);
    }
    if (statuses.empty()) {
        phase_->setText(tr("Индексация в этом сеансе ещё не запускалась"));
        return;
    }
    if (!userChoseSource_ || source_->currentIndex() < 0) {
        int best = 0;
        for (int i = 0; i < static_cast<int>(statuses.size()); ++i) {
            const auto phase = statuses[i].status.phase;
            if (phase != IndexPhase::Finished && phase != IndexPhase::Idle) {
                best = i;
                break;
            }
            if (statuses[i].status.finishedAt > statuses[best].status.finishedAt) best = i;
        }
        source_->setCurrentIndex(best);
    }
    const auto& entry = statuses[static_cast<std::size_t>(std::max(0, source_->currentIndex()))];
    const IndexerStatus& s = entry.status;
    const auto& t = s.timing;

    // Current speed over ~1 s windows.
    const qint64 nowMs = clock_.elapsed();
    const bool moving = (s.phase == IndexPhase::Indexing || s.phase == IndexPhase::Upgrading) && !s.pauseRequested;
    if (entry.root != sampledRoot_ || !moving || s.filesVisited < sampledVisited_) {
        sampledRoot_ = entry.root;
        sampledVisited_ = s.filesVisited;
        sampledAtMs_ = nowMs;
        if (!moving) filesPerSecond_ = 0;
    } else if (nowMs - sampledAtMs_ >= 1000) {
        const double instant = static_cast<double>(s.filesVisited - sampledVisited_) * 1000.0 / (nowMs - sampledAtMs_);
        filesPerSecond_ = filesPerSecond_ == 0 ? instant : 0.7 * filesPerSecond_ + 0.3 * instant;
        sampledVisited_ = s.filesVisited;
        sampledAtMs_ = nowMs;
    }

    // Stage times are summed over threads (several files are read at once),
    // so shares are of that sum; elapsed time and speed use the wall clock.
    const double active =
        t.counting + t.walking + t.reading + t.parsing + t.writing + t.saving + t.upgrading;

    phase_->setText(phaseName(s));
    QString files = tr("%1 из %2%3")
                        .arg(formatCount(s.filesVisited),
                             (s.totalIsEstimate ? QStringLiteral("~") : QString()) + formatCount(s.filesTotal),
                             s.filesTotal > 0 ? tr(" (%1%)").arg(std::min<quint64>(100, s.filesVisited * 100 / s.filesTotal))
                                              : QString());
    if (s.reconcile) files += tr(", обновлено %1").arg(formatCount(s.filesWritten));
    files_->setText(files);
    const double average = s.activeSeconds > 0 ? static_cast<double>(s.filesVisited) / s.activeSeconds : 0;
    speed_->setText(tr("сейчас %1 файлов/с, в среднем %2 файлов/с")
                        .arg(formatCount(static_cast<quint64>(filesPerSecond_ + 0.5)),
                             formatCount(static_cast<quint64>(average + 0.5))));
    activeTime_->setText(formatClock(static_cast<qint64>(s.activeSeconds)));
    if (s.extractionThreads == 0) {
        threads_->setText(tr("—"));
    } else {
        const auto rotational = manager_->diskIsRotational(entry.root);
        const QString disk = !rotational ? tr("тип диска не определён") : *rotational ? tr("жёсткий диск") : tr("SSD");
        const QString how = manager_->readThreads() == 0 ? tr("автоматически") : tr("задано вручную");
        threads_->setText(tr("%1 (%2, %3)").arg(s.extractionThreads).arg(disk, how));
    }

    const std::array<double, 7> values{t.counting, t.walking, t.reading, t.parsing, t.writing, t.saving, t.upgrading};
    for (std::size_t i = 0; i < stages_.size(); ++i) {
        const double share = active > 0 ? values[i] / active : 0;
        stages_[i].bar->setValue(static_cast<int>(share * 1000));
        stages_[i].value->setText(tr("%1 · %2%").arg(seconds(values[i])).arg(static_cast<int>(share * 100 + 0.5)));
        // The exact-index upgrade only ever happens once; hide it otherwise.
        const bool visible = i != 6 || values[i] > 0 || s.phase == IndexPhase::Upgrading;
        stages_[i].name->setVisible(visible);
        stages_[i].bar->setVisible(visible);
        stages_[i].value->setVisible(visible);
    }

    const double mbps = t.reading > 0 ? static_cast<double>(t.bytesRead) / 1048576.0 / t.reading : 0;
    readVolume_->setText(tr("%1 (%2 МБ/с)").arg(formatSize(t.bytesRead), ru().toString(mbps, 'f', 1)));
    textVolume_->setText(tr("%1 из %2 файлов").arg(formatSize(t.textBytes), formatCount(t.filesWithText)));
    indexSize_->setText(formatSize(IndexManager::indexSizeOnDisk(entry.root)));
    heavy_->setText(s.heavyFound > 0 ? tr("%1 из %2 (%3)")
                                           .arg(formatCount(s.heavyDone), formatCount(s.heavyFound),
                                                formatSize(s.heavyBytesTotal))
                                     : tr("нет"));
    problems_->setText(tr("не удалось прочитать текст файлов: %1 (ищутся по имени), недоступных папок: %2")
                           .arg(formatCount(s.filesFailed), formatCount(s.unreadableDirs)));
    advice_->setText(adviceFor(t));
}
