#include "MainWindow.h"

#include "IndexManager.h"
#include "ResultsTableModel.h"

#include "datasearch/core/IndexStorage.h"
#include "datasearch/core/Utf8.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <set>

#include <QAbstractItemView>
#include <QAction>
#include <QCheckBox>
#include <QScrollBar>
#include <QClipboard>
#include <QColor>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QSet>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTableView>
#include <QVBoxLayout>

using datasearch::core::IndexerStatus;
using datasearch::core::IndexPhase;
using datasearch::core::SearchQuery;
using datasearch::platform::VolumeType;

namespace {
const char* kSettingsExcludeMasksKey = "excludeMasks";
const char* kSettingsWordFormsKey = "searchWordForms";
constexpr int kResultLimit = 2000;
const char* kDefaultExcludeMasks = "*.tmp, node_modules, .git";
constexpr int kFileNameWidth = 480;
constexpr int kNoticeWidth = 320;

// A drive ("D:\", "/") stays as it is; a folder shows by its own name.
QString shortRootName(const QString& root) {
    const QString name = QFileInfo(QDir::cleanPath(root)).fileName();
    return name.isEmpty() ? root : name;
}

const QLocale& ru() {
    static const QLocale locale(QLocale::Russian, QLocale::Russia);
    return locale;
}

QString formatCount(quint64 n) {
    return ru().toString(static_cast<qulonglong>(n));
}

QString formatSize(quint64 bytes) {
    if (bytes >= 1024ull * 1024 * 1024) return ru().toString(bytes / 1073741824.0, 'f', 1) + QObject::tr(" ГБ");
    if (bytes >= 1024ull * 1024) return ru().toString(bytes / 1048576.0, 'f', 1) + QObject::tr(" МБ");
    if (bytes >= 1024) return ru().toString(bytes / 1024.0, 'f', 0) + QObject::tr(" КБ");
    return ru().toString(static_cast<qulonglong>(bytes)) + QObject::tr(" Б");
}

// Bar colour by completion, red through orange, green and teal to blue.
QColor progressColor(double fraction) {
    static const QColor stops[] = {QColor(0xe0, 0x4b, 0x3a), QColor(0xee, 0x8a, 0x2e), QColor(0xb5, 0xc9, 0x3b),
                                   QColor(0x4c, 0xaf, 0x50), QColor(0x2a, 0x9d, 0x8f), QColor(0x3b, 0x82, 0xc4)};
    constexpr int kLast = static_cast<int>(std::size(stops)) - 1;
    const double pos = std::clamp(fraction, 0.0, 1.0) * kLast;
    const int i = std::min(static_cast<int>(pos), kLast - 1);
    const double t = pos - i;
    auto mix = [t](int a, int b) { return static_cast<int>(a + (b - a) * t + 0.5); };
    return QColor(mix(stops[i].red(), stops[i + 1].red()), mix(stops[i].green(), stops[i + 1].green()),
                  mix(stops[i].blue(), stops[i + 1].blue()));
}

QString formatDuration(qint64 seconds) {
    if (seconds < 60) return QObject::tr("%1 с").arg(seconds);
    if (seconds < 3600) return QObject::tr("%1 мин %2 с").arg(seconds / 60).arg(seconds % 60);
    return QObject::tr("%1 ч %2 мин").arg(seconds / 3600).arg((seconds % 3600) / 60);
}

// One source's indexing state in plain words — every stage the backend goes
// through, so a long step (counting a big disk, a huge file, a pause waiting
// for the file in flight) reads as work in progress rather than a hang.
// Seconds until this source is done, or -1 when there's no basis yet. During
// the main pass only ordinary files count — the heavy ones wait for the end
// and don't move at the files/s rate; their own forecast takes over once
// they start (see IndexerStatus::heavySecondsLeft).
double secondsLeft(const IndexerStatus& s, double filesPerSecond) {
    if (s.phase == IndexPhase::Upgrading) {
        return filesPerSecond >= 1 && s.filesTotal > s.filesVisited
                   ? static_cast<double>(s.filesTotal - s.filesVisited) / filesPerSecond
                   : -1;
    }
    if (s.phase != IndexPhase::Indexing) return -1;
    if (s.processingHeavy) return s.heavySecondsLeft;
    if (filesPerSecond < 1) return -1;
    const quint64 heavyLeft = s.heavyFound - std::min(s.heavyDone, s.heavyFound);
    const quint64 notDone = s.filesTotal - std::min(s.filesVisited, s.filesTotal);
    const quint64 normalLeft = notDone - std::min(heavyLeft, notDone);
    return static_cast<double>(normalLeft) / filesPerSecond;
}

// 42 s -> "00:42", 1 h 5 min -> "1:05:00".
QString formatClock(qint64 seconds) {
    const qint64 h = seconds / 3600;
    const qint64 m = (seconds % 3600) / 60;
    const qint64 sec = seconds % 60;
    return h > 0 ? QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0'))
                 : QString("%1:%2").arg(m, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0'));
}

// `compact`: folder and file names only, to fit the one-line status bar;
// otherwise full paths, for its tooltip.

QString describeIndexing(const QString& root, const IndexerStatus& s, double filesPerSecond,
                         const QFontMetrics& fm, bool compact) {
    using std::chrono::duration_cast;
    using std::chrono::seconds;

    const QString name = compact ? shortRootName(root) : root;
    QString file = QString::fromStdString(s.currentPath);
    // Compact: the path inside the indexed folder, so the subfolder nesting
    // shows; shortened in the middle to keep both the top folder and the name.
    if (compact && !file.isEmpty()) {
        file = fm.elidedText(QDir::toNativeSeparators(QDir(root).relativeFilePath(file)), Qt::ElideMiddle,
                             kFileNameWidth);
    }
    if (!file.isEmpty()) file = QObject::tr("%1 (%2)").arg(file, formatSize(s.currentSize));

    const QString total = (s.totalIsEstimate ? QStringLiteral("~") : QString()) + formatCount(s.filesTotal);
    QString progress;
    if (s.reconcile) {
        progress = QObject::tr("проверка изменений: %1 из %2").arg(formatCount(s.filesVisited), total);
        if (s.filesWritten > 0) progress += QObject::tr(", обновлено %1").arg(formatCount(s.filesWritten));
    } else if (s.filesTotal > 0) {
        progress = QObject::tr("%1 из %2").arg(formatCount(s.filesVisited), total);
    } else {
        progress = QObject::tr("обработано %1").arg(formatCount(s.filesVisited));
    }
    QString failed;
    if (s.filesFailed > 0) {
        failed = (compact ? QObject::tr(", ошибок: %1") : QObject::tr(", пропущено из-за ошибок: %1"))
                     .arg(formatCount(s.filesFailed));
    }
    if (s.unreadableDirs > 0) {
        failed += (compact ? QObject::tr(", недоступных папок: %1")
                           : QObject::tr(", папок без доступа или с ошибкой чтения: %1"))
                      .arg(formatCount(s.unreadableDirs));
    }

    switch (s.phase) {
        case IndexPhase::Idle:
            return {};
        case IndexPhase::Upgrading:
            if (s.paused) return QObject::tr("%1: на паузе (обновление индекса для точного поиска)").arg(name);
            return QObject::tr("%1: обновление индекса для точного поиска — файлы не перечитываются")
                .arg(name);
        case IndexPhase::Counting:
            if (s.cancelRequested) return QObject::tr("%1: остановка…").arg(name);
            if (s.paused) return QObject::tr("%1: на паузе (подсчёт файлов: %2)").arg(name, formatCount(s.filesTotal));
            if (s.pauseRequested) return QObject::tr("%1: ставим на паузу…").arg(name);
            return s.heavyFound > 0
                       ? QObject::tr("%1: подсчёт файлов — найдено %2, из них крупных: %3…")
                             .arg(name, formatCount(s.filesTotal), formatCount(s.heavyFound))
                       : QObject::tr("%1: подсчёт файлов — найдено %2…").arg(name, formatCount(s.filesTotal));
        case IndexPhase::Indexing: {
            // The compact line leaves "N of M" and the percentage to the
            // progress bar, which already shows them.
            if (s.cancelRequested) return QObject::tr("%1: остановка, дожидаемся %2").arg(name, file);
            if (s.paused) {
                return compact ? QObject::tr("%1: на паузе%2").arg(name, failed)
                               : QObject::tr("%1: на паузе — %2%3").arg(name, progress, failed);
            }
            if (s.pauseRequested) return QObject::tr("%1: ставим на паузу, дожидаемся %2").arg(name, file);

            QStringList parts;
            if (!compact) {
                parts << progress + failed;
            } else if (s.reconcile && !s.processingHeavy) {
                parts << (s.filesWritten > 0
                              ? QObject::tr("проверка изменений, обновлено %1").arg(formatCount(s.filesWritten))
                              : QObject::tr("проверка изменений"));
            }

            if (s.processingHeavy) {
                // The last stretch: the heavy files set aside earlier, one by one.
                parts << QObject::tr("крупные файлы: %1 из %2 (%3 из %4)")
                             .arg(formatCount(s.heavyDone), formatCount(s.heavyFound), formatSize(s.heavyBytesDone),
                                  formatSize(s.heavyBytesTotal));
                if (!file.isEmpty()) parts << file;
            } else {
                if (!file.isEmpty()) parts << file;
                if (filesPerSecond >= 1) {
                    parts << QObject::tr("%1 файлов/с").arg(formatCount(static_cast<quint64>(filesPerSecond + 0.5)));
                }
                if (s.heavyFound > 0) {
                    parts << QObject::tr("крупные в конце: %1 (%2)")
                                 .arg(formatCount(s.heavyFound), formatSize(s.heavyBytesTotal));
                }
            }
            if (compact && !failed.isEmpty()) parts << failed.mid(2);
            return QObject::tr("%1: %2").arg(name, parts.join(QStringLiteral(" · ")));
        }
        case IndexPhase::Finishing:
            return s.reconcile ? QObject::tr("%1: удаление исчезнувших файлов и сохранение…").arg(name)
                               : QObject::tr("%1: сохранение индекса…").arg(name);
        case IndexPhase::Finished: {
            const qint64 took = duration_cast<seconds>(s.finishedAt - s.startedAt).count();
            if (s.cancelled) {
                return QObject::tr("%1: остановлено — обработано %2 из %3").arg(name, formatCount(s.filesVisited), total);
            }
            if (s.reconcile) {
                return QObject::tr("%1: изменения проверены за %2, обновлено %3%4")
                    .arg(name, formatDuration(took), formatCount(s.filesWritten), failed);
            }
            const QString heavy =
                s.heavyDone > 0 ? QObject::tr(", из них крупных: %1").arg(formatCount(s.heavyDone)) : QString();
            return QObject::tr("%1: готово за %2 — %3 файлов%4%5")
                .arg(name, formatDuration(took), formatCount(s.filesWritten), heavy, failed);
        }
    }
    return {};
}
} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(tr("DataSearch"));

    try {
        platform_ = datasearch::platform::createPlatformService();
    } catch (const std::exception& e) {
        QMessageBox::critical(
            this, tr("Ошибка"),
            tr("Не удалось инициализировать платформенный слой: %1").arg(e.what()));
    }

    indexManager_ = new IndexManager(platform_.get(), this);
    resultsModel_ = new ResultsTableModel(this);

    auto* central = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(central);

    auto* searchLayout = new QHBoxLayout();
    searchEdit_ = new QLineEdit(central);
    searchEdit_->setPlaceholderText(tr("Поиск по имени и содержимому файлов..."));
    searchEdit_->setToolTip(tr("Операторы: \"точная фраза\", -исключить, ext:docx, path:D:\\Work\\"));
    indexButton_ = new QPushButton(tr("Индексировать выбранные"), central);
    pauseResumeButton_ = new QPushButton(tr("Пауза"), central);
    wordFormsCheck_ = new QCheckBox(tr("С формами слов"), central);
    wordFormsCheck_->setToolTip(
        tr("Выключено — ищутся слова точно как введены (без учёта регистра): «Михайлов» не найдёт "
           "«Михайлова».\nВключено — ещё и другие формы слова и слова, начинающиеся так же."));
    searchLayout->addWidget(searchEdit_, 1);
    searchLayout->addWidget(wordFormsCheck_);
    searchLayout->addWidget(indexButton_);
    searchLayout->addWidget(pauseResumeButton_);
    rootLayout->addLayout(searchLayout);

    auto* settingsLayout = new QHBoxLayout();
    auto* excludeMasksLabel = new QLabel(tr("Исключить (маски через запятую):"), central);
    excludeMasksEdit_ = new QLineEdit(central);
    excludeMasksEdit_->setPlaceholderText(kDefaultExcludeMasks);
    settingsLayout->addWidget(excludeMasksLabel);
    settingsLayout->addWidget(excludeMasksEdit_, 1);
    rootLayout->addLayout(settingsLayout);

    auto* splitter = new QSplitter(Qt::Horizontal, central);

    auto* sourcesPanel = new QWidget(splitter);
    auto* sourcesLayout = new QVBoxLayout(sourcesPanel);
    sourcesLayout->setContentsMargins(0, 0, 0, 0);
    addFolderButton_ = new QPushButton(tr("Добавить папку..."), sourcesPanel);
    sourcesLayout->addWidget(addFolderButton_);
    volumeList_ = new QListWidget(sourcesPanel);
    sourcesLayout->addWidget(volumeList_, 1);
    splitter->addWidget(sourcesPanel);

    resultsView_ = new QTableView(splitter);
    resultsView_->setModel(resultsModel_);
    resultsView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    resultsView_->setSelectionMode(QAbstractItemView::SingleSelection);
    resultsView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    resultsView_->horizontalHeader()->setStretchLastSection(true);
    resultsView_->setContextMenuPolicy(Qt::CustomContextMenu);
    splitter->addWidget(resultsView_);
    splitter->setStretchFactor(1, 1);

    rootLayout->addWidget(splitter, 1);

    // An always-moving strip (stops only while paused) so the window visibly
    // stays alive through a long step, next to the share of files done.
    progressRow_ = new QWidget(central);
    auto* progressLayout = new QHBoxLayout(progressRow_);
    progressLayout->setContentsMargins(0, 0, 0, 0);
    busyBar_ = new QProgressBar(progressRow_);
    busyBar_->setRange(0, 0);
    busyBar_->setTextVisible(false);
    busyBar_->setFixedWidth(120);
    progressBar_ = new QProgressBar(progressRow_);
    progressBar_->setRange(0, 1000);
    progressBar_->setAlignment(Qt::AlignCenter);
    // Time lives on the bar: elapsed on the left (from zero, pauses not
    // counted), the estimate of what's left on the right.
    auto* barLayout = new QHBoxLayout(progressBar_);
    barLayout->setContentsMargins(10, 0, 10, 0);
    elapsedLabel_ = new QLabel(progressBar_);
    etaLabel_ = new QLabel(progressBar_);
    for (QLabel* label : {elapsedLabel_, etaLabel_}) {
        label->setStyleSheet(QStringLiteral("background: transparent; font-weight: 600;"));
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    barLayout->addWidget(elapsedLabel_);
    barLayout->addStretch(1);
    barLayout->addWidget(etaLabel_);
    progressLayout->addWidget(busyBar_);
    progressLayout->addWidget(progressBar_, 1);
    progressRow_->setVisible(false);
    rootLayout->addWidget(progressRow_);

    setCentralWidget(central);

    // Ignored horizontal policy: a long path in the text must never force the
    // window wider — the label just clips, and the full text is its tooltip.
    indexStatusLabel_ = new QLabel(this);
    indexStatusLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusBar()->addWidget(indexStatusLabel_, 1);
    statusLabel_ = new QLabel(this);
    statusBar()->addPermanentWidget(statusLabel_);

    resize(1100, 700);

    searchDebounce_.setSingleShot(true);
    searchDebounce_.setInterval(250);

    connect(searchEdit_, &QLineEdit::textChanged, this, &MainWindow::onSearchTextChanged);
    connect(&searchDebounce_, &QTimer::timeout, this, &MainWindow::runSearch);
    connect(indexButton_, &QPushButton::clicked, this, &MainWindow::onIndexSelectedClicked);
    connect(indexManager_, &IndexManager::finished, this, &MainWindow::onIndexFinished);
    clock_.start();
    indexStatusTimer_.setInterval(300);
    connect(&indexStatusTimer_, &QTimer::timeout, this, &MainWindow::updateIndexingStatus);
    indexStatusTimer_.start();
    connect(resultsView_, &QTableView::customContextMenuRequested, this,
            &MainWindow::onResultsContextMenuRequested);
    connect(resultsView_, &QTableView::doubleClicked, this, &MainWindow::onResultDoubleClicked);
    connect(indexManager_, &IndexManager::watcherActivity, this, &MainWindow::onWatcherActivity);
    connect(indexManager_, &IndexManager::sourceUnavailable, this, &MainWindow::onSourceUnavailable);
    connect(pauseResumeButton_, &QPushButton::clicked, this, &MainWindow::onPauseResumeClicked);
    connect(addFolderButton_, &QPushButton::clicked, this, &MainWindow::onAddFolderClicked);
    connect(excludeMasksEdit_, &QLineEdit::editingFinished, this, &MainWindow::onExcludeMasksEdited);
    wordFormsCheck_->setChecked(QSettings().value(kSettingsWordFormsKey, false).toBool());
    connect(wordFormsCheck_, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(kSettingsWordFormsKey, on);
        runSearch();
    });
    snippetDebounce_.setSingleShot(true);
    snippetDebounce_.setInterval(80);
    connect(&snippetDebounce_, &QTimer::timeout, this, &MainWindow::requestVisibleSnippets);
    connect(resultsView_->verticalScrollBar(), &QScrollBar::valueChanged, &snippetDebounce_,
            qOverload<>(&QTimer::start));
    connect(resultsView_->verticalScrollBar(), &QScrollBar::rangeChanged, &snippetDebounce_,
            qOverload<>(&QTimer::start));

    {
        QSettings settings;
        const QString saved = settings.value(kSettingsExcludeMasksKey, kDefaultExcludeMasks).toString();
        excludeMasksEdit_->setText(saved);
    }
    onExcludeMasksEdited();

    populateVolumes();

    // Reopen sources from a previous session instantly, then reconcile them
    // in the background (ТЗ п.13.1/FR-23) — search works right away below.
    indexManager_->loadKnownSources();
    runSearch();
}

void MainWindow::populateVolumes() {
    volumeList_->clear();
    if (!platform_) return;

    const auto knownRoots = indexManager_->knownRoots();
    const std::set<std::string> knownRootsStd(knownRoots.begin(), knownRoots.end());
    QSet<QString> addedRoots;

    std::vector<datasearch::platform::VolumeInfo> volumes;
    try {
        volumes = platform_->enumerateVolumes();
    } catch (const std::exception& e) {
        QMessageBox::warning(this, tr("Ошибка"),
                              tr("Не удалось получить список дисков: %1").arg(e.what()));
        return;
    }

    for (const auto& volume : volumes) {
        const QString root = QString::fromStdString(volume.rootPath);

        QString typeLabel;
        switch (volume.type) {
            case VolumeType::Local: typeLabel = tr("локальный"); break;
            case VolumeType::Removable: typeLabel = tr("съёмный"); break;
            case VolumeType::Network: typeLabel = tr("сетевой"); break;
            default: typeLabel = tr("неизвестно"); break;
        }

        const double freeGb = static_cast<double>(volume.freeBytes) / (1024.0 * 1024.0 * 1024.0);
        const double totalGb = static_cast<double>(volume.totalBytes) / (1024.0 * 1024.0 * 1024.0);
        const QString label = volume.label.empty()
                                   ? root
                                   : QString("%1 (%2)").arg(root, QString::fromStdString(volume.label));
        const QString text = QString("%1 — %2, свободно %3 из %4 ГБ")
                                  .arg(label, typeLabel)
                                  .arg(freeGb, 0, 'f', 1)
                                  .arg(totalGb, 0, 'f', 1);

        // Sources reopened from a previous session (ТЗ п.13.1) start checked,
        // so their already-indexed results show up without the user having
        // to re-select them.
        const bool known = knownRootsStd.count(volume.rootPath) != 0;
        addSourceItem(root, text, known);
        addedRoots.insert(root);
    }

    // Manually added folders (see onAddFolderClicked) don't show up in
    // enumerateVolumes(), but if one was indexed in a previous session it
    // should still be offered — otherwise its index becomes unreachable
    // from the UI after a restart even though the data is still there.
    for (const auto& root : knownRootsStd) {
        const QString qRoot = QString::fromStdString(root);
        if (addedRoots.contains(qRoot)) continue;
        addSourceItem(qRoot, qRoot, /*checked=*/true);
    }
}

void MainWindow::addSourceItem(const QString& root, const QString& text, bool checked) {
    auto* item = new QListWidgetItem(text, volumeList_);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    item->setData(Qt::UserRole, root);
}

QStringList MainWindow::checkedRoots() const {
    QStringList roots;
    for (int i = 0; i < volumeList_->count(); ++i) {
        const QListWidgetItem* item = volumeList_->item(i);
        if (item->checkState() == Qt::Checked) {
            roots.append(item->data(Qt::UserRole).toString());
        }
    }
    return roots;
}

void MainWindow::onSearchTextChanged(const QString&) {
    searchDebounce_.start();
}

void MainWindow::runSearch() {
    const QStringList roots = checkedRoots();
    if (roots.isEmpty()) {
        resultsModel_->setRecords({});
        showNotice(tr("Выберите хотя бы один диск для поиска"));
        return;
    }

    std::vector<std::string> stdRoots;
    stdRoots.reserve(static_cast<std::size_t>(roots.size()));
    for (const QString& r : roots) stdRoots.push_back(r.toStdString());

    SearchQuery query;
    query.namePattern = searchEdit_->text().toStdString();
    // Relevance (BM25, name weighted above content, ТЗ п.8) is SearchQuery's
    // default sort — matches Elasticsearch-style ranked results; browsing
    // with an empty query naturally falls back to a plain name-sorted list
    // (see IndexStorage::search).
    query.exactWords = !wordFormsCheck_->isChecked();
    query.withSnippets = false;  // fetched per visible row, see requestVisibleSnippets()
    query.limit = kResultLimit;
    query.offset = 0;

    showNotice(tr("Идёт поиск..."));

    // Runs off the GUI thread (ТЗ NFR-7) — a broad query (e.g. a single
    // character) against a large index can take a while; without this the
    // whole window would freeze until it finished.
    const QString requestText = searchEdit_->text();
    const QStringList requestRoots = roots;
    const bool requestExact = query.exactWords;
    indexManager_->searchAsync(
        query, stdRoots, this,
        [this, requestText, requestRoots, requestExact](std::vector<datasearch::core::FileRecord> results,
                                                        std::uint64_t total) {
            // The user may have kept typing (or changed the selected
            // sources or the mode) while this search was running — a newer
            // runSearch() call already queued a fresher request, so these
            // results are stale; drop them rather than briefly flashing
            // outdated data.
            if (searchEdit_->text() != requestText || checkedRoots() != requestRoots ||
                wordFormsCheck_->isChecked() == requestExact) {
                return;
            }
            if (total == 0 && requestExact && !requestText.trimmed().isEmpty()) {
                showNotice(tr("Ничего не найдено: ищутся слова целиком, как введены. "
                              "Для форм слов и начала слова включите «С формами слов»"));
            } else if (total > results.size()) {
                showNotice(tr("Найдено файлов: %1 (показаны первые %2)")
                               .arg(formatCount(total), formatCount(results.size())));
            } else {
                showNotice(tr("Найдено файлов: %1").arg(formatCount(total)));
            }
            ++resultsGeneration_;
            shownPattern_ = requestText;
            shownExact_ = requestExact;
            snippetRequested_.assign(results.size(), false);
            resultsModel_->setRecords(std::move(results));
            requestVisibleSnippets();
        });
}

void MainWindow::requestVisibleSnippets() {
    const int rows = resultsModel_->rowCount();
    if (rows == 0 || shownPattern_.trimmed().isEmpty() || static_cast<int>(snippetRequested_.size()) != rows) return;

    int first = resultsView_->rowAt(0);
    int last = resultsView_->rowAt(resultsView_->viewport()->height() - 1);
    if (first < 0) first = 0;
    if (last < 0) last = rows - 1;
    last = std::min(rows - 1, last + 5);  // a few below the fold, so scrolling a little finds them ready

    std::vector<std::pair<int, std::string>> wanted;
    for (int row = first; row <= last; ++row) {
        if (snippetRequested_[row]) continue;
        snippetRequested_[row] = true;
        wanted.emplace_back(row, resultsModel_->recordAt(row).path);
    }
    if (wanted.empty()) return;

    SearchQuery query;
    query.namePattern = shownPattern_.toStdString();
    query.exactWords = shownExact_;
    const quint64 generation = resultsGeneration_;
    indexManager_->snippetsAsync(query, std::move(wanted), this,
                                 [this, generation](int row, std::string path, std::string snippet) {
                                     if (generation != resultsGeneration_) return;
                                     resultsModel_->setSnippet(row, path, snippet);
                                 });
}

void MainWindow::onIndexSelectedClicked() {
    const QStringList roots = checkedRoots();
    if (roots.isEmpty()) {
        QMessageBox::information(this, tr("Индексация"),
                                  tr("Сначала отметьте один или несколько дисков."));
        return;
    }

    std::vector<std::string> stdRoots;
    stdRoots.reserve(static_cast<std::size_t>(roots.size()));
    for (const QString& r : roots) stdRoots.push_back(r.toStdString());

    const auto alreadyRunning = indexManager_->indexRoots(stdRoots);
    if (!alreadyRunning.empty()) {
        QStringList names;
        for (const auto& root : alreadyRunning) names << shortRootName(QString::fromStdString(root));
        showNotice(tr("Уже индексируется: %1 — продолжаем").arg(names.join(", ")));
    }
    updateIndexingStatus();
}

void MainWindow::onIndexFinished(const QString&, bool) {
    updateIndexingStatus();
    runSearch();
}

void MainWindow::updateIndexingStatus() {
    const auto statuses = indexManager_->indexingStatus();
    const qint64 nowMs = clock_.elapsed();
    const QFontMetrics fm(indexStatusLabel_->font());

    QStringList lines;
    QStringList fullLines;
    const IndexManager::RootStatus* lastFinished = nullptr;
    bool anyActive = false;
    bool anyPauseRequested = false;
    bool allPaused = true;
    bool anyFinishing = false;
    quint64 visitedSum = 0;
    quint64 totalSum = 0;
    double etaSeconds = -1;

    for (const auto& entry : statuses) {
        const IndexerStatus& s = entry.status;

        // Files/second, smoothed over ~1 s windows. Time spent paused (or
        // waiting for a pause to take effect) is excluded from the rate.
        RateSample& rate = rates_[entry.root];
        const bool counted = s.phase == IndexPhase::Indexing || s.phase == IndexPhase::Upgrading;
        if (!counted || s.pauseRequested) {
            if (!counted) rate.filesPerSecond = 0;
            rate.visited = s.filesVisited;
            rate.atMs = nowMs;
        } else if (rate.atMs < 0 || s.filesVisited < rate.visited) {
            rate.visited = s.filesVisited;
            rate.atMs = nowMs;
            rate.filesPerSecond = 0;
        } else if (nowMs - rate.atMs >= 1000) {
            const double instant = static_cast<double>(s.filesVisited - rate.visited) * 1000.0 / (nowMs - rate.atMs);
            rate.filesPerSecond = rate.filesPerSecond == 0 ? instant : 0.7 * rate.filesPerSecond + 0.3 * instant;
            rate.visited = s.filesVisited;
            rate.atMs = nowMs;
        }

        const bool active = s.phase == IndexPhase::Upgrading || s.phase == IndexPhase::Counting ||
                            s.phase == IndexPhase::Indexing || s.phase == IndexPhase::Finishing;
        const QString root = QString::fromStdString(entry.root);
        if (active) {
            anyActive = true;
            anyPauseRequested = anyPauseRequested || s.pauseRequested;
            lines << describeIndexing(root, s, rate.filesPerSecond, fm, true);
            fullLines << describeIndexing(root, s, rate.filesPerSecond, fm, false);
            allPaused = allPaused && s.paused;
            anyFinishing = anyFinishing || s.phase == IndexPhase::Finishing;
            etaSeconds = std::max(etaSeconds, secondsLeft(s, rate.filesPerSecond));
            visitedSum += std::min(s.filesVisited, s.filesTotal);
            totalSum += s.filesTotal;
        } else if (s.phase == IndexPhase::Finished &&
                   (lastFinished == nullptr || s.finishedAt > lastFinished->status.finishedAt)) {
            lastFinished = &entry;
        }
    }
    if (!anyActive && lastFinished != nullptr) {
        const QString root = QString::fromStdString(lastFinished->root);
        lines << describeIndexing(root, lastFinished->status, 0, fm, true);
        fullLines << describeIndexing(root, lastFinished->status, 0, fm, false);
    }

    indexStatusLabel_->setText(lines.join(QStringLiteral("   |   ")));
    indexStatusLabel_->setToolTip(fullLines.join('\n'));

    progressRow_->setVisible(anyActive);
    if (!anyActive) {
        activeElapsedMs_ = 0;
        lastElapsedTickMs_ = -1;
    } else {
        if (lastElapsedTickMs_ >= 0 && !allPaused) activeElapsedMs_ += nowMs - lastElapsedTickMs_;
        lastElapsedTickMs_ = nowMs;
        elapsedLabel_->setText(formatClock(activeElapsedMs_ / 1000));
        etaLabel_->setText(etaSeconds >= 1 ? tr("осталось ~%1").arg(formatClock(static_cast<qint64>(etaSeconds + 0.5)))
                                           : QString());

        // Range (0,0) makes the style animate the strip; a fixed range freezes it.
        if (allPaused) {
            busyBar_->setRange(0, 1);
            busyBar_->setValue(0);
        } else {
            busyBar_->setRange(0, 0);
        }
        const int value = anyFinishing ? 1000
                          : totalSum == 0 ? 0
                                          : static_cast<int>(1000.0 * static_cast<double>(visitedSum) / totalSum);
        progressBar_->setValue(value);
        progressBar_->setFormat(totalSum == 0 ? QStringLiteral("%p%")
                                              : tr("%1 из %2 — %p%").arg(formatCount(visitedSum), formatCount(totalSum)));
        // A style sheet (rather than the native look) so the text is drawn on
        // the bar on every platform — macOS's native bar shows no text.
        // Re-applied only when the colour actually changes.
        const QString sheet = QStringLiteral(
                                  "QProgressBar { border: 1px solid palette(mid); border-radius: 6px;"
                                  " background: palette(base); min-height: 18px; font-weight: 600; }"
                                  "QProgressBar::chunk { border-radius: 5px; background-color: %1; }")
                                  .arg(progressColor(value / 1000.0).name());
        if (progressBar_->styleSheet() != sheet) progressBar_->setStyleSheet(sheet);
    }
    pauseResumeButton_->setEnabled(anyActive);
    pauseResumeButton_->setText(anyPauseRequested ? tr("Продолжить") : tr("Пауза"));
}

void MainWindow::onResultsContextMenuRequested(const QPoint& pos) {
    const QModelIndex index = resultsView_->indexAt(pos);
    if (!index.isValid()) return;
    const int row = index.row();

    QMenu menu(this);
    QAction* openAction = menu.addAction(tr("Открыть"));
    QAction* showAction = menu.addAction(tr("Показать в папке"));
    QAction* copyAction = menu.addAction(tr("Копировать путь"));

    QAction* chosen = menu.exec(resultsView_->viewport()->mapToGlobal(pos));
    if (chosen == openAction) {
        openRow(row);
    } else if (chosen == showAction) {
        showRowInFolder(row);
    } else if (chosen == copyAction) {
        copyRowPath(row);
    }
}

void MainWindow::onResultDoubleClicked(const QModelIndex& index) {
    if (index.isValid()) openRow(index.row());
}

void MainWindow::openRow(int row) {
    if (!platform_) return;
    const auto& record = resultsModel_->recordAt(row);
    try {
        platform_->openFile(datasearch::core::pathFromUtf8(record.path));
    } catch (const std::exception& e) {
        QMessageBox::warning(this, tr("Ошибка"), tr("Не удалось открыть файл: %1").arg(e.what()));
    }
}

void MainWindow::showRowInFolder(int row) {
    if (!platform_) return;
    const auto& record = resultsModel_->recordAt(row);
    try {
        platform_->showInFolder(datasearch::core::pathFromUtf8(record.path));
    } catch (const std::exception& e) {
        QMessageBox::warning(this, tr("Ошибка"),
                              tr("Не удалось показать файл в папке: %1").arg(e.what()));
    }
}

void MainWindow::copyRowPath(int row) {
    const auto& record = resultsModel_->recordAt(row);
    QGuiApplication::clipboard()->setText(QString::fromStdString(record.path));
}

void MainWindow::showNotice(const QString& text) {
    statusLabel_->setText(QFontMetrics(statusLabel_->font()).elidedText(text, Qt::ElideRight, kNoticeWidth));
    statusLabel_->setToolTip(text);
}

void MainWindow::onWatcherActivity(const QString& rootLabel, const QString& description) {
    const QString text = QString("%1: %2").arg(rootLabel, description);
    showNotice(text);
}

void MainWindow::onSourceUnavailable(const QString& rootLabel) {
    // ТЗ п.11.4: source (e.g. a disconnected network drive) is unreachable
    // right now — its existing index is untouched and still searchable, this
    // is just a notice that results may be stale until it's back.
    showNotice(
        tr("%1: диск недоступен, показаны данные последней индексации").arg(rootLabel));
}

std::vector<std::string> MainWindow::parseExcludeMasks() const {
    std::vector<std::string> masks;
    const QStringList parts = excludeMasksEdit_->text().split(',', Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        const QString trimmed = part.trimmed();
        if (!trimmed.isEmpty()) masks.push_back(trimmed.toStdString());
    }
    return masks;
}

void MainWindow::onExcludeMasksEdited() {
    const auto masks = parseExcludeMasks();
    indexManager_->setExcludeMasks(masks);

    QSettings settings;
    settings.setValue(kSettingsExcludeMasksKey, excludeMasksEdit_->text());
}

void MainWindow::onPauseResumeClicked() {
    if (indexManager_->isAnyIndexingPaused()) {
        indexManager_->resumeAllIndexing();
    } else {
        indexManager_->pauseAllIndexing();
    }
    updateIndexingStatus();
}

void MainWindow::onAddFolderClicked() {
    const QString dir =
        QFileDialog::getExistingDirectory(this, tr("Выбрать папку для индексации"), QDir::homePath());
    if (dir.isEmpty()) return;

    for (int i = 0; i < volumeList_->count(); ++i) {
        QListWidgetItem* item = volumeList_->item(i);
        if (item->data(Qt::UserRole).toString() == dir) {
            item->setCheckState(Qt::Checked);
            runSearch();
            return;
        }
    }

    addSourceItem(dir, dir, /*checked=*/true);
    runSearch();
}
