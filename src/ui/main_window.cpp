#include "omni/ui/main_window.hpp"
#include "omni/engines/conversion_engine.hpp"
#include "omni/engines/image_engine.hpp"
#include "omni/ui/drop_zone_frame.hpp"
#include "omni/ui/theme.hpp"

#include <QAbstractItemView>
#include <QAction>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <set>
#include <system_error>

namespace omni::ui {

using omni::core::CompressionLevel;
using omni::core::WorkMode;

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("OmniConvert 万能格式工坊 · 离线便携跨平台版 (C++/Qt6)"));
    setWindowIcon(QIcon(QStringLiteral(":/app_icon.png")));
    resize(1100, 740);
    setMinimumSize(920, 620);
    setAcceptDrops(true);

    settings_ = omni::infra::PortableEnv::loadSettings();
    darkMode_ = settings_.darkMode;
    currentMode_ = WorkMode::Image;

    buildUi();
    applyTheme();
    restoreSavedUiState();
    refreshEngineStatus();
}

MainWindow::~MainWindow() {
    if (worker_ && worker_->isRunning()) {
        worker_->requestCancel();
        worker_->wait(3000);
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveSettings();
    if (worker_ && worker_->isRunning()) {
        worker_->requestCancel();
        worker_->wait(2500);
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::saveSettings() const {
    omni::infra::PortableSettings s;
    s.darkMode = darkMode_;
    s.lastMode = omni::core::modeToKey(currentMode_);
    s.outDir = outDirEdit_->text().trimmed().toUtf8().toStdString();
    s.useCustomDir = (outModeCombo_->currentIndex() == 1);
    omni::infra::PortableEnv::saveSettings(s);
}

void MainWindow::buildUi() {
    auto* central = new QWidget(this);
    setCentralWidget(central);
    auto* rootLayout = new QHBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // 1. Left Sidebar
    auto* sidebar = new QFrame(central);
    sidebar->setObjectName(QStringLiteral("Sidebar"));
    sidebar->setFixedWidth(230);
    auto* sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(16, 22, 16, 18);
    sideLayout->setSpacing(8);

    auto* brandRow = new QHBoxLayout();
    brandRow->setSpacing(8);
    auto* iconLbl = new QLabel(sidebar);
    iconLbl->setPixmap(QIcon(QStringLiteral(":/app_icon.png")).pixmap(26, 26));
    brandRow->addWidget(iconLbl);

    auto* titleLbl = new QLabel(QStringLiteral("OmniConvert"), sidebar);
    titleLbl->setObjectName(QStringLiteral("AppTitle"));
    brandRow->addWidget(titleLbl, 1);
    sideLayout->addLayout(brandRow);

    auto* subLbl = new QLabel(QStringLiteral("离线便携 · 万能转换压缩工坊"), sidebar);
    subLbl->setObjectName(QStringLiteral("Muted"));
    sideLayout->addWidget(subLbl);
    sideLayout->addSpacing(14);

    navGroup_ = new QButtonGroup(this);
    navGroup_->setExclusive(true);

    const std::vector<std::pair<WorkMode, QString>> navDefs = {
        {WorkMode::Image, QStringLiteral("🖼️  图片压缩与转换")},
        {WorkMode::Audio, QStringLiteral("🎵  音频转换与提音")},
        {WorkMode::Video, QStringLiteral("🎬  视频压缩与动图")},
        {WorkMode::Novel, QStringLiteral("📚  小说格式互转")},
    };

    for (std::size_t i = 0; i < navDefs.size(); ++i) {
        const WorkMode modeKey = navDefs[i].first;
        auto* btn = new QPushButton(navDefs[i].second, sidebar);
        btn->setObjectName(QStringLiteral("NavBtn"));
        btn->setCheckable(true);
        btn->setCursor(Qt::PointingHandCursor);
        if (i == 0) btn->setChecked(true);
        connect(btn, &QPushButton::clicked, this, [this, modeKey]() { switchMode(modeKey); });
        navGroup_->addButton(btn);
        navButtons_[modeKey] = btn;
        sideLayout->addWidget(btn);
    }

    sideLayout->addStretch(1);

    engineStatusBtn_ = new QPushButton(QStringLiteral("🟢 离线引擎状态"), sidebar);
    engineStatusBtn_->setCursor(Qt::PointingHandCursor);
    connect(engineStatusBtn_, &QPushButton::clicked, this, &MainWindow::showEngineReportDialog);
    sideLayout->addWidget(engineStatusBtn_);

    themeBtn_ = new QPushButton(QStringLiteral("☀️ 切换浅色模式"), sidebar);
    themeBtn_->setCursor(Qt::PointingHandCursor);
    connect(themeBtn_, &QPushButton::clicked, this, &MainWindow::toggleTheme);
    sideLayout->addWidget(themeBtn_);

    rootLayout->addWidget(sidebar);

    // 2. Right Main Workspace
    auto* workspace = new QWidget(central);
    auto* wsLayout = new QVBoxLayout(workspace);
    wsLayout->setContentsMargins(20, 18, 20, 18);
    wsLayout->setSpacing(12);

    // Top Drop Zone & Toolbar
    dropZone_ = new DropZoneFrame(workspace);
    connect(dropZone_, &DropZoneFrame::pathsDropped, this, &MainWindow::handleDroppedPaths);
    auto* dzLayout = new QHBoxLayout(dropZone_);
    dzLayout->setContentsMargins(18, 12, 18, 12);

    auto* dzTextBox = new QVBoxLayout();
    modeHeadlineLbl_ = new QLabel(QStringLiteral("🖼️ 图片多级压缩与全格式转换"), dropZone_);
    modeHeadlineLbl_->setObjectName(QStringLiteral("SectionTitle"));
    modeHintLbl_ = new QLabel(QStringLiteral("拖入文件或整个文件夹到窗口任意位置 · 支持 JPG / PNG / WEBP / AVIF / HEIC / ICO 等"), dropZone_);
    modeHintLbl_->setObjectName(QStringLiteral("Muted"));
    dzTextBox->addWidget(modeHeadlineLbl_);
    dzTextBox->addWidget(modeHintLbl_);
    dzLayout->addLayout(dzTextBox, 1);

    auto* btnAddFiles = new QPushButton(QStringLiteral("➕ 添加文件"), dropZone_);
    btnAddFiles->setCursor(Qt::PointingHandCursor);
    connect(btnAddFiles, &QPushButton::clicked, this, &MainWindow::browseAddFiles);

    auto* btnAddDir = new QPushButton(QStringLiteral("📁 添加文件夹"), dropZone_);
    btnAddDir->setCursor(Qt::PointingHandCursor);
    connect(btnAddDir, &QPushButton::clicked, this, &MainWindow::browseAddFolder);

    auto* btnClearDone = new QPushButton(QStringLiteral("🧹 清理已完成"), dropZone_);
    btnClearDone->setCursor(Qt::PointingHandCursor);
    connect(btnClearDone, &QPushButton::clicked, this, &MainWindow::clearCompletedRows);

    auto* btnClearAll = new QPushButton(QStringLiteral("🗑️ 清空列表"), dropZone_);
    btnClearAll->setCursor(Qt::PointingHandCursor);
    connect(btnClearAll, &QPushButton::clicked, this, &MainWindow::clearAllRows);

    dzLayout->addWidget(btnAddFiles);
    dzLayout->addWidget(btnAddDir);
    dzLayout->addWidget(btnClearDone);
    dzLayout->addWidget(btnClearAll);
    wsLayout->addWidget(dropZone_);

    // Middle Task Queue Table
    table_ = new QTableWidget(0, 5, workspace);
    table_->setHorizontalHeaderLabels({
        QStringLiteral("文件名"),
        QStringLiteral("原格式 · 原始体积"),
        QStringLiteral("输出体积 · 节省比"),
        QStringLiteral("进度"),
        QStringLiteral("状态 (双击打开位置)")
    });
    table_->setAlternatingRowColors(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->setVisible(false);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QWidget::customContextMenuRequested, this, &MainWindow::showTableContextMenu);
    connect(table_, &QTableWidget::cellDoubleClicked, this, &MainWindow::onRowDoubleClicked);

    QHeaderView* header = table_->horizontalHeader();
    header->setSectionResizeMode(0, QHeaderView::Stretch);
    header->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(3, QHeaderView::Fixed);
    table_->setColumnWidth(3, 130);
    header->setSectionResizeMode(4, QHeaderView::ResizeToContents);

    auto* delAction = new QAction(this);
    delAction->setShortcuts({QKeySequence::Delete, QKeySequence(Qt::Key_Backspace)});
    connect(delAction, &QAction::triggered, this, &MainWindow::removeSelectedRows);
    table_->addAction(delAction);

    wsLayout->addWidget(table_, 1);

    // Parameters Card
    auto* settingsCard = new QFrame(workspace);
    settingsCard->setObjectName(QStringLiteral("Card"));
    auto* scLayout = new QVBoxLayout(settingsCard);
    scLayout->setContentsMargins(16, 12, 16, 12);
    scLayout->setSpacing(10);

    auto* topOptsRow = new QHBoxLayout();
    topOptsRow->addWidget(new QLabel(QStringLiteral("目标格式:"), settingsCard));
    targetFmtCombo_ = new QComboBox(settingsCard);
    targetFmtCombo_->setMinimumWidth(115);
    topOptsRow->addWidget(targetFmtCombo_);

    topOptsRow->addSpacing(14);
    compLabel_ = new QLabel(QStringLiteral("压缩强度:"), settingsCard);
    topOptsRow->addWidget(compLabel_);
    compCombo_ = new QComboBox(settingsCard);
    for (const auto& preset : omni::core::compressionPresets()) {
        compCombo_->addItem(QString::fromUtf8(preset.description), QString::fromUtf8(preset.key));
    }
    compCombo_->setCurrentIndex(2); // Default: balanced
    compCombo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    topOptsRow->addWidget(compCombo_, 1);
    scLayout->addLayout(topOptsRow);

    subStack_ = new QStackedWidget(settingsCard);
    subStack_->addWidget(buildImageSubpanel());
    subStack_->addWidget(buildAudioSubpanel());
    subStack_->addWidget(buildVideoSubpanel());
    subStack_->addWidget(buildNovelSubpanel());
    scLayout->addWidget(subStack_);

    wsLayout->addWidget(settingsCard);

    // Footer Output & Control Card
    auto* footerCard = new QFrame(workspace);
    footerCard->setObjectName(QStringLiteral("Card"));
    auto* ftLayout = new QVBoxLayout(footerCard);
    ftLayout->setContentsMargins(16, 12, 16, 12);
    ftLayout->setSpacing(10);

    auto* outRow = new QHBoxLayout();
    outRow->addWidget(new QLabel(QStringLiteral("输出位置:"), footerCard));
    outModeCombo_ = new QComboBox(footerCard);
    outModeCombo_->addItems({
        QStringLiteral("保存在源文件所在目录（自动重命名防覆盖）"),
        QStringLiteral("统一保存到指定目录")
    });
    connect(outModeCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onOutModeChanged);
    outRow->addWidget(outModeCombo_);

    outDirEdit_ = new QLineEdit(footerCard);
    outDirEdit_->setPlaceholderText(QStringLiteral("请选择输出目录..."));
    outDirEdit_->setEnabled(false);
    outRow->addWidget(outDirEdit_, 1);

    btnPickDir_ = new QPushButton(QStringLiteral("浏览..."), footerCard);
    btnPickDir_->setEnabled(false);
    connect(btnPickDir_, &QPushButton::clicked, this, &MainWindow::pickOutputDir);
    outRow->addWidget(btnPickDir_);

    auto* btnOpenOut = new QPushButton(QStringLiteral("📂 打开输出目录"), footerCard);
    connect(btnOpenOut, &QPushButton::clicked, this, &MainWindow::openCurrentOutputDir);
    outRow->addWidget(btnOpenOut);
    ftLayout->addLayout(outRow);

    auto* runRow = new QHBoxLayout();
    summaryLbl_ = new QLabel(QStringLiteral("就绪 · 请拖入或添加需要处理的文件"), footerCard);
    summaryLbl_->setObjectName(QStringLiteral("Muted"));
    runRow->addWidget(summaryLbl_, 1);

    batchBar_ = new QProgressBar(footerCard);
    batchBar_->setFixedWidth(200);
    batchBar_->setValue(0);
    runRow->addWidget(batchBar_);

    startBtn_ = new QPushButton(QStringLiteral("🚀 开始批量转换"), footerCard);
    startBtn_->setObjectName(QStringLiteral("PrimaryBtn"));
    startBtn_->setCursor(Qt::PointingHandCursor);
    connect(startBtn_, &QPushButton::clicked, this, &MainWindow::onStartOrStopClicked);
    runRow->addWidget(startBtn_);

    ftLayout->addLayout(runRow);
    wsLayout->addWidget(footerCard);

    rootLayout->addWidget(workspace, 1);
    populateModeFormats(WorkMode::Image);
}

QWidget* MainWindow::buildImageSubpanel() {
    auto* w = new QWidget(this);
    auto* ly = new QHBoxLayout(w);
    ly->setContentsMargins(0, 0, 0, 0);
    ly->addWidget(new QLabel(QStringLiteral("尺寸限制:"), w));

    imgDimCombo_ = new QComboBox(w);
    imgDimCombo_->addItem(QStringLiteral("保持原图分辨率"), 0);
    imgDimCombo_->addItem(QStringLiteral("限制长边 3840 px (4K)"), 3840);
    imgDimCombo_->addItem(QStringLiteral("限制长边 2560 px (2K)"), 2560);
    imgDimCombo_->addItem(QStringLiteral("限制长边 1920 px (1080P)"), 1920);
    imgDimCombo_->addItem(QStringLiteral("限制长边 1280 px (网页高清)"), 1280);
    imgDimCombo_->addItem(QStringLiteral("限制长边 800 px (轻量缩略图)"), 800);
    ly->addWidget(imgDimCombo_);

    ly->addSpacing(16);
    imgStripExifChk_ = new QCheckBox(QStringLiteral("清除 EXIF 拍摄参数与地理位置隐私（自动保留正确旋转方向）"), w);
    ly->addWidget(imgStripExifChk_);
    ly->addStretch(1);
    return w;
}

QWidget* MainWindow::buildAudioSubpanel() {
    auto* w = new QWidget(this);
    auto* ly = new QHBoxLayout(w);
    ly->setContentsMargins(0, 0, 0, 0);
    ly->addWidget(new QLabel(QStringLiteral("采样率:"), w));

    audSrCombo_ = new QComboBox(w);
    audSrCombo_->addItem(QStringLiteral("保持原音频采样率"), 0);
    audSrCombo_->addItem(QStringLiteral("48000 Hz (影视级)"), 48000);
    audSrCombo_->addItem(QStringLiteral("44100 Hz (CD 标准)"), 44100);
    audSrCombo_->addItem(QStringLiteral("24000 Hz (语音有声书)"), 24000);
    ly->addWidget(audSrCombo_);

    ly->addSpacing(16);
    audLoudnormChk_ = new QCheckBox(QStringLiteral("开启智能响度均衡 (自动统一录音/播客音量大小)"), w);
    ly->addWidget(audLoudnormChk_);
    ly->addStretch(1);
    return w;
}

QWidget* MainWindow::buildVideoSubpanel() {
    auto* w = new QWidget(this);
    auto* ly = new QHBoxLayout(w);
    ly->setContentsMargins(0, 0, 0, 0);
    ly->addWidget(new QLabel(QStringLiteral("画面尺寸:"), w));

    vidResCombo_ = new QComboBox(w);
    vidResCombo_->addItem(QStringLiteral("保持原分辨率"), 0);
    vidResCombo_->addItem(QStringLiteral("最高 1080P (1920宽)"), 1920);
    vidResCombo_->addItem(QStringLiteral("最高 720P (1280宽)"), 1280);
    vidResCombo_->addItem(QStringLiteral("最高 480P (854宽)"), 854);
    ly->addWidget(vidResCombo_);

    ly->addSpacing(12);
    ly->addWidget(new QLabel(QStringLiteral("编码器:"), w));
    vidCodecCombo_ = new QComboBox(w);
    vidCodecCombo_->addItem(QStringLiteral("H.264 (全设备兼容性最佳)"), QStringLiteral("h264"));
    vidCodecCombo_->addItem(QStringLiteral("H.265 / HEVC (同画质体积更小)"), QStringLiteral("h265"));
    ly->addWidget(vidCodecCombo_);

    ly->addSpacing(12);
    vidMuteChk_ = new QCheckBox(QStringLiteral("移除音轨（导出静音纯画面视频）"), w);
    ly->addWidget(vidMuteChk_);
    ly->addStretch(1);
    return w;
}

QWidget* MainWindow::buildNovelSubpanel() {
    auto* w = new QWidget(this);
    auto* ly = new QHBoxLayout(w);
    ly->setContentsMargins(0, 0, 0, 0);

    novIndentChk_ = new QCheckBox(QStringLiteral("自动段首缩进两格并清理空行"), w);
    novIndentChk_->setChecked(true);
    ly->addWidget(novIndentChk_);

    ly->addSpacing(10);
    ly->addWidget(new QLabel(QStringLiteral("PDF版式:"), w));
    novPdfCombo_ = new QComboBox(w);
    novPdfCombo_->addItem(QStringLiteral("A4 标准版"), QStringLiteral("a4"));
    novPdfCombo_->addItem(QStringLiteral("B5 舒适阅读"), QStringLiteral("b5"));
    novPdfCombo_->addItem(QStringLiteral("6寸 电纸书/手机大字版"), QStringLiteral("eink6"));
    ly->addWidget(novPdfCombo_);

    ly->addSpacing(10);
    ly->addWidget(new QLabel(QStringLiteral("TXT编码:"), w));
    novEncCombo_ = new QComboBox(w);
    novEncCombo_->addItem(QStringLiteral("UTF-8 (现代通用推荐)"), QStringLiteral("utf-8"));
    novEncCombo_->addItem(QStringLiteral("GB18030 (兼容老式MP3/车载阅读)"), QStringLiteral("gb18030"));
    ly->addWidget(novEncCombo_);

    ly->addSpacing(10);
    ly->addWidget(new QLabel(QStringLiteral("作者署名:"), w));
    novAuthorEdit_ = new QLineEdit(w);
    novAuthorEdit_->setPlaceholderText(QStringLiteral("留空则自动识别原书作者"));
    novAuthorEdit_->setMaximumWidth(160);
    ly->addWidget(novAuthorEdit_);
    ly->addStretch(1);
    return w;
}

void MainWindow::applyTheme() {
    setStyleSheet(buildStylesheet(darkMode_));
    themeBtn_->setText(darkMode_ ? QStringLiteral("☀️ 切换浅色模式") : QStringLiteral("🌙 切换深色模式"));
    if (startBtn_) {
        startBtn_->style()->unpolish(startBtn_);
        startBtn_->style()->polish(startBtn_);
    }
}

void MainWindow::toggleTheme() {
    darkMode_ = !darkMode_;
    applyTheme();
    saveSettings();
}

void MainWindow::restoreSavedUiState() {
    if (!settings_.outDir.empty()) {
        outDirEdit_->setText(QString::fromUtf8(settings_.outDir.c_str()));
    }
    if (settings_.useCustomDir) {
        outModeCombo_->setCurrentIndex(1);
    }
    const WorkMode savedMode = omni::core::modeFromKey(settings_.lastMode);
    if (auto it = navButtons_.find(savedMode); it != navButtons_.end()) {
        it->second->setChecked(true);
        switchMode(savedMode);
    }
}

void MainWindow::switchMode(WorkMode mode) {
    if (worker_ && worker_->isRunning()) {
        navButtons_[currentMode_]->setChecked(true);
        QMessageBox::information(this, QStringLiteral("任务进行中"),
                                 QStringLiteral("请等待当前批量转换完成或点击停止后再切换工坊模块。"));
        return;
    }

    if (mode == currentMode_ && targetFmtCombo_->count() > 0) {
        return;
    }

    if (!fileRecords_.empty()) {
        const bool hasIncompatible = std::any_of(fileRecords_.begin(), fileRecords_.end(), [&](const FileRecord& r) {
            return !omni::core::isFileAcceptedInMode(r.inPath, mode);
        });
        if (hasIncompatible) {
            clearAllRows();
        }
    }

    currentMode_ = mode;
    populateModeFormats(mode);
    saveSettings();
}

void MainWindow::populateModeFormats(WorkMode mode) {
    targetFmtCombo_->clear();
    switch (mode) {
        case WorkMode::Image: {
            modeHeadlineLbl_->setText(QStringLiteral("🖼️ 图片多级压缩与全格式转换"));
            modeHintLbl_->setText(QStringLiteral("拖入图片或文件夹 · 支持 JPG / PNG / WEBP / AVIF / HEIC / BMP / TIFF / GIF / ICO"));
            QStringList fmts = {QStringLiteral("WEBP"), QStringLiteral("JPG"), QStringLiteral("PNG")};
            if (omni::engines::ImageEngine::supportsAvif()) fmts << QStringLiteral("AVIF");
            if (omni::engines::ImageEngine::supportsHeic()) fmts << QStringLiteral("HEIC");
            fmts << QStringLiteral("ICO") << QStringLiteral("BMP") << QStringLiteral("TIFF") << QStringLiteral("GIF");
            targetFmtCombo_->addItems(fmts);
            compLabel_->setVisible(true);
            compCombo_->setVisible(true);
            subStack_->setCurrentIndex(0);
            break;
        }
        case WorkMode::Audio:
            modeHeadlineLbl_->setText(QStringLiteral("🎵 音频多级压缩转换 & 视频一键提取音频"));
            modeHintLbl_->setText(QStringLiteral("支持直接拖入 MP4/MKV/MOV 视频提取音乐，或 MP3 / FLAC / WAV / M4A / OGG / OPUS 互转"));
            targetFmtCombo_->addItems({
                QStringLiteral("MP3"), QStringLiteral("M4A"), QStringLiteral("FLAC"),
                QStringLiteral("WAV"), QStringLiteral("OGG"), QStringLiteral("OPUS")
            });
            compLabel_->setVisible(true);
            compCombo_->setVisible(true);
            subStack_->setCurrentIndex(1);
            break;

        case WorkMode::Video:
            modeHeadlineLbl_->setText(QStringLiteral("🎬 视频多级瘦身压缩 & 高清 GIF 动图转换"));
            modeHintLbl_->setText(QStringLiteral("支持 MP4 / MKV / MOV / WEBM / AVI / FLV 等多级画质体积压缩与调色盘 GIF 导出"));
            targetFmtCombo_->addItems({
                QStringLiteral("MP4"), QStringLiteral("MKV"), QStringLiteral("MOV"),
                QStringLiteral("WEBM"), QStringLiteral("GIF"), QStringLiteral("AVI")
            });
            compLabel_->setVisible(true);
            compCombo_->setVisible(true);
            subStack_->setCurrentIndex(2);
            break;

        case WorkMode::Novel:
            modeHeadlineLbl_->setText(QStringLiteral("📚 小说电子书全格式互转与智能分章排版"));
            modeHintLbl_->setText(QStringLiteral("自动识别 TXT 乱码编码与中英文章节目录 · 支持 TXT / EPUB / MOBI / AZW3 / PDF / DOCX / HTML / MD / FB2"));
            targetFmtCombo_->addItems({
                QStringLiteral("EPUB"), QStringLiteral("TXT"), QStringLiteral("MOBI"), QStringLiteral("PDF"),
                QStringLiteral("HTML"), QStringLiteral("DOCX"), QStringLiteral("MD"), QStringLiteral("FB2")
            });
            compLabel_->setVisible(false);
            compCombo_->setVisible(false);
            subStack_->setCurrentIndex(3);
            break;
    }
}

void MainWindow::refreshEngineStatus() {
    const auto st = omni::engines::ConversionEngine::inspectHealth();
    if (st.allCoreReady()) {
        engineStatusBtn_->setText(QStringLiteral("🟢 离线引擎全部就绪"));
    } else {
        engineStatusBtn_->setText(QStringLiteral("🟡 部分可选引擎待安装"));
    }
}

void MainWindow::showEngineReportDialog() {
    const auto st = omni::engines::ConversionEngine::inspectHealth();
    const QString ffStatus = st.ffmpegAvailable
                                 ? (QStringLiteral("✅ ") + QString::fromUtf8(st.ffmpegPath.c_str()))
                                 : QStringLiteral("❌ 未检测到 (请将 ffmpeg 放入同级 ./bin 目录)");
    const QString calStatus = !st.calibrePath.empty()
                                  ? (QStringLiteral("✅ ") + QString::fromUtf8(st.calibrePath.c_str()))
                                  : QStringLiteral("未安装 (已启用内置纯 C++ 离线 MOBI 引擎)");

    QStringList lines;
    lines << QStringLiteral("• 图片核心引擎 (Qt6 Image + C++ 色彩量化): ✅ 已内置就绪")
          << QStringLiteral("• AVIF 新一代图像支持: %1").arg(st.avifSupported ? QStringLiteral("✅ 已就绪") : QStringLiteral("⚠️ 需 FFmpeg 或插件"))
          << QStringLiteral("• 苹果 HEIC 图像支持: %1").arg(st.heicSupported ? QStringLiteral("✅ 已就绪") : QStringLiteral("⚠️ 当前系统未启用 HEIC 写入"))
          << QStringLiteral("• 离线音视频内核 (FFmpeg): %1").arg(ffStatus)
          << QStringLiteral("• PDF 排版渲染与提取引擎 (Qt6 PDF / 原生内核): ✅ 已内置就绪")
          << QStringLiteral("• EPUB 3.0 标准打包与解析引擎 (C++ ZIP/OPF): ✅ 已内置就绪")
          << QStringLiteral("• Word 文档引擎 (原生 OpenXML DOCX): ✅ 已内置就绪")
          << QStringLiteral("• MOBI/AZW3 电子书引擎 (内置 PalmDOC/MOBI6): ✅ 已内置就绪")
          << QStringLiteral("• GB18030/Big5/UTF-8 智能编码引擎: ✅ 已内置就绪")
          << QStringLiteral("• Calibre 增强插件 (可选): %1").arg(calStatus)
          << QStringLiteral("\n便携配置文件位置:\n%1").arg(omni::core::pathToQString(st.configPath));

    QMessageBox::information(this, QStringLiteral("离线引擎健康自检"), lines.join('\n'));
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData() && event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    } else {
        QMainWindow::dragEnterEvent(event);
    }
}

void MainWindow::dropEvent(QDropEvent* event) {
    QStringList paths;
    if (event->mimeData()) {
        for (const QUrl& u : event->mimeData()->urls()) {
            const QString local = u.toLocalFile();
            if (!local.isEmpty()) paths << local;
        }
    }
    if (!paths.isEmpty()) {
        handleDroppedPaths(paths);
    }
    event->acceptProposedAction();
}

void MainWindow::handleDroppedPaths(const QStringList& rawPaths) {
    if (worker_ && worker_->isRunning()) return;

    std::vector<std::filesystem::path> allFiles;
    for (const QString& qp : rawPaths) {
        QFileInfo fi(qp);
        if (!fi.exists()) continue;
        if (fi.isFile()) {
            allFiles.push_back(omni::core::qStringToPath(fi.absoluteFilePath()));
        } else if (fi.isDir()) {
            QStringList dirFiles;
            QDirIterator it(fi.absoluteFilePath(), QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString fPath = it.next();
                if (!QFileInfo(fPath).fileName().startsWith('.')) {
                    dirFiles << fPath;
                }
            }
            dirFiles.sort();
            for (const QString& fPath : dirFiles) {
                allFiles.push_back(omni::core::qStringToPath(fPath));
            }
        }
    }

    if (allFiles.empty()) return;

    std::vector<std::filesystem::path> accepted;
    for (const auto& f : allFiles) {
        if (omni::core::isFileAcceptedInMode(f, currentMode_)) {
            accepted.push_back(f);
        }
    }

    bool autoSwitched = false;
    WorkMode switchedMode = currentMode_;

    // Smart auto-switch when queue is empty and dropped files belong to another workshop module
    if (accepted.empty() && fileRecords_.empty()) {
        std::map<WorkMode, int> counts;
        for (const auto& f : allFiles) {
            if (const auto m = omni::core::detectCategoryForFile(f)) {
                counts[*m]++;
            }
        }
        if (!counts.empty()) {
            auto bestIt = std::max_element(counts.begin(), counts.end(), [](const auto& a, const auto& b) {
                return a.second < b.second;
            });
            switchedMode = bestIt->first;
            navButtons_[switchedMode]->setChecked(true);
            switchMode(switchedMode);
            autoSwitched = true;

            for (const auto& f : allFiles) {
                if (omni::core::isFileAcceptedInMode(f, currentMode_)) {
                    accepted.push_back(f);
                }
            }
        }
    }

    if (accepted.empty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("未匹配到支持的文件"),
            QStringLiteral("所选文件不属于当前工坊支持的格式，请检查文件后缀或切换到对应分类模块。"));
        return;
    }

    const int added = appendFilesToTable(accepted);
    if (autoSwitched) {
        summaryLbl_->setText(
            QStringLiteral("✨ 已智能识别文件类型并切换至【%1】 · 导入 %2 个文件")
                .arg(QString::fromUtf8(omni::core::modeDisplayName(switchedMode).c_str()))
                .arg(added));
    } else {
        summaryLbl_->setText(
            QStringLiteral("已导入 %1 个文件 · 当前队列共 %2 个任务").arg(added).arg(fileRecords_.size()));
    }
}

void MainWindow::browseAddFiles() {
    QString filter;
    switch (currentMode_) {
        case WorkMode::Image:
            filter = QStringLiteral("图片文件 (*.jpg *.jpeg *.png *.webp *.avif *.heic *.heif *.bmp *.tiff *.tif *.gif *.ico)");
            break;
        case WorkMode::Audio:
            filter = QStringLiteral("音频与视频文件 (*.mp3 *.wav *.flac *.aac *.m4a *.ogg *.opus *.wma *.aiff *.ape *.amr *.mp4 *.mkv *.mov *.avi *.webm)");
            break;
        case WorkMode::Video:
            filter = QStringLiteral("视频文件 (*.mp4 *.mkv *.mov *.avi *.webm *.flv *.wmv *.m4v *.ts *.mts *.mpg *.mpeg *.3gp)");
            break;
        case WorkMode::Novel:
            filter = QStringLiteral("小说与文档 (*.txt *.epub *.mobi *.azw *.azw3 *.pdf *.docx *.fb2 *.md *.html *.htm)");
            break;
    }
    const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("选择需要转换的文件"), QString(), filter);
    if (!files.isEmpty()) {
        handleDroppedPaths(files);
    }
}

void MainWindow::browseAddFolder() {
    const QString folder = QFileDialog::getExistingDirectory(this, QStringLiteral("选择包含待处理文件的文件夹"));
    if (!folder.isEmpty()) {
        handleDroppedPaths({folder});
    }
}

int MainWindow::appendFilesToTable(const std::vector<std::filesystem::path>& files) {
    std::set<std::filesystem::path> existing;
    for (const auto& rec : fileRecords_) {
        std::error_code ec;
        existing.insert(std::filesystem::weakly_canonical(rec.inPath, ec));
    }

    int added = 0;
    for (const auto& f : files) {
        std::error_code ec;
        const auto canon = std::filesystem::weakly_canonical(f, ec);
        if (!existing.insert(canon).second) continue;

        const std::uintmax_t sizeB = std::filesystem::file_size(f, ec);
        const int row = table_->rowCount();
        table_->insertRow(row);

        auto* nameItem = new QTableWidgetItem(omni::core::pathToQString(f.filename()));
        nameItem->setToolTip(omni::core::pathToQString(f));

        QString extLabel = omni::core::pathToQString(f.extension()).toUpper();
        if (extLabel.startsWith('.')) extLabel.remove(0, 1);
        if (extLabel.isEmpty()) extLabel = QStringLiteral("FILE");

        const QString origDesc = QStringLiteral("%1 · %2")
                                     .arg(extLabel, QString::fromUtf8(omni::core::formatByteSize(sizeB).c_str()));
        auto* origItem = new QTableWidgetItem(origDesc);
        origItem->setTextAlignment(Qt::AlignCenter);

        auto* afterItem = new QTableWidgetItem(QStringLiteral("—"));
        afterItem->setTextAlignment(Qt::AlignCenter);

        auto* progBar = new QProgressBar(table_);
        progBar->setRange(0, 100);
        progBar->setValue(0);
        table_->setCellWidget(row, 3, progBar);

        auto* statusItem = new QTableWidgetItem(QStringLiteral("⏳ 待处理"));
        statusItem->setTextAlignment(Qt::AlignCenter);

        table_->setItem(row, 0, nameItem);
        table_->setItem(row, 1, origItem);
        table_->setItem(row, 2, afterItem);
        table_->setItem(row, 4, statusItem);

        fileRecords_.push_back(FileRecord{f, sizeB, {}, false});
        ++added;
    }
    return added;
}

void MainWindow::showTableContextMenu(const QPoint& pos) {
    QMenu menu(this);
    QAction* actReveal = menu.addAction(QStringLiteral("📂 在文件夹中显示"));
    QAction* actRemove = menu.addAction(QStringLiteral("🗑️ 移除选中项"));
    QAction* chosen = menu.exec(table_->viewport()->mapToGlobal(pos));
    if (chosen == actRemove) {
        removeSelectedRows();
    } else if (chosen == actReveal) {
        std::set<int> rows;
        for (const QModelIndex& idx : table_->selectionModel()->selectedIndexes()) {
            rows.insert(idx.row());
        }
        if (!rows.empty()) {
            onRowDoubleClicked(*rows.begin(), 0);
        }
    }
}

void MainWindow::onRowDoubleClicked(int row, int /*column*/) {
    if (row >= 0 && static_cast<std::size_t>(row) < fileRecords_.size()) {
        const auto& rec = fileRecords_[static_cast<std::size_t>(row)];
        std::error_code ec;
        const std::filesystem::path target = (!rec.outPath.empty() && std::filesystem::exists(rec.outPath, ec))
                                                 ? rec.outPath
                                                 : rec.inPath;
        omni::infra::PortableEnv::revealInFileManager(target);
    }
}

void MainWindow::removeSelectedRows() {
    if (worker_ && worker_->isRunning()) return;
    std::set<int, std::greater<int>> rowsDesc;
    for (const QModelIndex& idx : table_->selectionModel()->selectedIndexes()) {
        rowsDesc.insert(idx.row());
    }
    for (int r : rowsDesc) {
        table_->removeRow(r);
        fileRecords_.erase(fileRecords_.begin() + r);
    }
    summaryLbl_->setText(QStringLiteral("当前队列剩余 %1 个任务").arg(fileRecords_.size()));
}

void MainWindow::clearCompletedRows() {
    if (worker_ && worker_->isRunning()) return;
    for (int r = static_cast<int>(fileRecords_.size()) - 1; r >= 0; --r) {
        if (fileRecords_[static_cast<std::size_t>(r)].done) {
            table_->removeRow(r);
            fileRecords_.erase(fileRecords_.begin() + r);
        }
    }
    summaryLbl_->setText(QStringLiteral("已清理完成项 · 剩余 %1 个任务").arg(fileRecords_.size()));
}

void MainWindow::clearAllRows() {
    if (worker_ && worker_->isRunning()) return;
    table_->setRowCount(0);
    fileRecords_.clear();
    batchBar_->setValue(0);
    summaryLbl_->setText(QStringLiteral("列表已清空"));
}

void MainWindow::onOutModeChanged(int index) {
    const bool custom = (index == 1);
    outDirEdit_->setEnabled(custom);
    btnPickDir_->setEnabled(custom);
    saveSettings();
}

void MainWindow::pickOutputDir() {
    const QString d = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择输出文件夹"), outDirEdit_->text().trimmed());
    if (!d.isEmpty()) {
        outDirEdit_->setText(d);
        saveSettings();
    }
}

void MainWindow::openCurrentOutputDir() {
    if (outModeCombo_->currentIndex() == 1 && !outDirEdit_->text().trimmed().isEmpty()) {
        const auto p = omni::core::qStringToPath(outDirEdit_->text().trimmed());
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        omni::infra::PortableEnv::revealInFileManager(p);
        return;
    }
    if (!fileRecords_.empty()) {
        const auto& last = fileRecords_.back();
        std::error_code ec;
        if (!last.outPath.empty() && std::filesystem::exists(last.outPath, ec)) {
            omni::infra::PortableEnv::revealInFileManager(last.outPath);
        } else {
            omni::infra::PortableEnv::revealInFileManager(last.inPath.parent_path());
        }
    } else {
        omni::infra::PortableEnv::revealInFileManager(omni::infra::PortableEnv::appRootDir());
    }
}

void MainWindow::onStartOrStopClicked() {
    if (worker_ && worker_->isRunning()) {
        worker_->requestCancel();
        startBtn_->setEnabled(false);
        startBtn_->setText(QStringLiteral("正在中止..."));
        return;
    }

    if (fileRecords_.empty()) {
        QMessageBox::information(this, QStringLiteral("列表为空"),
                                 QStringLiteral("请先添加或拖入需要转换压缩的文件。"));
        return;
    }

    std::filesystem::path outDir;
    if (outModeCombo_->currentIndex() == 1) {
        const QString customStr = outDirEdit_->text().trimmed();
        if (customStr.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("未指定输出目录"),
                                 QStringLiteral("请选择统一保存的目标文件夹，或切换回保存在原文件目录。"));
            return;
        }
        outDir = omni::core::qStringToPath(customStr);
    }

    std::vector<omni::core::TaskItem> tasks;
    tasks.reserve(fileRecords_.size());
    rowProgressMap_.clear();

    for (std::size_t idx = 0; idx < fileRecords_.size(); ++idx) {
        const int row = static_cast<int>(idx);
        fileRecords_[idx].done = false;
        fileRecords_[idx].outPath.clear();
        rowProgressMap_[row] = 0;

        if (auto* bar = qobject_cast<QProgressBar*>(table_->cellWidget(row, 3))) {
            bar->setValue(0);
        }
        if (auto* item2 = table_->item(row, 2)) {
            item2->setText(QStringLiteral("—"));
            item2->setData(Qt::ForegroundRole, QVariant());
        }
        if (auto* item4 = table_->item(row, 4)) {
            item4->setText(QStringLiteral("⏳ 排队中"));
            item4->setToolTip(QString());
        }
        tasks.push_back(omni::core::TaskItem{row, fileRecords_[idx].inPath, currentMode_});
    }

    omni::core::ConversionOptions opts;
    opts.outputDir = outDir;
    opts.targetFormat = targetFmtCombo_->currentText().trimmed().toLower().toUtf8().toStdString();
    opts.compLevel = omni::core::compressionFromKey(compCombo_->currentData().toString().toUtf8().toStdString());
    opts.imgMaxDim = imgDimCombo_->currentData().toInt();
    opts.imgStripExif = imgStripExifChk_->isChecked();
    opts.audSampleRate = audSrCombo_->currentData().toInt();
    opts.audLoudnorm = audLoudnormChk_->isChecked();
    opts.vidMaxWidth = vidResCombo_->currentData().toInt();
    opts.vidCodec = vidCodecCombo_->currentData().toString().toUtf8().toStdString();
    opts.vidMute = vidMuteChk_->isChecked();
    opts.novAutoIndent = novIndentChk_->isChecked();
    opts.novPdfPreset = novPdfCombo_->currentData().toString().toUtf8().toStdString();
    opts.novTxtEncoding = novEncCombo_->currentData().toString().toUtf8().toStdString();
    opts.novCustomAuthor = novAuthorEdit_->text().trimmed().toUtf8().toStdString();

    completedCount_ = 0;
    totalTasks_ = static_cast<int>(tasks.size());
    batchBar_->setRange(0, totalTasks_ * 100);
    batchBar_->setValue(0);

    startBtn_->setObjectName(QStringLiteral("StopBtn"));
    startBtn_->setText(QStringLiteral("⏹ 停止转换"));
    applyTheme();

    if (worker_) {
        worker_->deleteLater();
        worker_ = nullptr;
    }
    worker_ = new omni::core::QtSchedulerBridge(std::move(tasks), std::move(opts), this);
    connect(worker_, &omni::core::QtSchedulerBridge::taskProgress, this, &MainWindow::onTaskProgress);
    connect(worker_, &omni::core::QtSchedulerBridge::taskFinished, this, &MainWindow::onTaskFinished);
    connect(worker_, &omni::core::QtSchedulerBridge::batchFinished, this, &MainWindow::onBatchFinished);
    worker_->start();
}

void MainWindow::onTaskProgress(int rowId, int pct, const QString& statusText) {
    if (rowId >= 0 && rowId < table_->rowCount()) {
        if (auto* bar = qobject_cast<QProgressBar*>(table_->cellWidget(rowId, 3))) {
            bar->setValue(pct);
        }
        if (auto* stItem = table_->item(rowId, 4)) {
            stItem->setText(statusText);
        }
        rowProgressMap_[rowId] = pct;
        int sumPct = 0;
        for (const auto& kv : rowProgressMap_) sumPct += kv.second;
        batchBar_->setValue(sumPct);

        if (auto* nameItem = table_->item(rowId, 0)) {
            summaryLbl_->setText(
                QStringLiteral("正在处理 (%1/%2)：%3")
                    .arg(std::min(completedCount_ + 1, totalTasks_))
                    .arg(totalTasks_)
                    .arg(nameItem->text()));
        }
    }
}

void MainWindow::onTaskFinished(int rowId, bool ok, const QString& outPath, const QString& msg, quint64 outBytes) {
    ++completedCount_;
    rowProgressMap_[rowId] = 100;
    int sumPct = 0;
    for (const auto& kv : rowProgressMap_) sumPct += kv.second;
    batchBar_->setValue(sumPct);

    if (rowId >= 0 && static_cast<std::size_t>(rowId) < fileRecords_.size()) {
        auto& rec = fileRecords_[static_cast<std::size_t>(rowId)];
        if (auto* bar = qobject_cast<QProgressBar*>(table_->cellWidget(rowId, 3))) {
            bar->setValue(ok ? 100 : 0);
        }
        if (auto* stItem = table_->item(rowId, 4)) {
            const QString shortMsg = (msg.size() <= 28) ? msg : (msg.left(26) + QStringLiteral("..."));
            stItem->setText(shortMsg);
            stItem->setToolTip(msg);
        }

        if (ok && outBytes > 0) {
            rec.done = true;
            rec.outPath = omni::core::qStringToPath(outPath);
            if (auto* afterItem = table_->item(rowId, 2)) {
                if (rec.inSize > 0) {
                    const double ratio = (static_cast<double>(outBytes) - static_cast<double>(rec.inSize)) /
                                         static_cast<double>(rec.inSize) * 100.0;
                    const QString ratioStr = QString::asprintf("%+.1f%%", ratio);
                    afterItem->setText(QStringLiteral("%1 (%2)").arg(
                        QString::fromUtf8(omni::core::formatByteSize(outBytes).c_str()), ratioStr));
                    if (ratio < 0.0) {
                        afterItem->setForeground(QColor(QStringLiteral("#10b981")));
                    }
                } else {
                    afterItem->setText(QString::fromUtf8(omni::core::formatByteSize(outBytes).c_str()));
                }
            }
        }
    }
}

void MainWindow::onBatchFinished(int successCnt, int failCnt, quint64 savedBytes) {
    startBtn_->setEnabled(true);
    startBtn_->setObjectName(QStringLiteral("PrimaryBtn"));
    startBtn_->setText(QStringLiteral("🚀 开始批量转换"));
    applyTheme();

    const QString savedTip = (savedBytes > 0)
                                 ? QStringLiteral(" · 累计节省空间 %1")
                                       .arg(QString::fromUtf8(omni::core::formatByteSize(savedBytes).c_str()))
                                 : QString();
    summaryLbl_->setText(
        QStringLiteral("🎉 批量任务结束：成功 %1 个，失败/中止 %2 个%3（双击已完成行可定位文件）")
            .arg(successCnt)
            .arg(failCnt)
            .arg(savedTip));
}

} // namespace omni::ui
