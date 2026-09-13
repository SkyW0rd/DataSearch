#pragma once

#include "datasearch/core/FileRecord.h"

#include <QAbstractTableModel>

#include <string>
#include <vector>

// Backs the results QTableView: Имя | Путь | Размер | Дата изменения | Фрагмент
// с подсветкой найденного (ТЗ FR-15).
class ResultsTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { ColumnName = 0, ColumnPath, ColumnSize, ColumnModified, ColumnSnippet, ColumnCount };

    explicit ResultsTableModel(QObject* parent = nullptr);

    void setRecords(std::vector<datasearch::core::FileRecord> records);
    const datasearch::core::FileRecord& recordAt(int row) const;
    // Fills in one row's excerpt, fetched after the rows themselves (only
    // for rows on screen). Ignored if `row` no longer holds `path`.
    void setSnippet(int row, const std::string& path, const std::string& snippet);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    std::vector<datasearch::core::FileRecord> records_;
};
