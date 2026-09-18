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
}
