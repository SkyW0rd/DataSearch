#include "MainWindow.h"

#include "IndexManager.h"
#include "ResultsTableModel.h"

#include "datasearch/core/IndexStorage.h"
#include "datasearch/core/Utf8.h"

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
#include <QSplitter>
#include <QStatusBar>
#include <QTableView>
#include <QVBoxLayout>

using datasearch::core::SearchQuery;
using datasearch::core::SortField;
using datasearch::core::SortOrder;
using datasearch::platform::VolumeType;

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(tr("DataSearch"));

    try {
        platform_ = datasearch::platform::createPlatformService();
    } catch (const std::exception& e) {
        QMessageBox::critical(
            this, tr("Ошибка"),
            tr("Не удалось инициализировать платформенный слой: %1").arg(e.what()));
    }

    indexManager_ = new IndexManager(this);
    resultsModel_ = new ResultsTableModel(this);

    auto* central = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(central);

    auto* searchLayout = new QHBoxLayout();
    searchEdit_ = new QLineEdit(central);
    searchEdit_->setPlaceholderText(tr("Поиск по имени файла..."));
    indexButton_ = new QPushButton(tr("Индексировать выбранные"), central);
    searchLayout->addWidget(searchEdit_, 1);
    searchLayout->addWidget(indexButton_);
    rootLayout->addLayout(searchLayout);

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

    populateVolumes();
}

void MainWindow::populateVolumes() {
    volumeList_->clear();
    if (!platform_) return;

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
        item->setCheckState(Qt::Unchecked);
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
    query.sortField = SortField::Name;
    query.sortOrder = SortOrder::Ascending;
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
