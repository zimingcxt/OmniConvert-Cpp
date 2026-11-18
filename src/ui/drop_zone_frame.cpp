#include "omni/ui/drop_zone_frame.hpp"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>

namespace omni::ui {

DropZoneFrame::DropZoneFrame(QWidget* parent)
    : QFrame(parent) {
    setObjectName(QStringLiteral("DropZone"));
    setAcceptDrops(true);
}

void DropZoneFrame::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData() && event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void DropZoneFrame::dropEvent(QDropEvent* event) {
    QStringList paths;
    if (event->mimeData()) {
        for (const QUrl& url : event->mimeData()->urls()) {
            const QString local = url.toLocalFile();
            if (!local.isEmpty()) {
                paths.append(local);
            }
        }
    }
    if (!paths.isEmpty()) {
        emit pathsDropped(paths);
    }
    event->acceptProposedAction();
}

} // namespace omni::ui
