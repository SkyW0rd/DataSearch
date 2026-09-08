#include "MainWindow.h"

#include "IndexManager.h"
#include "ResultsTableModel.h"

#include "datasearch/core/IndexStorage.h"
#include "datasearch/core/Utf8.h"

#include <set>

#include <QAbstractItemView>
#include <QAction>
#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTableView>
#include <QVBoxLayout>

using datasearch::core::SearchQuery;
using datasearch::platform::VolumeType;

namespace {
const char* kSettingsExcludeMasksKey = "excludeMasks";
const char* kDefaultExcludeMasks = "*.tmp, node_modules, .git";
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
    searchLayout->addWidget(searchEdit_, 1);
    searchLayout->addWidget(indexButton_);
    rootLayout->addLayout(searchLayout);

    auto* settingsLayout = new QHBoxLayout();
    auto* excludeMasksLabel = new QLabel(tr("Исключить (маски через запятую):"), central);
    excludeMasksEdit_ = new QLineEdit(central);
    excludeMasksEdit_->setPlaceholderText(kDefaultExcludeMasks);
    settingsLayout->addWidget(excludeMasksLabel);
    settingsLayout->addWidget(excludeMasksEdit_, 1);
    rootLayout->addLayout(settingsLayout);

    auto* splitter = new QSplitter(Qt::Horizontal, central);

    volumeList_ = new QListWidget(splitter);
    splitter->addWidget(volumeList_);

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

    progressBar_ = new QProgressBar(central);
    progressBar_->setRange(0, 0);
    progressBar_->setVisible(false);
    rootLayout->addWidget(progressBar_);

    setCentralWidget(central);

    statusLabel_ = new QLabel(this);
    statusBar()->addWidget(statusLabel_, 1);

    resize(1100, 700);

    searchDebounce_.setSingleShot(true);
    searchDebounce_.setInterval(250);

    connect(searchEdit_, &QLineEdit::textChanged, this, &MainWindow::onSearchTextChanged);
    connect(&searchDebounce_, &QTimer::timeout, this, &MainWindow::runSearch);
    connect(indexButton_, &QPushButton::clicked, this, &MainWindow::onIndexSelectedClicked);
    connect(indexManager_, &IndexManager::progress, this, &MainWindow::onIndexProgress);
    connect(indexManager_, &IndexManager::finished, this, &MainWindow::onIndexFinished);
    connect(resultsView_, &QTableView::customContextMenuRequested, this,
            &MainWindow::onResultsContextMenuRequested);
    connect(resultsView_, &QTableView::doubleClicked, this, &MainWindow::onResultDoubleClicked);
    connect(indexManager_, &IndexManager::watcherActivity, this, &MainWindow::onWatcherActivity);
    connect(indexManager_, &IndexManager::sourceUnavailable, this, &MainWindow::onSourceUnavailable);
    connect(excludeMasksEdit_, &QLineEdit::editingFinished, this, &MainWindow::onExcludeMasksEdited);

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

        auto* item = new QListWidgetItem(text, volumeList_);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        // Sources reopened from a previous session (ТЗ п.13.1) start checked,
        // so their already-indexed results show up without the user having
        // to re-select them.
        const bool known = knownRootsStd.count(volume.rootPath) != 0;
        item->setCheckState(known ? Qt::Checked : Qt::Unchecked);
        item->setData(Qt::UserRole, root);
    }
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
        statusLabel_->setText(tr("Выберите хотя бы один диск для поиска"));
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
    query.limit = 500;
    query.offset = 0;

    auto results = indexManager_->search(query, stdRoots);
    statusLabel_->setText(tr("Найдено файлов: %1").arg(results.size()));
    resultsModel_->setRecords(std::move(results));
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

    progressBar_->setVisible(true);
    statusLabel_->setText(tr("Индексация запущена..."));
    indexManager_->indexRoots(stdRoots);
}

void MainWindow::onIndexProgress(quint64 filesIndexed, const QString& currentPath,
                                  const QString& rootLabel) {
    statusLabel_->setText(
        tr("%1: проиндексировано %2 файлов (%3)").arg(rootLabel).arg(filesIndexed).arg(currentPath));
}

void MainWindow::onIndexFinished(const QString& rootLabel, bool cancelled) {
    progressBar_->setVisible(false);
    statusLabel_->setText(cancelled ? tr("%1: индексация отменена").arg(rootLabel)
                                     : tr("%1: индексация завершена").arg(rootLabel));
    runSearch();
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

void MainWindow::onWatcherActivity(const QString& rootLabel, const QString& description) {
    statusLabel_->setText(QString("%1: %2").arg(rootLabel, description));
}

void MainWindow::onSourceUnavailable(const QString& rootLabel) {
    // ТЗ п.11.4: source (e.g. a disconnected network drive) is unreachable
    // right now — its existing index is untouched and still searchable, this
    // is just a notice that results may be stale until it's back.
    statusLabel_->setText(
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
