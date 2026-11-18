#pragma once

#include "omni/core/dual_pool_scheduler.hpp"
#include "omni/core/types.hpp"

#include <QString>
#include <QThread>
#include <vector>

namespace omni::core {

class QtSchedulerBridge : public QThread {
    Q_OBJECT

public:
    QtSchedulerBridge(
        std::vector<TaskItem> tasks,
        ConversionOptions options,
        QObject* parent = nullptr);

    void requestCancel();
    bool isCancelled() const;

signals:
    void taskProgress(int rowId, int percent, QString statusText);
    void taskFinished(int rowId, bool ok, QString outPath, QString message, quint64 outBytes);
    void batchFinished(int successCount, int failCount, quint64 savedBytes);

protected:
    void run() override;

private:
    std::vector<TaskItem> tasks_;
    ConversionOptions options_;
    DualPoolScheduler scheduler_;
};

} // namespace omni::core
