#include "ResultsTableModel.h"

#include <QDateTime>
#include <QLocale>

using datasearch::core::FileRecord;

namespace {

QString formatSize(std::uint64_t bytes) {
    static const char* units[] = {"Б", "КБ", "МБ", "ГБ", "ТБ"};
    double value = static_cast<double>(bytes);
    int unitIndex = 0;
    while (value >= 1024.0 && unitIndex < 4) {
        value /= 1024.0;
        ++unitIndex;
    }
    if (unitIndex == 0) {
        return QString("%1 %2").arg(bytes).arg(units[unitIndex]);
    }
    return QString("%1 %2").arg(value, 0, 'f', 1).arg(units[unitIndex]);
}

QString formatTime(std::int64_t epochSeconds) {
    if (epochSeconds <= 0) return QString();
    return QDateTime::fromSecsSinceEpoch(epochSeconds).toString("dd.MM.yyyy HH:mm");
}

} // namespace

ResultsTableModel::ResultsTableModel(QObject* parent) : QAbstractTableModel(parent) {}

void ResultsTableModel::setRecords(std::vector<FileRecord> records) {
    beginResetModel();
    records_ = std::move(records);
    endResetModel();
}

const FileRecord& ResultsTableModel::recordAt(int row) const {
    return records_.at(static_cast<std::size_t>(row));
}

int ResultsTableModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(records_.size());
}

int ResultsTableModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return ColumnCount;
}

QVariant ResultsTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        static_cast<std::size_t>(index.row()) >= records_.size()) {
        return {};
    }
    if (role != Qt::DisplayRole) return {};

    const FileRecord& record = records_[static_cast<std::size_t>(index.row())];
    switch (index.column()) {
        case ColumnName: return QString::fromStdString(record.name);
        case ColumnPath: return QString::fromStdString(record.path);
        case ColumnSize: return formatSize(record.size);
        case ColumnModified: return formatTime(record.modifiedTime);
        default: return {};
    }
}

QVariant ResultsTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    switch (section) {
        case ColumnName: return QObject::tr("Имя");
        case ColumnPath: return QObject::tr("Путь");
        case ColumnSize: return QObject::tr("Размер");
        case ColumnModified: return QObject::tr("Дата изменения");
        default: return {};
    }
}
