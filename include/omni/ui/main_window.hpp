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
class QDoubleSpinBox;
class QFrame;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;
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
    void moveSelectedRowUp();
    void moveSelectedRowDown();
    void removeSelectedRows();
    void clearCompletedRows();
    void clearAllRows();
    void showTableContextMenu(const QPoint& pos);
    void onRowDoubleClicked(int row, int column);
    void onOutModeChanged(int index);
    void pickOutputDir();
    void openCurrentOutputDir();
    void onStartOrStopClicked();

    void onCompressionChanged();
    void onImageDimModeChanged();
    void pickImageBgColor();
    void onVideoResModeChanged();
    void onNovelCoverModeChanged();
    void pickNovelCoverImage();
    void previewProceduralCover();
    void onDocumentActionChanged();
    void onToolboxActionChanged();

    void onTaskProgress(int rowId, int pct, const QString& statusText);
    void onTaskFinished(int rowId, bool ok, const QString& outPath, const QString& msg, quint64 outBytes);
    void onBatchFinished(int successCnt, int failCnt, quint64 savedBytes);

private:
    void buildUi();
    QWidget* buildImageSubpanel();
    QWidget* buildAudioSubpanel();
    QWidget* buildVideoSubpanel();
    QWidget* buildNovelSubpanel();
    QWidget* buildDocumentSubpanel();
    QWidget* buildToolboxSubpanel();

    void applyTheme();
    void restoreSavedUiState();
    void saveSettings() const;
    void populateModeFormats(omni::core::WorkMode mode);
    void refreshEngineStatus();
    void updateDropZoneHints();
    void swapTableRows(int rowA, int rowB);
    std::string currentSubActionKey() const;
    omni::core::ConversionOptions collectConversionOptions() const;
    int appendFilesToTable(const std::vector<std::filesystem::path>& files);

    omni::infra::PortableSettings settings_;
    bool darkMode_ = true;
    omni::core::WorkMode currentMode_ = omni::core::WorkMode::Image;
    omni::core::QtSchedulerBridge* worker_ = nullptr;

    std::vector<FileRecord> fileRecords_;
    std::map<int, int> rowProgressMap_;
    int completedCount_ = 0;
    int totalTasks_ = 0;

    // Navigation & global controls
    QButtonGroup* navGroup_ = nullptr;
    std::map<omni::core::WorkMode, QPushButton*> navButtons_;
    QPushButton* engineStatusBtn_ = nullptr;
    QPushButton* themeBtn_ = nullptr;

    DropZoneFrame* dropZone_ = nullptr;
    QLabel* modeHeadlineLbl_ = nullptr;
    QLabel* modeHintLbl_ = nullptr;
    QTableWidget* table_ = nullptr;

    QLabel* targetFmtLbl_ = nullptr;
    QComboBox* targetFmtCombo_ = nullptr;
    QLabel* compLabel_ = nullptr;
    QComboBox* compCombo_ = nullptr;
    QStackedWidget* subStack_ = nullptr;

    // 1. Image subpanel
    QComboBox* imgDimCombo_ = nullptr;
    QSpinBox* imgCustomDimSpin_ = nullptr;
    QSpinBox* imgScaleSpin_ = nullptr;
    QComboBox* imgRotateCombo_ = nullptr;
    QPushButton* imgBgColorBtn_ = nullptr;
    QString imgBgHex_ = QStringLiteral("#FFFFFF");
    QCheckBox* imgFlipChk_ = nullptr;
    QCheckBox* imgGrayChk_ = nullptr;
    QCheckBox* imgStripExifChk_ = nullptr;
    QLineEdit* imgWatermarkEdit_ = nullptr;
    QFrame* imgCustomFrame_ = nullptr;
    QSlider* imgQualitySlider_ = nullptr;
    QSpinBox* imgQualitySpin_ = nullptr;
    QComboBox* imgPaletteCombo_ = nullptr;

    // 2. Audio subpanel
    QComboBox* audSrCombo_ = nullptr;
    QComboBox* audChCombo_ = nullptr;
    QComboBox* audSpeedCombo_ = nullptr;
    QLineEdit* audTrimStartEdit_ = nullptr;
    QLineEdit* audTrimEndEdit_ = nullptr;
    QCheckBox* audLoudnormChk_ = nullptr;
    QFrame* audCustomFrame_ = nullptr;
    QSpinBox* audBitrateSpin_ = nullptr;

    // 3. Video subpanel
    QComboBox* vidResCombo_ = nullptr;
    QSpinBox* vidCustomWidthSpin_ = nullptr;
    QComboBox* vidCodecCombo_ = nullptr;
    QComboBox* vidFpsCombo_ = nullptr;
    QLineEdit* vidTrimStartEdit_ = nullptr;
    QLineEdit* vidTrimEndEdit_ = nullptr;
    QCheckBox* vidMuteChk_ = nullptr;
    QFrame* vidCustomFrame_ = nullptr;
    QSlider* vidCrfSlider_ = nullptr;
    QSpinBox* vidCrfSpin_ = nullptr;

    // 4. Novel subpanel
    QComboBox* novCoverModeCombo_ = nullptr;
    QPushButton* novPickCoverBtn_ = nullptr;
    QPushButton* novPreviewCoverBtn_ = nullptr;
    QLabel* novCoverThumbLbl_ = nullptr;
    QLabel* novCoverPathLbl_ = nullptr;
    std::filesystem::path novCustomCoverPath_;
    QLineEdit* novTitleEdit_ = nullptr;
    QLineEdit* novAuthorEdit_ = nullptr;
    QComboBox* novZhCombo_ = nullptr;
    QComboBox* novIndentCombo_ = nullptr;
    QDoubleSpinBox* novFontSizeSpin_ = nullptr;
    QDoubleSpinBox* novLineHeightSpin_ = nullptr;
    QComboBox* novPdfCombo_ = nullptr;
    QComboBox* novEncCombo_ = nullptr;
    QCheckBox* novCleanAdsChk_ = nullptr;
    QLineEdit* novRegexEdit_ = nullptr;

    // 5. Document & Table Workshop subpanel
    QComboBox* docActionCombo_ = nullptr;
    QWidget* docPagePresetWrap_ = nullptr;
    QComboBox* docPagePresetCombo_ = nullptr;
    QWidget* docDpiWrap_ = nullptr;
    QComboBox* docDpiCombo_ = nullptr;
    QWidget* docMergedNameWrap_ = nullptr;
    QLineEdit* docMergedNameEdit_ = nullptr;
    QLabel* docHintLbl_ = nullptr;

    // 6. Everyday Life Toolbox subpanel
    QComboBox* toolActionCombo_ = nullptr;
    QStackedWidget* toolDetailStack_ = nullptr;
    QSpinBox* toolTargetKbSpin_ = nullptr;
    QLineEdit* toolWatermarkEdit_ = nullptr;
    QSlider* toolWmOpacitySlider_ = nullptr;
    QSpinBox* toolWmOpacitySpin_ = nullptr;
    QSpinBox* toolSubShiftSpin_ = nullptr;
    QLabel* toolHintLbl_ = nullptr;

    // Footer
    QComboBox* outModeCombo_ = nullptr;
    QLineEdit* outDirEdit_ = nullptr;
    QPushButton* btnPickDir_ = nullptr;
    QLabel* summaryLbl_ = nullptr;
    QProgressBar* batchBar_ = nullptr;
    QPushButton* startBtn_ = nullptr;
};

} // namespace omni::ui
