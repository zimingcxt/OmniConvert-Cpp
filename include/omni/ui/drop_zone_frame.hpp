#pragma once

#include <QFrame>
#include <QStringList>

namespace omni::ui {

class DropZoneFrame : public QFrame {
    Q_OBJECT

public:
    explicit DropZoneFrame(QWidget* parent = nullptr);

signals:
    void pathsDropped(const QStringList& localPaths);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
};

} // namespace omni::ui
