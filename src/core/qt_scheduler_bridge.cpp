#include "omni/core/qt_scheduler_bridge.hpp"

namespace omni::core {

QtSchedulerBridge::QtSchedulerBridge(
    std::vector<TaskItem> tasks,
    ConversionOptions options,
    QObject* parent)
    : QThread(parent),
      tasks_(std::move(tasks)),
      options_(std::move(options)) {}

void QtSchedulerBridge::requestCancel() {
    scheduler_.requestCancel();
}

bool QtSchedulerBridge::isCancelled() const {
    return scheduler_.isCancelled();
}

void QtSchedulerBridge::run() {
    scheduler_.executeBatch(
        tasks_,
        options_,
        [this](int rowId, int pct, const std::string& statusText) {
            emit taskProgress(rowId, pct, QString::fromUtf8(statusText.c_str()));
        },
        [this](const TaskResult& res) {
            emit taskFinished(
                res.rowId,
                res.ok,
                pathToQString(res.outputPath),
                QString::fromUtf8(res.message.c_str()),
                static_cast<quint64>(res.outputBytes));
        },
        [this](int successCnt, int failCnt, std::uintmax_t savedBytes) {
            emit batchFinished(successCnt, failCnt, static_cast<quint64>(savedBytes));
        });
}

} // namespace omni::core
