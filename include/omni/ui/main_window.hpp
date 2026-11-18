#pragma once

#include "omni/core/qt_scheduler_bridge.hpp"
#include "omni/core/types.hpp"
#include "omni/infra/portable_env.hpp"

#include <QMainWindow>
#include <filesystem>
#include <map>
#include <vector>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTableWidget;

namespace omni::ui {

class DropZoneFrame;

struct FileRecord {
    std::filesystem::path inPath;
    std::uintmax_t inSize = 0;
    std::filesystem::path outPath;
    bool done = false;
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    void switchMode(omni::core::WorkMode mode);
    void toggleTheme();
    void showEngineReportDialog();
    void handleDroppedPaths(const QStringList& rawPaths);
    void browseAddFiles();
    void browseAddFolder();
    void removeSelectedRows();
    void clearCompletedRows();
    void clearAllRows();
    void showTableContextMenu(const QPoint& pos);
    void onRowDoubleClicked(int row, int column);
    void onOutModeChanged(int index);
    void pickOutputDir();
    void openCurrentOutputDir();
    void onStartOrStopClicked();

    void onTaskProgress(int rowId, int pct, const QString& statusText);
    void onTaskFinished(int rowId, bool ok, const QString& outPath, const QString& msg, quint64 outBytes);
    void onBatchFinished(int successCnt, int failCnt, quint64 savedBytes);

private:
    void buildUi();
    QWidget* buildImageSubpanel();
    QWidget* buildAudioSubpanel();
    QWidget* buildVideoSubpanel();
    QWidget* buildNovelSubpanel();

    void applyTheme();
    void restoreSavedUiState();
    void saveSettings() const;
    void populateModeFormats(omni::core::WorkMode mode);
    void refreshEngineStatus();
    int appendFilesToTable(const std::vector<std::filesystem::path>& files);

    omni::infra::PortableSettings settings_;
    bool darkMode_ = true;
    omni::core::WorkMode currentMode_ = omni::core::WorkMode::Image;
    omni::core::QtSchedulerBridge* worker_ = nullptr;

    std::vector<FileRecord> fileRecords_;
    std::map<int, int> rowProgressMap_;
    int completedCount_ = 0;
    int totalTasks_ = 0;

    // UI widgets
    QButtonGroup* navGroup_ = nullptr;
    std::map<omni::core::WorkMode, QPushButton*> navButtons_;
    QPushButton* engineStatusBtn_ = nullptr;
    QPushButton* themeBtn_ = nullptr;

    DropZoneFrame* dropZone_ = nullptr;
    QLabel* modeHeadlineLbl_ = nullptr;
    QLabel* modeHintLbl_ = nullptr;
    QTableWidget* table_ = nullptr;

    QComboBox* targetFmtCombo_ = nullptr;
    QLabel* compLabel_ = nullptr;
    QComboBox* compCombo_ = nullptr;
    QStackedWidget* subStack_ = nullptr;

    // Image subpanel
    QComboBox* imgDimCombo_ = nullptr;
    QCheckBox* imgStripExifChk_ = nullptr;

    // Audio subpanel
    QComboBox* audSrCombo_ = nullptr;
    QCheckBox* audLoudnormChk_ = nullptr;

    // Video subpanel
    QComboBox* vidResCombo_ = nullptr;
    QComboBox* vidCodecCombo_ = nullptr;
    QCheckBox* vidMuteChk_ = nullptr;

    // Novel subpanel
    QCheckBox* novIndentChk_ = nullptr;
    QComboBox* novPdfCombo_ = nullptr;
    QComboBox* novEncCombo_ = nullptr;
    QLineEdit* novAuthorEdit_ = nullptr;

    // Footer
    QComboBox* outModeCombo_ = nullptr;
    QLineEdit* outDirEdit_ = nullptr;
    QPushButton* btnPickDir_ = nullptr;
    QLabel* summaryLbl_ = nullptr;
    QProgressBar* batchBar_ = nullptr;
    QPushButton* startBtn_ = nullptr;
};

} // namespace omni::ui
