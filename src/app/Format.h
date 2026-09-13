#pragma once

#include <QLocale>
#include <QObject>
#include <QString>

// Numbers, sizes and durations as the UI shows them (Russian grouping and
// decimal comma: "124 000", "60,2 МБ").
namespace display {

inline const QLocale& ru() {
    static const QLocale locale(QLocale::Russian, QLocale::Russia);
    return locale;
}

inline QString formatCount(quint64 n) {
    return ru().toString(static_cast<qulonglong>(n));
}

inline QString formatSize(quint64 bytes) {
    if (bytes >= 1024ull * 1024 * 1024) return ru().toString(bytes / 1073741824.0, 'f', 1) + QObject::tr(" ГБ");
    if (bytes >= 1024ull * 1024) return ru().toString(bytes / 1048576.0, 'f', 1) + QObject::tr(" МБ");
    if (bytes >= 1024) return ru().toString(bytes / 1024.0, 'f', 0) + QObject::tr(" КБ");
    return ru().toString(static_cast<qulonglong>(bytes)) + QObject::tr(" Б");
}

inline QString formatDuration(qint64 seconds) {
    if (seconds < 60) return QObject::tr("%1 с").arg(seconds);
    if (seconds < 3600) return QObject::tr("%1 мин %2 с").arg(seconds / 60).arg(seconds % 60);
    return QObject::tr("%1 ч %2 мин").arg(seconds / 3600).arg((seconds % 3600) / 60);
}

// 42 s -> "00:42", 1 h 5 min -> "1:05:00".
inline QString formatClock(qint64 seconds) {
    const qint64 h = seconds / 3600;
    const qint64 m = (seconds % 3600) / 60;
    const qint64 sec = seconds % 60;
    return h > 0 ? QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0'))
                 : QString("%1:%2").arg(m, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0'));
}

} // namespace display
