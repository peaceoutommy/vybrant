// In-app drag and drop: our drags never leave the application, and the system (XDND) drag kept showing a 'no drop'
// cursor between our own windows under XWayland. runInAppDrag() grabs the mouse, floats a badge under the pointer,
// feeds the widget under the pointer the normal drag events (targets keep their dragEnter/dragMove/drop handlers) and
// delivers the drop on release. Returns true when something accepted the drop.
#pragma once
#include <QWidget>
#include <QMimeData>
#include <memory>

namespace mt {
bool runInAppDrag(QWidget* source, std::unique_ptr<QMimeData> mime, const QString& badgeText);

// Our drags carry "kind|value" (audio|path, native|key, plugin|key, pattern|id, lane|id, preset|path) under kItemMime,
// next to plain file URLs for audio
inline const char* const kItemMime = "application/x-tactum-item";
inline bool hasAppDrop(const QMimeData* m) { return m->hasUrls() || m->hasFormat(kItemMime); }
struct DropItem {
    QString kind, value;
};
inline DropItem dropItem(const QMimeData* m) {
    QString s = QString::fromUtf8(m->data(kItemMime));
    return {s.section('|', 0, 0), s.section('|', 1)};
}
inline void setDropItem(QMimeData* m, const QString& payload) { m->setData(kItemMime, payload.toUtf8()); }
}
