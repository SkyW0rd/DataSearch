#include "ProblemFilesDialog.h"

#include "Format.h"

#include "datasearch/core/Utf8.h"

#include <QAbstractItemView>
#include <QAction>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <ctime>
#include <map>
#include <set>

using datasearch::core::ExtractionProblem;
using namespace display;

namespace {

// In the order the summary lists them.
const ExtractionProblem kReasons[] = {ExtractionProblem::ImagesOnly, ExtractionProblem::Damaged,
                                      ExtractionProblem::Protected,  ExtractionProblem::CannotOpen,
                                      ExtractionProblem::TooLarge,   ExtractionProblem::Failed};

QString reasonTitle(ExtractionProblem problem) {
    switch (problem) {
        case ExtractionProblem::CannotOpen: return QObject::tr("Не удалось открыть");
        case ExtractionProblem::Damaged: return QObject::tr("Повреждены");
        case ExtractionProblem::Protected: return QObject::tr("Защищены паролем");
        case ExtractionProblem::TooLarge: return QObject::tr("Слишком большие");
        case ExtractionProblem::Failed: return QObject::tr("Ошибка при чтении");
        case ExtractionProblem::ImagesOnly: return QObject::tr("Сканы без текста");
        default: return QObject::tr("Другое");
    }
}

QString reasonHint(ExtractionProblem problem) {
    switch (problem) {
        case ExtractionProblem::CannotOpen:
            return QObject::tr("Нет доступа, файл был занят другой программой или диск не прочитал его.\n"
                               "Если файл освободился — проиндексируйте заново.");
        case ExtractionProblem::Damaged:
            return QObject::tr("Файл повреждён или на самом деле другого формата, чем говорит расширение\n"
                               "(например, старый .doc, переименованный в .docx).");
        case ExtractionProblem::Protected:
            return QObject::tr("Для открытия нужен пароль — без него текст не прочитать.\n"
                               "PDF, которые открываются без пароля и только запрещают копирование или печать,\n"
                               "программа читает сама.");
        case ExtractionProblem::ImagesOnly:
            return QObject::tr("Страницы — картинки (скан или фотография), текстового слоя в файле нет.\n"
                               "По имени такие файлы находятся; чтобы искать по их тексту, нужно распознавание (OCR),\n"
                               "которого в программе пока нет. Повторная индексация не поможет.");
        case ExtractionProblem::TooLarge:
            return QObject::tr("Текст таких файлов не читается, чтобы не занимать слишком много памяти:\n"
                               "текстовые файлы и PDF больше 100 МБ, документы Word и Excel с текстом больше 512 МБ.\n"
                               "Повторная индексация не поможет, пока файл не станет меньше.");
        case ExtractionProblem::Failed:
            return QObject::tr("Чтение прервалось с ошибкой — например, не хватило памяти.\n"
                               "Может помочь повторная индексация, когда памяти будет свободнее.");
        default: return {};
    }
}

QString reasonForRow(const datasearch::core::ProblemFile& file) {
    QString text = reasonTitle(file.problem);
    if (file.problem == ExtractionProblem::Failed && !file.detail.empty()) {
        const bool memory = file.detail.find("bad_alloc") != std::string::npos;
        text += QObject::tr(" (%1)").arg(memory ? QObject::tr("не хватило памяти") : QString::fromStdString(file.detail));
    }
    return text;
}

// Sorts by the byte count, not by its text ("900 КБ" vs "1,2 МБ").
class SizeItem : public QTableWidgetItem {
public:
    SizeItem(quint64 bytes) : QTableWidgetItem(formatSize(bytes)), bytes_(bytes) {}
    bool operator<(const QTableWidgetItem& other) const override {
        const auto* size = dynamic_cast<const SizeItem*>(&other);
        return size != nullptr ? bytes_ < size->bytes_ : QTableWidgetItem::operator<(other);
    }

private:
    quint64 bytes_;
};

// "1 файл", "2 файла", "5 файлов".
QString filesCount(quint64 n) {
    const quint64 lastTwo = n % 100;
    const quint64 last = n % 10;
    const char* word = (lastTwo >= 11 && lastTwo <= 14) ? "файлов" : last == 1 ? "файл" : (last >= 2 && last <= 4) ? "файла" : "файлов";
    return QStringLiteral("%1 %2").arg(formatCount(n), QString::fromUtf8(word));
}

} // namespace

ProblemFilesDialog::ProblemFilesDialog(IndexManager* manager, datasearch::platform::IPlatformService* platform,
                                       QWidget* parent)
    : QDialog(parent), manager_(manager), platform_(platform) {
    setWindowTitle(tr("Файлы, которые не удалось прочитать"));
    resize(920, 560);

    auto* layout = new QVBoxLayout(this);
    intro_ = new QLabel(this);
    intro_->setWordWrap(true);
    layout->addWidget(intro_);

    auto* summaryBox = new QWidget(this);
    summary_ = new QGridLayout(summaryBox);
    summary_->setContentsMargins(0, 6, 0, 6);
    summary_->setColumnStretch(0, 1);
    layout->addWidget(summaryBox);

    listBox_ = new QWidget(this);
    auto* listLayout = new QVBoxLayout(listBox_);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listTitle_ = new QLabel(listBox_);
    listLayout->addWidget(listTitle_);
    table_ = new QTableWidget(0, 4, listBox_);
    table_->setHorizontalHeaderLabels({tr("Имя"), tr("Путь"), tr("Размер"), tr("Причина")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setWordWrap(false);
    table_->setTextElideMode(Qt::ElideMiddle);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->horizontalHeader()->resizeSection(0, 220);
    table_->horizontalHeader()->resizeSection(1, 380);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    // Listed in folder order, as they come; a header click sorts by a column.
    table_->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
    listLayout->addWidget(table_, 1);
    auto* listButtons = new QHBoxLayout();
    auto* openButton = new QPushButton(tr("Открыть"), listBox_);
    auto* folderButton = new QPushButton(tr("Показать в папке"), listBox_);
    retrySelected_ = new QPushButton(tr("Проиндексировать выбранные заново"), listBox_);
    QPushButton* retrySelected = retrySelected_;
    listButtons->addWidget(openButton);
    listButtons->addWidget(folderButton);
    listButtons->addStretch(1);
    listButtons->addWidget(retrySelected);
    listLayout->addLayout(listButtons);
    listBox_->setVisible(false);
    layout->addWidget(listBox_, 1);

    status_ = new QLabel(this);
    status_->setWordWrap(true);
    layout->addWidget(status_);
    layout->addStretch(0);

    auto* buttons = new QDialogButtonBox(this);
    QPushButton* close = buttons->addButton(tr("Закрыть"), QDialogButtonBox::RejectRole);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    // Enter closes the window rather than opening files.
    for (QPushButton* button : {openButton, folderButton, retrySelected}) button->setAutoDefault(false);
    close->setDefault(true);
    layout->addWidget(buttons);

    connect(openButton, &QPushButton::clicked, this, &ProblemFilesDialog::openSelected);
    connect(folderButton, &QPushButton::clicked, this, &ProblemFilesDialog::showSelectedInFolder);
    connect(retrySelected, &QPushButton::clicked, this, [this] { retry(selectedIndexes()); });
    connect(table_, &QTableWidget::cellDoubleClicked, this, &ProblemFilesDialog::openSelected);
    connect(table_, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        if (!table_->indexAt(pos).isValid()) return;
        QMenu menu(this);
        QAction* open = menu.addAction(tr("Открыть"));
        QAction* folder = menu.addAction(tr("Показать в папке"));
        QAction* copy = menu.addAction(tr("Копировать путь"));
        menu.addSeparator();
        QAction* again = menu.addAction(tr("Проиндексировать заново"));
        QAction* chosen = menu.exec(table_->viewport()->mapToGlobal(pos));
        if (chosen == open) openSelected();
        if (chosen == folder) showSelectedInFolder();
        if (chosen == again) retry(selectedIndexes());
        if (chosen == copy) {
            QStringList paths;
            for (std::size_t i : selectedIndexes()) paths << QDir::toNativeSeparators(QString::fromStdString(files_[i].file.path));
            QGuiApplication::clipboard()->setText(paths.join('\n'));
        }
    });
    connect(manager_, &IndexManager::problemFilesChanged, this, &ProblemFilesDialog::reload);

    reload();
}

void ProblemFilesDialog::reload() {
    files_ = manager_->problemFiles();
    if (!retrying_.empty()) {
        std::map<std::pair<std::string, std::string>, std::int64_t> listed;
        for (const auto& entry : files_) listed[{entry.root, entry.file.path}] = entry.file.seenAt;
        std::size_t waiting = 0, stillFailing = 0;
        for (const auto& key : retrying_) {
            const auto it = listed.find(key);
            if (it == listed.end()) continue;  // read fine this time
            if (it->second < retryStartedAt_) ++waiting;
            else ++stillFailing;
        }
        if (waiting == 0) {
            const std::size_t fixed = retrying_.size() - stillFailing;
            if (stillFailing == 0) {
                status_->setText(tr("Готово: текст прочитан — %1.").arg(filesCount(fixed)));
            } else if (fixed == 0) {
                status_->setText(tr("Готово: по-прежнему не читаются — %1.").arg(filesCount(stillFailing)));
            } else {
                status_->setText(tr("Готово: прочитано заново — %1, по-прежнему не читаются — %2.")
                                     .arg(filesCount(fixed), filesCount(stillFailing)));
            }
            retrying_.clear();
        }
    }
    rebuildSummary();
    if (listBox_->isVisible()) fillTable();
    retrySelected_->setEnabled(retrying_.empty());
}

void ProblemFilesDialog::rebuildSummary() {
    while (QLayoutItem* item = summary_->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    if (files_.empty()) {
        intro_->setText(tr("Таких файлов нет — текст всех проиндексированных файлов прочитан."));
        listBox_->setVisible(false);
        return;
    }
    intro_->setText(tr("Эти файлы есть в индексе и находятся по имени, но их текст прочитать не удалось. "
                       "При запуске программы они не перечитываются, пока не изменятся. "
                       "Чтобы попробовать ещё раз, нажмите «Проиндексировать заново»."));

    std::map<ExtractionProblem, std::vector<std::size_t>> byReason;
    for (std::size_t i = 0; i < files_.size(); ++i) byReason[files_[i].file.problem].push_back(i);

    int row = 0;
    auto addRow = [&](const QString& title, const QString& hint, const std::vector<std::size_t>& indexes, int reason,
                      bool bold) {
        auto* name = new QLabel(title);
        auto* count = new QLabel(filesCount(indexes.size()));
        if (bold) {
            name->setStyleSheet(QStringLiteral("font-weight: 600;"));
            count->setStyleSheet(QStringLiteral("font-weight: 600;"));
        }
        name->setToolTip(hint);
        count->setToolTip(hint);
        auto* show = new QPushButton(tr("Показать"));
        auto* again = new QPushButton(tr("Проиндексировать заново"));
        again->setEnabled(retrying_.empty());  // one retry at a time
        connect(show, &QPushButton::clicked, this, [this, reason] { showList(reason); });
        connect(again, &QPushButton::clicked, this, [this, indexes] { retry(indexes); });
        summary_->addWidget(name, row, 0);
        summary_->addWidget(count, row, 1, Qt::AlignRight);
        summary_->addWidget(show, row, 2);
        summary_->addWidget(again, row, 3);
        ++row;
    };
    for (ExtractionProblem reason : kReasons) {
        const auto it = byReason.find(reason);
        if (it == byReason.end()) continue;
        addRow(reasonTitle(reason), reasonHint(reason), it->second, static_cast<int>(reason), false);
    }
    std::vector<std::size_t> all(files_.size());
    for (std::size_t i = 0; i < all.size(); ++i) all[i] = i;
    addRow(tr("Всего"), QString(), all, -1, true);
}

void ProblemFilesDialog::showList(int reason) {
    shownReason_ = reason;
    listBox_->setVisible(true);
    fillTable();
}

void ProblemFilesDialog::fillTable() {
    listTitle_->setText(shownReason_ < 0
                            ? tr("Все файлы:")
                            : tr("%1:").arg(reasonTitle(static_cast<ExtractionProblem>(shownReason_))));
    table_->setSortingEnabled(false);
    table_->setRowCount(0);
    for (std::size_t i = 0; i < files_.size(); ++i) {
        const auto& file = files_[i].file;
        if (shownReason_ >= 0 && static_cast<int>(file.problem) != shownReason_) continue;
        const int row = table_->rowCount();
        table_->insertRow(row);
        const QString path = QDir::toNativeSeparators(QString::fromStdString(file.path));
        auto* name = new QTableWidgetItem(QFileInfo(path).fileName());
        name->setData(Qt::UserRole, static_cast<qulonglong>(i));
        name->setToolTip(path);
        auto* where = new QTableWidgetItem(path);
        where->setToolTip(path);
        auto* size = new SizeItem(file.size);
        auto* why = new QTableWidgetItem(reasonForRow(file));
        why->setToolTip(reasonHint(file.problem));
        table_->setItem(row, 0, name);
        table_->setItem(row, 1, where);
        table_->setItem(row, 2, size);
        table_->setItem(row, 3, why);
    }
    table_->setSortingEnabled(true);
}

std::vector<std::size_t> ProblemFilesDialog::selectedIndexes() const {
    std::set<int> rows;
    for (const QModelIndex& index : table_->selectionModel()->selectedRows()) rows.insert(index.row());
    if (rows.empty() && table_->currentRow() >= 0) rows.insert(table_->currentRow());
    std::vector<std::size_t> out;
    for (int row : rows) {
        if (const QTableWidgetItem* item = table_->item(row, 0)) {
            out.push_back(static_cast<std::size_t>(item->data(Qt::UserRole).toULongLong()));
        }
    }
    return out;
}

void ProblemFilesDialog::retry(const std::vector<std::size_t>& indexes) {
    if (indexes.empty()) {
        status_->setText(tr("Выберите файлы в списке."));
        return;
    }
    retrying_.clear();
    for (std::size_t i : indexes) retrying_.emplace_back(files_[i].root, files_[i].file.path);
    retryStartedAt_ = static_cast<std::int64_t>(std::time(nullptr));
    status_->setText(tr("Файлы читаются заново: %1… Искать можно и сейчас.").arg(filesCount(retrying_.size())));
    rebuildSummary();  // its retry buttons off until this one is done
    retrySelected_->setEnabled(false);
    manager_->retryProblemFiles(retrying_);
}

void ProblemFilesDialog::openSelected() {
    const auto indexes = selectedIndexes();
    if (platform_ == nullptr || indexes.empty()) return;
    for (std::size_t i : indexes) {
        try {
            platform_->openFile(datasearch::core::pathFromUtf8(files_[i].file.path));
        } catch (const std::exception& e) {
            QMessageBox::warning(this, tr("Ошибка"), tr("Не удалось открыть файл: %1").arg(e.what()));
            return;
        }
    }
}

void ProblemFilesDialog::showSelectedInFolder() {
    const auto indexes = selectedIndexes();
    if (platform_ == nullptr || indexes.empty()) return;
    try {
        platform_->showInFolder(datasearch::core::pathFromUtf8(files_[indexes.front()].file.path));
    } catch (const std::exception& e) {
        QMessageBox::warning(this, tr("Ошибка"), tr("Не удалось показать файл в папке: %1").arg(e.what()));
    }
}
