#include "omni/ui/main_window.hpp"
#include "omni/engines/conversion_engine.hpp"
#include "omni/engines/image_engine.hpp"
#include "omni/engines/novel_engine.hpp"
#include "omni/ui/drop_zone_frame.hpp"
#include "omni/ui/theme.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <system_error>

namespace omni::ui {

using omni::core::CompressionLevel;
using omni::core::WorkMode;

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      settings_(omni::infra::PortableEnv::loadSettings()),
      darkMode_(settings_.darkMode) {

    setWindowTitle(QStringLiteral("OmniConvert 万能格式工坊 Pro v3.0 — 纯本地离线隐私引擎"));
    resize(1180, 820);
    setMinimumSize(980, 680);
    setAcceptDrops(true);

    buildUi();
    applyTheme();
    restoreSavedUiState();
    refreshEngineStatus();
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent* event) {
    if (worker_ && worker_->isRunning()) {
        const auto ans = QMessageBox::question(
            this,
            QStringLiteral("任务正在执行"),
            QStringLiteral("当前仍有文件正在转换中，确定要中止任务并退出吗？"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (ans != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        worker_->requestCancel();
        worker_->wait(4000);
    }
    saveSettings();
    event->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData() && event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent* event) {
    if (!event->mimeData() || !event->mimeData()->hasUrls()) return;
    QStringList paths;
    for (const QUrl& u : event->mimeData()->urls()) {
        if (u.isLocalFile()) paths << u.toLocalFile();
    }
    if (!paths.isEmpty()) {
        handleDroppedPaths(paths);
        event->acceptProposedAction();
    }
}

void MainWindow::buildUi() {
    auto* root = new QWidget(this);
    setCentralWidget(root);
    auto* rootLayout = new QHBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // ==================== Left Navigation Sidebar ====================
    auto* sidebar = new QFrame(root);
    sidebar->setObjectName(QStringLiteral("Sidebar"));
    sidebar->setFixedWidth(226);
    auto* sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(16, 20, 16, 16);
    sideLayout->setSpacing(6);

    auto* brandRow = new QHBoxLayout();
    auto* logoLbl = new QLabel(sidebar);
    const QPixmap logoPix(QStringLiteral(":/app_icon.png"));
    if (!logoPix.isNull()) {
        logoLbl->setPixmap(logoPix.scaled(30, 30, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    auto* titleCol = new QVBoxLayout();
    titleCol->setSpacing(1);
    auto* appTitle = new QLabel(QStringLiteral("OmniConvert Pro"), sidebar);
    appTitle->setObjectName(QStringLiteral("AppTitle"));
    auto* appSub = new QLabel(QStringLiteral("离线全能格式与效率工坊"), sidebar);
    appSub->setObjectName(QStringLiteral("Muted"));
    titleCol->addWidget(appTitle);
    titleCol->addWidget(appSub);
    brandRow->addWidget(logoLbl);
    brandRow->addLayout(titleCol, 1);
    sideLayout->addLayout(brandRow);
    sideLayout->addSpacing(12);

    navGroup_ = new QButtonGroup(this);
    navGroup_->setExclusive(true);

    struct NavItemDef {
        WorkMode mode;
        const char* label;
    };
    static const NavItemDef navDefs[] = {
        {WorkMode::Image,    "🖼️  图片转换与精修"},
        {WorkMode::Audio,    "🎵  音频提取与变速裁剪"},
        {WorkMode::Video,    "🎬  视频转码与瘦身截取"},
        {WorkMode::Novel,    "📚  小说电子书定制工坊"},
        {WorkMode::Document, "📄  PDF 与数据表格工坊"},
        {WorkMode::Toolbox,  "🧰  生活效率百宝箱"}
    };

    for (const auto& nd : navDefs) {
        auto* btn = new QPushButton(QString::fromUtf8(nd.label), sidebar);
        btn->setObjectName(QStringLiteral("NavBtn"));
        btn->setCheckable(true);
        btn->setCursor(Qt::PointingHandCursor);
        navGroup_->addButton(btn, static_cast<int>(nd.mode));
        navButtons_[nd.mode] = btn;
        sideLayout->addWidget(btn);
        connect(btn, &QPushButton::clicked, this, [this, m = nd.mode]() { switchMode(m); });
    }

    sideLayout->addStretch(1);

    auto* privacyLbl = new QLabel(QStringLiteral("🔒 100% 本地离线运算\n零上传 · 保护个人文件隐私"), sidebar);
    privacyLbl->setObjectName(QStringLiteral("Muted"));
    sideLayout->addWidget(privacyLbl);
    sideLayout->addSpacing(6);

    engineStatusBtn_ = new QPushButton(QStringLiteral("⚙️ 离线引擎自检..."), sidebar);
    engineStatusBtn_->setCursor(Qt::PointingHandCursor);
    connect(engineStatusBtn_, &QPushButton::clicked, this, &MainWindow::showEngineReportDialog);
    sideLayout->addWidget(engineStatusBtn_);

    themeBtn_ = new QPushButton(sidebar);
    themeBtn_->setCursor(Qt::PointingHandCursor);
    connect(themeBtn_, &QPushButton::clicked, this, &MainWindow::toggleTheme);
    sideLayout->addWidget(themeBtn_);

    rootLayout->addWidget(sidebar);

    // ==================== Right Workspace Area ====================
    auto* mainArea = new QWidget(root);
    auto* mainLayout = new QVBoxLayout(mainArea);
    mainLayout->setContentsMargins(18, 16, 18, 16);
    mainLayout->setSpacing(11);

    // 1. DropZone Banner
    dropZone_ = new DropZoneFrame(mainArea);
    dropZone_->setMinimumHeight(88);
    auto* dzLayout = new QVBoxLayout(dropZone_);
    dzLayout->setContentsMargins(18, 12, 18, 12);
    dzLayout->setSpacing(4);

    modeHeadlineLbl_ = new QLabel(dropZone_);
    modeHeadlineLbl_->setAlignment(Qt::AlignCenter);
    modeHeadlineLbl_->setObjectName(QStringLiteral("SectionTitle"));

    modeHintLbl_ = new QLabel(dropZone_);
    modeHintLbl_->setAlignment(Qt::AlignCenter);
    modeHintLbl_->setObjectName(QStringLiteral("Muted"));
    modeHintLbl_->setWordWrap(true);

    dzLayout->addWidget(modeHeadlineLbl_);
    dzLayout->addWidget(modeHintLbl_);
    connect(dropZone_, &DropZoneFrame::pathsDropped, this, &MainWindow::handleDroppedPaths);
    mainLayout->addWidget(dropZone_);

    // 2. Action Toolbar (File Queue Controls + Reorder)
    auto* barRow = new QHBoxLayout();
    barRow->setSpacing(7);

    auto* btnAddFile = new QPushButton(QStringLiteral("➕ 添加文件"), mainArea);
    auto* btnAddDir  = new QPushButton(QStringLiteral("📂 导入文件夹"), mainArea);
    auto* btnMoveUp  = new QPushButton(QStringLiteral("⬆️ 上移"), mainArea);
    auto* btnMoveDn  = new QPushButton(QStringLiteral("⬇️ 下移"), mainArea);
    auto* btnRemSel  = new QPushButton(QStringLiteral("🗑️ 移除选中"), mainArea);
    auto* btnClrDone = new QPushButton(QStringLiteral("🧹 清除已完成"), mainArea);
    auto* btnClrAll  = new QPushButton(QStringLiteral("✨ 清空列表"), mainArea);

    btnMoveUp->setToolTip(QStringLiteral("调整队列顺序（在多图合成为 PDF 或多份 PDF 合并时决定页面先后顺序）"));
    btnMoveDn->setToolTip(QStringLiteral("调整队列顺序（在多图合成为 PDF 或多份 PDF 合并时决定页面先后顺序）"));

    connect(btnAddFile, &QPushButton::clicked, this, &MainWindow::browseAddFiles);
    connect(btnAddDir,  &QPushButton::clicked, this, &MainWindow::browseAddFolder);
    connect(btnMoveUp,  &QPushButton::clicked, this, &MainWindow::moveSelectedRowUp);
    connect(btnMoveDn,  &QPushButton::clicked, this, &MainWindow::moveSelectedRowDown);
    connect(btnRemSel,  &QPushButton::clicked, this, &MainWindow::removeSelectedRows);
    connect(btnClrDone, &QPushButton::clicked, this, &MainWindow::clearCompletedRows);
    connect(btnClrAll,  &QPushButton::clicked, this, &MainWindow::clearAllRows);

    barRow->addWidget(btnAddFile);
    barRow->addWidget(btnAddDir);
    barRow->addWidget(btnMoveUp);
    barRow->addWidget(btnMoveDn);
    barRow->addStretch(1);
    barRow->addWidget(btnRemSel);
    barRow->addWidget(btnClrDone);
    barRow->addWidget(btnClrAll);
    mainLayout->addLayout(barRow);

    // 3. Task Table
    table_ = new QTableWidget(0, 5, mainArea);
    table_->setHorizontalHeaderLabels({
        QStringLiteral("文件名"),
        QStringLiteral("源格式"),
        QStringLiteral("原始大小"),
        QStringLiteral("转换后 / 节省比例"),
        QStringLiteral("状态与进度（双击打开所在位置）")
    });
    table_->setAlternatingRowColors(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->setVisible(false);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);

    auto* hdr = table_->horizontalHeader();
    hdr->setSectionResizeMode(0, QHeaderView::Stretch);
    hdr->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    hdr->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    hdr->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    hdr->setSectionResizeMode(4, QHeaderView::Interactive);
    table_->setColumnWidth(4, 240);

    connect(table_, &QTableWidget::customContextMenuRequested, this, &MainWindow::showTableContextMenu);
    connect(table_, &QTableWidget::cellDoubleClicked, this, &MainWindow::onRowDoubleClicked);
    mainLayout->addWidget(table_, 1);

    // 4. Fine-Grained Parameter Card
    auto* paramCard = new QFrame(mainArea);
    paramCard->setObjectName(QStringLiteral("Card"));
    auto* paramLayout = new QVBoxLayout(paramCard);
    paramLayout->setContentsMargins(14, 11, 14, 11);
    paramLayout->setSpacing(9);

    auto* topParamRow = new QHBoxLayout();
    topParamRow->setSpacing(10);

    targetFmtLbl_ = new QLabel(QStringLiteral("目标输出格式:"), paramCard);
    targetFmtCombo_ = new QComboBox(paramCard);
    targetFmtCombo_->setMinimumWidth(145);

    compLabel_ = new QLabel(QStringLiteral("压缩与画质档位:"), paramCard);
    compCombo_ = new QComboBox(paramCard);
    compCombo_->setMinimumWidth(295);
    for (const auto& preset : omni::core::compressionPresets()) {
        compCombo_->addItem(QString::fromUtf8(preset.description), static_cast<int>(preset.level));
    }
    compCombo_->setCurrentIndex(2); // Balanced default
    connect(compCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onCompressionChanged);

    topParamRow->addWidget(targetFmtLbl_);
    topParamRow->addWidget(targetFmtCombo_);
    topParamRow->addSpacing(12);
    topParamRow->addWidget(compLabel_);
    topParamRow->addWidget(compCombo_);
    topParamRow->addStretch(1);
    paramLayout->addLayout(topParamRow);

    subStack_ = new QStackedWidget(paramCard);
    subStack_->addWidget(buildImageSubpanel());    // 0: Image
    subStack_->addWidget(buildAudioSubpanel());    // 1: Audio
    subStack_->addWidget(buildVideoSubpanel());    // 2: Video
    subStack_->addWidget(buildNovelSubpanel());    // 3: Novel
    subStack_->addWidget(buildDocumentSubpanel()); // 4: Document
    subStack_->addWidget(buildToolboxSubpanel());  // 5: Toolbox
    paramLayout->addWidget(subStack_);

    mainLayout->addWidget(paramCard);

    // 5. Output Directory & Batch Execution Footer Card
    auto* footCard = new QFrame(mainArea);
    footCard->setObjectName(QStringLiteral("Card"));
    auto* footLayout = new QVBoxLayout(footCard);
    footLayout->setContentsMargins(14, 10, 14, 10);
    footLayout->setSpacing(8);

    auto* outRow = new QHBoxLayout();
    outRow->setSpacing(8);
    outRow->addWidget(new QLabel(QStringLiteral("输出目录:"), footCard));

    outModeCombo_ = new QComboBox(footCard);
    outModeCombo_->addItem(QStringLiteral("保存在源文件同级目录（自动防重名覆盖）"));
    outModeCombo_->addItem(QStringLiteral("指定统一输出文件夹"));
    connect(outModeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onOutModeChanged);

    outDirEdit_ = new QLineEdit(footCard);
    outDirEdit_->setPlaceholderText(QStringLiteral("未选择（默认输出在原文件旁边）"));
    outDirEdit_->setEnabled(false);

    btnPickDir_ = new QPushButton(QStringLiteral("浏览..."), footCard);
    btnPickDir_->setEnabled(false);
    connect(btnPickDir_, &QPushButton::clicked, this, &MainWindow::pickOutputDir);

    auto* btnOpenDir = new QPushButton(QStringLiteral("📂 打开输出位置"), footCard);
    connect(btnOpenDir, &QPushButton::clicked, this, &MainWindow::openCurrentOutputDir);

    outRow->addWidget(outModeCombo_);
    outRow->addWidget(outDirEdit_, 1);
    outRow->addWidget(btnPickDir_);
    outRow->addWidget(btnOpenDir);
    footLayout->addLayout(outRow);

    auto* runRow = new QHBoxLayout();
    runRow->setSpacing(12);

    summaryLbl_ = new QLabel(QStringLiteral("就绪：拖入文件或点击上方「添加文件」即可开始"), footCard);
    summaryLbl_->setObjectName(QStringLiteral("Muted"));

    batchBar_ = new QProgressBar(footCard);
    batchBar_->setRange(0, 100);
    batchBar_->setValue(0);
    batchBar_->setMinimumWidth(220);

    startBtn_ = new QPushButton(QStringLiteral("🚀 开始批量处理"), footCard);
    startBtn_->setObjectName(QStringLiteral("PrimaryBtn"));
    startBtn_->setCursor(Qt::PointingHandCursor);
    connect(startBtn_, &QPushButton::clicked, this, &MainWindow::onStartOrStopClicked);

    runRow->addWidget(summaryLbl_, 1);
    runRow->addWidget(batchBar_);
    runRow->addWidget(startBtn_);
    footLayout->addLayout(runRow);

    mainLayout->addWidget(footCard);
    rootLayout->addWidget(mainArea, 1);
}

QWidget* MainWindow::buildImageSubpanel() {
    auto* w = new QWidget(this);
    auto* vbox = new QVBoxLayout(w);
    vbox->setContentsMargins(0, 2, 0, 0);
    vbox->setSpacing(7);

    auto* row1 = new QHBoxLayout();
    row1->setSpacing(8);

    row1->addWidget(new QLabel(QStringLiteral("尺寸限制:"), w));
    imgDimCombo_ = new QComboBox(w);
    imgDimCombo_->addItem(QStringLiteral("保持原图尺寸"), 0);
    imgDimCombo_->addItem(QStringLiteral("按百分比缩放 (%)"), -2);
    imgDimCombo_->addItem(QStringLiteral("自定义最大边长 (px)"), -1);
    imgDimCombo_->addItem(QStringLiteral("最长边 ≤ 3840px (4K)"), 3840);
    imgDimCombo_->addItem(QStringLiteral("最长边 ≤ 2560px (2K)"), 2560);
    imgDimCombo_->addItem(QStringLiteral("最长边 ≤ 1920px (1080P)"), 1920);
    imgDimCombo_->addItem(QStringLiteral("最长边 ≤ 1280px (720P)"), 1280);
    imgDimCombo_->addItem(QStringLiteral("最长边 ≤ 800px (网页图)"), 800);
    connect(imgDimCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onImageDimModeChanged);
    row1->addWidget(imgDimCombo_);

    imgCustomDimSpin_ = new QSpinBox(w);
    imgCustomDimSpin_->setRange(16, 16384);
    imgCustomDimSpin_->setValue(1600);
    imgCustomDimSpin_->setSuffix(QStringLiteral(" px"));
    imgCustomDimSpin_->setVisible(false);
    row1->addWidget(imgCustomDimSpin_);

    imgScaleSpin_ = new QSpinBox(w);
    imgScaleSpin_->setRange(10, 400);
    imgScaleSpin_->setValue(50);
    imgScaleSpin_->setSuffix(QStringLiteral(" %"));
    imgScaleSpin_->setVisible(false);
    row1->addWidget(imgScaleSpin_);

    row1->addWidget(new QLabel(QStringLiteral("旋转:"), w));
    imgRotateCombo_ = new QComboBox(w);
    imgRotateCombo_->addItem(QStringLiteral("不旋转"), 0);
    imgRotateCombo_->addItem(QStringLiteral("顺时针 90°"), 90);
    imgRotateCombo_->addItem(QStringLiteral("旋转 180°"), 180);
    imgRotateCombo_->addItem(QStringLiteral("逆时针 90°"), 270);
    row1->addWidget(imgRotateCombo_);

    imgBgColorBtn_ = new QPushButton(QStringLiteral("🎨 透明底色 #FFFFFF"), w);
    imgBgColorBtn_->setToolTip(QStringLiteral("PNG/SVG 等透明图片转为 JPG/BMP 时填充的背景颜色"));
    connect(imgBgColorBtn_, &QPushButton::clicked, this, &MainWindow::pickImageBgColor);
    row1->addWidget(imgBgColorBtn_);

    row1->addStretch(1);
    vbox->addLayout(row1);

    auto* row2 = new QHBoxLayout();
    row2->setSpacing(12);
    imgFlipChk_ = new QCheckBox(QStringLiteral("水平镜像翻转"), w);
    imgGrayChk_ = new QCheckBox(QStringLiteral("黑白扫描去色"), w);
    imgStripExifChk_ = new QCheckBox(QStringLiteral("抹除 EXIF 拍摄与定位隐私"), w);
    imgStripExifChk_->setChecked(true);

    row2->addWidget(imgFlipChk_);
    row2->addWidget(imgGrayChk_);
    row2->addWidget(imgStripExifChk_);
    row2->addSpacing(8);
    row2->addWidget(new QLabel(QStringLiteral("右下角标水印:"), w));
    imgWatermarkEdit_ = new QLineEdit(w);
    imgWatermarkEdit_->setPlaceholderText(QStringLiteral("选填，如：© 摄影署名（留空无水印）"));
    imgWatermarkEdit_->setMaximumWidth(230);
    row2->addWidget(imgWatermarkEdit_);
    row2->addStretch(1);
    vbox->addLayout(row2);

    // Precision row shown when Compression == Custom
    imgCustomFrame_ = new QFrame(w);
    imgCustomFrame_->setObjectName(QStringLiteral("SubCard"));
    auto* cRow = new QHBoxLayout(imgCustomFrame_);
    cRow->setContentsMargins(10, 6, 10, 6);
    cRow->setSpacing(10);

    cRow->addWidget(new QLabel(QStringLiteral("🎛️ 编码画质 (1-100):"), imgCustomFrame_));
    imgQualitySlider_ = new QSlider(Qt::Horizontal, imgCustomFrame_);
    imgQualitySlider_->setRange(1, 100);
    imgQualitySlider_->setValue(82);
    imgQualitySlider_->setMaximumWidth(170);

    imgQualitySpin_ = new QSpinBox(imgCustomFrame_);
    imgQualitySpin_->setRange(1, 100);
    imgQualitySpin_->setValue(82);
    connect(imgQualitySlider_, &QSlider::valueChanged, imgQualitySpin_, &QSpinBox::setValue);
    connect(imgQualitySpin_, QOverload<int>::of(&QSpinBox::valueChanged), imgQualitySlider_, &QSlider::setValue);

    cRow->addWidget(imgQualitySlider_);
    cRow->addWidget(imgQualitySpin_);
    cRow->addSpacing(12);
    cRow->addWidget(new QLabel(QStringLiteral("PNG/GIF 调色板量化:"), imgCustomFrame_));
    imgPaletteCombo_ = new QComboBox(imgCustomFrame_);
    imgPaletteCombo_->addItem(QStringLiteral("真彩色无损 (不减色)"), 0);
    imgPaletteCombo_->addItem(QStringLiteral("256 色高保真"), 256);
    imgPaletteCombo_->addItem(QStringLiteral("128 色均衡瘦身"), 128);
    imgPaletteCombo_->addItem(QStringLiteral("64 色高压缩"), 64);
    imgPaletteCombo_->addItem(QStringLiteral("32 色极简图标"), 32);
    imgPaletteCombo_->addItem(QStringLiteral("16 色极限体积"), 16);
    cRow->addWidget(imgPaletteCombo_);
    cRow->addStretch(1);
    imgCustomFrame_->setVisible(false);

    vbox->addWidget(imgCustomFrame_);
    return w;
}

QWidget* MainWindow::buildAudioSubpanel() {
    auto* w = new QWidget(this);
    auto* vbox = new QVBoxLayout(w);
    vbox->setContentsMargins(0, 2, 0, 0);
    vbox->setSpacing(7);

    auto* row1 = new QHBoxLayout();
    row1->setSpacing(9);

    row1->addWidget(new QLabel(QStringLiteral("采样率:"), w));
    audSrCombo_ = new QComboBox(w);
    audSrCombo_->addItem(QStringLiteral("保持原采样率"), 0);
    audSrCombo_->addItem(QStringLiteral("96000 Hz (Hi-Res 母带)"), 96000);
    audSrCombo_->addItem(QStringLiteral("48000 Hz (影视标准)"), 48000);
    audSrCombo_->addItem(QStringLiteral("44100 Hz (CD 级)"), 44100);
    audSrCombo_->addItem(QStringLiteral("22050 Hz (有声书)"), 22050);
    audSrCombo_->addItem(QStringLiteral("16000 Hz (AI 语音识别专用)"), 16000);
    audSrCombo_->addItem(QStringLiteral("8000 Hz (电话录音体积最小)"), 8000);
    row1->addWidget(audSrCombo_);

    row1->addWidget(new QLabel(QStringLiteral("声道:"), w));
    audChCombo_ = new QComboBox(w);
    audChCombo_->addItem(QStringLiteral("保持原声道"), 0);
    audChCombo_->addItem(QStringLiteral("立体声 (2.0)"), 2);
    audChCombo_->addItem(QStringLiteral("单声道 (1.0 语音推荐)"), 1);
    row1->addWidget(audChCombo_);

    row1->addWidget(new QLabel(QStringLiteral("倍速:"), w));
    audSpeedCombo_ = new QComboBox(w);
    audSpeedCombo_->addItem(QStringLiteral("0.5x 慢放"), 0.5);
    audSpeedCombo_->addItem(QStringLiteral("0.75x 慢速精听"), 0.75);
    audSpeedCombo_->addItem(QStringLiteral("1.0x 原速"), 1.0);
    audSpeedCombo_->addItem(QStringLiteral("1.25x 轻快"), 1.25);
    audSpeedCombo_->addItem(QStringLiteral("1.5x 倍速刷课/播客"), 1.5);
    audSpeedCombo_->addItem(QStringLiteral("2.0x 极速"), 2.0);
    audSpeedCombo_->setCurrentIndex(2);
    row1->addWidget(audSpeedCombo_);

    audLoudnormChk_ = new QCheckBox(QStringLiteral("EBU R128 智能响度均衡"), w);
    row1->addWidget(audLoudnormChk_);
    row1->addStretch(1);
    vbox->addLayout(row1);

    auto* row2 = new QHBoxLayout();
    row2->setSpacing(8);
    row2->addWidget(new QLabel(QStringLiteral("✂️ 区间裁剪（做铃声/截片段，留空转换全曲）:"), w));
    audTrimStartEdit_ = new QLineEdit(w);
    audTrimStartEdit_->setPlaceholderText(QStringLiteral("起始 如 00:15"));
    audTrimStartEdit_->setMaximumWidth(115);
    audTrimEndEdit_ = new QLineEdit(w);
    audTrimEndEdit_->setPlaceholderText(QStringLiteral("结束 如 00:55"));
    audTrimEndEdit_->setMaximumWidth(115);
    row2->addWidget(audTrimStartEdit_);
    row2->addWidget(new QLabel(QStringLiteral("至"), w));
    row2->addWidget(audTrimEndEdit_);
    row2->addStretch(1);
    vbox->addLayout(row2);

    audCustomFrame_ = new QFrame(w);
    audCustomFrame_->setObjectName(QStringLiteral("SubCard"));
    auto* cRow = new QHBoxLayout(audCustomFrame_);
    cRow->setContentsMargins(10, 6, 10, 6);
    cRow->addWidget(new QLabel(QStringLiteral("🎛️ 自定义目标音频码率:"), audCustomFrame_));
    audBitrateSpin_ = new QSpinBox(audCustomFrame_);
    audBitrateSpin_->setRange(16, 512);
    audBitrateSpin_->setSingleStep(16);
    audBitrateSpin_->setValue(192);
    audBitrateSpin_->setSuffix(QStringLiteral(" kbps"));
    cRow->addWidget(audBitrateSpin_);
    auto* hint = new QLabel(QStringLiteral("（人声播客 48~64kbps 极省空间，高音质音乐建议 192~320kbps）"), audCustomFrame_);
    hint->setObjectName(QStringLiteral("Muted"));
    cRow->addWidget(hint);
    cRow->addStretch(1);
    audCustomFrame_->setVisible(false);

    vbox->addWidget(audCustomFrame_);
    return w;
}

QWidget* MainWindow::buildVideoSubpanel() {
    auto* w = new QWidget(this);
    auto* vbox = new QVBoxLayout(w);
    vbox->setContentsMargins(0, 2, 0, 0);
    vbox->setSpacing(7);

    auto* row1 = new QHBoxLayout();
    row1->setSpacing(8);

    row1->addWidget(new QLabel(QStringLiteral("分辨率上限:"), w));
    vidResCombo_ = new QComboBox(w);
    vidResCombo_->addItem(QStringLiteral("保持原分辨率"), 0);
    vidResCombo_->addItem(QStringLiteral("自定义最大宽度 (px)"), -1);
    vidResCombo_->addItem(QStringLiteral("3840px (4K UHD)"), 3840);
    vidResCombo_->addItem(QStringLiteral("1920px (1080P 全高清)"), 1920);
    vidResCombo_->addItem(QStringLiteral("1280px (720P 均衡)"), 1280);
    vidResCombo_->addItem(QStringLiteral("854px (480P 聊天发送)"), 854);
    vidResCombo_->addItem(QStringLiteral("480px (表情包/动图推荐)"), 480);
    connect(vidResCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onVideoResModeChanged);
    row1->addWidget(vidResCombo_);

    vidCustomWidthSpin_ = new QSpinBox(w);
    vidCustomWidthSpin_->setRange(120, 7680);
    vidCustomWidthSpin_->setSingleStep(16);
    vidCustomWidthSpin_->setValue(1440);
    vidCustomWidthSpin_->setSuffix(QStringLiteral(" px"));
    vidCustomWidthSpin_->setVisible(false);
    row1->addWidget(vidCustomWidthSpin_);

    row1->addWidget(new QLabel(QStringLiteral("编码器:"), w));
    vidCodecCombo_ = new QComboBox(w);
    vidCodecCombo_->addItem(QStringLiteral("H.264 (全平台高兼容)"), QStringLiteral("h264"));
    vidCodecCombo_->addItem(QStringLiteral("H.265 / HEVC (更省体积)"), QStringLiteral("h265"));
    row1->addWidget(vidCodecCombo_);

    row1->addWidget(new QLabel(QStringLiteral("帧率:"), w));
    vidFpsCombo_ = new QComboBox(w);
    vidFpsCombo_->addItem(QStringLiteral("保持原帧率"), 0);
    vidFpsCombo_->addItem(QStringLiteral("60 FPS"), 60);
    vidFpsCombo_->addItem(QStringLiteral("30 FPS"), 30);
    vidFpsCombo_->addItem(QStringLiteral("24 FPS 电影感"), 24);
    vidFpsCombo_->addItem(QStringLiteral("15 FPS 动图顺滑"), 15);
    vidFpsCombo_->addItem(QStringLiteral("10 FPS 动图极小"), 10);
    row1->addWidget(vidFpsCombo_);

    vidMuteChk_ = new QCheckBox(QStringLiteral("剔除音轨 (静音)"), w);
    row1->addWidget(vidMuteChk_);
    row1->addStretch(1);
    vbox->addLayout(row1);

    auto* row2 = new QHBoxLayout();
    row2->setSpacing(8);
    row2->addWidget(new QLabel(QStringLiteral("✂️ 视频片段截取（留空转码全片）:"), w));
    vidTrimStartEdit_ = new QLineEdit(w);
    vidTrimStartEdit_->setPlaceholderText(QStringLiteral("起始 如 00:00:10"));
    vidTrimStartEdit_->setMaximumWidth(125);
    vidTrimEndEdit_ = new QLineEdit(w);
    vidTrimEndEdit_->setPlaceholderText(QStringLiteral("结束 如 00:01:30"));
    vidTrimEndEdit_->setMaximumWidth(125);
    row2->addWidget(vidTrimStartEdit_);
    row2->addWidget(new QLabel(QStringLiteral("至"), w));
    row2->addWidget(vidTrimEndEdit_);
    row2->addStretch(1);
    vbox->addLayout(row2);

    vidCustomFrame_ = new QFrame(w);
    vidCustomFrame_->setObjectName(QStringLiteral("SubCard"));
    auto* cRow = new QHBoxLayout(vidCustomFrame_);
    cRow->setContentsMargins(10, 6, 10, 6);
    cRow->addWidget(new QLabel(QStringLiteral("🎛️ 恒定画质因子 CRF (0无损 ~ 51极限压缩，越小越清晰):"), vidCustomFrame_));
    vidCrfSlider_ = new QSlider(Qt::Horizontal, vidCustomFrame_);
    vidCrfSlider_->setRange(0, 51);
    vidCrfSlider_->setValue(24);
    vidCrfSlider_->setMaximumWidth(170);

    vidCrfSpin_ = new QSpinBox(vidCustomFrame_);
    vidCrfSpin_->setRange(0, 51);
    vidCrfSpin_->setValue(24);
    connect(vidCrfSlider_, &QSlider::valueChanged, vidCrfSpin_, &QSpinBox::setValue);
    connect(vidCrfSpin_, QOverload<int>::of(&QSpinBox::valueChanged), vidCrfSlider_, &QSlider::setValue);

    cRow->addWidget(vidCrfSlider_);
    cRow->addWidget(vidCrfSpin_);
    auto* hint = new QLabel(QStringLiteral("（推荐 18~22 珍藏画质，23~26 日常均衡，28~34 极限缩小体积）"), vidCustomFrame_);
    hint->setObjectName(QStringLiteral("Muted"));
    cRow->addWidget(hint);
    cRow->addStretch(1);
    vidCustomFrame_->setVisible(false);

    vbox->addWidget(vidCustomFrame_);
    return w;
}

QWidget* MainWindow::buildNovelSubpanel() {
    auto* w = new QWidget(this);
    auto* mainHBox = new QHBoxLayout(w);
    mainHBox->setContentsMargins(0, 2, 0, 0);
    mainHBox->setSpacing(12);

    // Left cover thumbnail box
    novCoverThumbLbl_ = new QLabel(QStringLiteral("书封\n预览"), w);
    novCoverThumbLbl_->setObjectName(QStringLiteral("CoverThumb"));
    novCoverThumbLbl_->setFixedSize(50, 70);
    novCoverThumbLbl_->setAlignment(Qt::AlignCenter);
    mainHBox->addWidget(novCoverThumbLbl_);

    auto* rowsBox = new QVBoxLayout();
    rowsBox->setSpacing(7);

    // Row 1: Cover customization + Book Title & Author
    auto* row1 = new QHBoxLayout();
    row1->setSpacing(7);

    row1->addWidget(new QLabel(QStringLiteral("封面设置:"), w));
    novCoverModeCombo_ = new QComboBox(w);
    novCoverModeCombo_->addItem(QStringLiteral("保留原封面 / 不额外添加"), QStringLiteral("keep"));
    novCoverModeCombo_->addItem(QStringLiteral("🖼️ 选择本地图片自定义封面"), QStringLiteral("custom"));
    novCoverModeCombo_->addItem(QStringLiteral("🎨 自动生成典雅书封 (书名+作者)"), QStringLiteral("auto"));
    connect(novCoverModeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onNovelCoverModeChanged);
    row1->addWidget(novCoverModeCombo_);

    novPickCoverBtn_ = new QPushButton(QStringLiteral("📂 选图..."), w);
    novPickCoverBtn_->setVisible(false);
    connect(novPickCoverBtn_, &QPushButton::clicked, this, &MainWindow::pickNovelCoverImage);
    row1->addWidget(novPickCoverBtn_);

    novPreviewCoverBtn_ = new QPushButton(QStringLiteral("👁️ 预览书封"), w);
    novPreviewCoverBtn_->setVisible(false);
    connect(novPreviewCoverBtn_, &QPushButton::clicked, this, &MainWindow::previewProceduralCover);
    row1->addWidget(novPreviewCoverBtn_);

    novCoverPathLbl_ = new QLabel(w);
    novCoverPathLbl_->setObjectName(QStringLiteral("Muted"));
    novCoverPathLbl_->setVisible(false);
    row1->addWidget(novCoverPathLbl_);

    row1->addSpacing(6);
    row1->addWidget(new QLabel(QStringLiteral("书名:"), w));
    novTitleEdit_ = new QLineEdit(w);
    novTitleEdit_->setPlaceholderText(QStringLiteral("留空自动识别"));
    novTitleEdit_->setMaximumWidth(145);
    row1->addWidget(novTitleEdit_);

    row1->addWidget(new QLabel(QStringLiteral("作者:"), w));
    novAuthorEdit_ = new QLineEdit(w);
    novAuthorEdit_->setPlaceholderText(QStringLiteral("留空自动/佚名"));
    novAuthorEdit_->setMaximumWidth(125);
    row1->addWidget(novAuthorEdit_);
    row1->addStretch(1);
    rowsBox->addLayout(row1);

    // Row 2: Zh variant, Typography (indent, font size, line height), PDF preset, TXT encoding
    auto* row2 = new QHBoxLayout();
    row2->setSpacing(7);

    row2->addWidget(new QLabel(QStringLiteral("简繁:"), w));
    novZhCombo_ = new QComboBox(w);
    novZhCombo_->addItem(QStringLiteral("保持原文"), QStringLiteral("none"));
    novZhCombo_->addItem(QStringLiteral("繁 ➔ 简"), QStringLiteral("t2s"));
    novZhCombo_->addItem(QStringLiteral("简 ➔ 繁"), QStringLiteral("s2t"));
    row2->addWidget(novZhCombo_);

    row2->addWidget(new QLabel(QStringLiteral("首行缩进:"), w));
    novIndentCombo_ = new QComboBox(w);
    novIndentCombo_->addItem(QStringLiteral("2 字符标准"), 2);
    novIndentCombo_->addItem(QStringLiteral("4 字符宽缩进"), 4);
    novIndentCombo_->addItem(QStringLiteral("顶格不缩进"), 0);
    row2->addWidget(novIndentCombo_);

    row2->addWidget(new QLabel(QStringLiteral("字号:"), w));
    novFontSizeSpin_ = new QDoubleSpinBox(w);
    novFontSizeSpin_->setRange(9.0, 22.0);
    novFontSizeSpin_->setSingleStep(0.5);
    novFontSizeSpin_->setDecimals(1);
    novFontSizeSpin_->setValue(11.5);
    novFontSizeSpin_->setSuffix(QStringLiteral(" pt"));
    row2->addWidget(novFontSizeSpin_);

    row2->addWidget(new QLabel(QStringLiteral("行距:"), w));
    novLineHeightSpin_ = new QDoubleSpinBox(w);
    novLineHeightSpin_->setRange(1.2, 2.6);
    novLineHeightSpin_->setSingleStep(0.1);
    novLineHeightSpin_->setDecimals(1);
    novLineHeightSpin_->setValue(1.8);
    novLineHeightSpin_->setSuffix(QStringLiteral(" 倍"));
    row2->addWidget(novLineHeightSpin_);

    row2->addWidget(new QLabel(QStringLiteral("PDF版面:"), w));
    novPdfCombo_ = new QComboBox(w);
    novPdfCombo_->addItem(QStringLiteral("A4 标准"), QStringLiteral("a4"));
    novPdfCombo_->addItem(QStringLiteral("B5 书籍"), QStringLiteral("b5"));
    novPdfCombo_->addItem(QStringLiteral("6寸墨水屏"), QStringLiteral("eink6"));
    row2->addWidget(novPdfCombo_);

    row2->addWidget(new QLabel(QStringLiteral("TXT编码:"), w));
    novEncCombo_ = new QComboBox(w);
    novEncCombo_->addItem(QStringLiteral("UTF-8"), QStringLiteral("utf-8"));
    novEncCombo_->addItem(QStringLiteral("GB18030"), QStringLiteral("gb18030"));
    row2->addWidget(novEncCombo_);

    row2->addStretch(1);
    rowsBox->addLayout(row2);

    // Row 3: Ad cleaner + Custom chapter regex
    auto* row3 = new QHBoxLayout();
    row3->setSpacing(10);
    novCleanAdsChk_ = new QCheckBox(QStringLiteral("🧹 自动净化网文网址与求票广告行"), w);
    novCleanAdsChk_->setChecked(true);
    row3->addWidget(novCleanAdsChk_);

    row3->addWidget(new QLabel(QStringLiteral("自定义分章正则:"), w));
    novRegexEdit_ = new QLineEdit(w);
    novRegexEdit_->setPlaceholderText(QStringLiteral("选填，默认已内置中西文章节智能识别（如 ^【\\d+】.*）"));
    row3->addWidget(novRegexEdit_, 1);
    rowsBox->addLayout(row3);

    mainHBox->addLayout(rowsBox, 1);
    return w;
}

QWidget* MainWindow::buildDocumentSubpanel() {
    auto* w = new QWidget(this);
    auto* vbox = new QVBoxLayout(w);
    vbox->setContentsMargins(0, 2, 0, 0);
    vbox->setSpacing(7);

    auto* row1 = new QHBoxLayout();
    row1->setSpacing(10);
    row1->addWidget(new QLabel(QStringLiteral("办公处理模式:"), w));

    docActionCombo_ = new QComboBox(w);
    docActionCombo_->addItem(QStringLiteral("📑 多张图片按顺序合成为单个 PDF（扫描件 / 漫画 / 票据整理）"), QStringLiteral("img_to_pdf"));
    docActionCombo_->addItem(QStringLiteral("📚 多份 PDF 文档无损合并为一册"), QStringLiteral("merge_pdf"));
    docActionCombo_->addItem(QStringLiteral("🖼️ PDF 逐页导出为高清图片文件夹"), QStringLiteral("pdf_to_img"));
    docActionCombo_->addItem(QStringLiteral("📊 数据表格互转与 Excel 中文乱码修复 (CSV/TSV/JSON/MD/HTML)"), QStringLiteral("table_convert"));
    connect(docActionCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onDocumentActionChanged);
    row1->addWidget(docActionCombo_);

    docPagePresetWrap_ = new QWidget(w);
    auto* ppLay = new QHBoxLayout(docPagePresetWrap_);
    ppLay->setContentsMargins(0, 0, 0, 0);
    ppLay->setSpacing(6);
    ppLay->addWidget(new QLabel(QStringLiteral("PDF页面规格:"), docPagePresetWrap_));
    docPagePresetCombo_ = new QComboBox(docPagePresetWrap_);
    docPagePresetCombo_->addItem(QStringLiteral("贴合原图比例（无白边满幅）"), QStringLiteral("fit"));
    docPagePresetCombo_->addItem(QStringLiteral("标准 A4 纵向居中"), QStringLiteral("a4"));
    docPagePresetCombo_->addItem(QStringLiteral("标准 B5 纵向居中"), QStringLiteral("b5"));
    ppLay->addWidget(docPagePresetCombo_);
    row1->addWidget(docPagePresetWrap_);

    docDpiWrap_ = new QWidget(w);
    auto* dpiLay = new QHBoxLayout(docDpiWrap_);
    dpiLay->setContentsMargins(0, 0, 0, 0);
    dpiLay->setSpacing(6);
    dpiLay->addWidget(new QLabel(QStringLiteral("导出清晰度:"), docDpiWrap_));
    docDpiCombo_ = new QComboBox(docDpiWrap_);
    docDpiCombo_->addItem(QStringLiteral("150 DPI (清晰推荐)"), 150);
    docDpiCombo_->addItem(QStringLiteral("300 DPI (印刷级超清)"), 300);
    docDpiCombo_->addItem(QStringLiteral("96 DPI (轻量快速)"), 96);
    dpiLay->addWidget(docDpiCombo_);
    docDpiWrap_->setVisible(false);
    row1->addWidget(docDpiWrap_);

    docMergedNameWrap_ = new QWidget(w);
    auto* mnLay = new QHBoxLayout(docMergedNameWrap_);
    mnLay->setContentsMargins(0, 0, 0, 0);
    mnLay->setSpacing(6);
    mnLay->addWidget(new QLabel(QStringLiteral("合并输出文件名:"), docMergedNameWrap_));
    docMergedNameEdit_ = new QLineEdit(QStringLiteral("合并文档"), docMergedNameWrap_);
    docMergedNameEdit_->setMaximumWidth(160);
    mnLay->addWidget(docMergedNameEdit_);
    row1->addWidget(docMergedNameWrap_);

    row1->addStretch(1);
    vbox->addLayout(row1);

    docHintLbl_ = new QLabel(w);
    docHintLbl_->setObjectName(QStringLiteral("Muted"));
    vbox->addWidget(docHintLbl_);
    return w;
}

QWidget* MainWindow::buildToolboxSubpanel() {
    auto* w = new QWidget(this);
    auto* vbox = new QVBoxLayout(w);
    vbox->setContentsMargins(0, 2, 0, 0);
    vbox->setSpacing(7);

    auto* row1 = new QHBoxLayout();
    row1->setSpacing(10);
    row1->addWidget(new QLabel(QStringLiteral("实用百宝箱功能:"), w));

    toolActionCombo_ = new QComboBox(w);
    toolActionCombo_->addItem(QStringLiteral("🪪 证件照 / 报名表图片精确卡 KB 压缩（严格 ≤ 指定大小）"), QStringLiteral("exact_kb"));
    toolActionCombo_->addItem(QStringLiteral("🛡️ 身份证 / 营业执照 / 合同全屏防盗斜向安全水印"), QStringLiteral("id_watermark"));
    toolActionCombo_->addItem(QStringLiteral("💬 影视字幕格式互转、乱码修复与时间轴校准 (SRT/VTT/ASS/TXT)"), QStringLiteral("subtitle"));
    toolActionCombo_->addItem(QStringLiteral("🔐 批量文件 SHA-256 / MD5 完整性哈希校验清单生成"), QStringLiteral("hash_manifest"));
    connect(toolActionCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onToolboxActionChanged);
    row1->addWidget(toolActionCombo_);
    row1->addStretch(1);
    vbox->addLayout(row1);

    toolDetailStack_ = new QStackedWidget(w);

    // Page 0: Exact KB photo compressor
    {
        auto* p0 = new QWidget(toolDetailStack_);
        auto* lay = new QHBoxLayout(p0);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(8);
        lay->addWidget(new QLabel(QStringLiteral("严格体积上限:"), p0));
        toolTargetKbSpin_ = new QSpinBox(p0);
        toolTargetKbSpin_->setRange(10, 10240);
        toolTargetKbSpin_->setValue(100);
        toolTargetKbSpin_->setSuffix(QStringLiteral(" KB"));
        lay->addWidget(toolTargetKbSpin_);

        lay->addWidget(new QLabel(QStringLiteral("常用报名规格快捷填入:"), p0));
        for (int kb : {20, 30, 50, 100, 200, 500}) {
            auto* chip = new QPushButton(QString::number(kb) + QStringLiteral(" KB"), p0);
            chip->setObjectName(QStringLiteral("ChipBtn"));
            chip->setCursor(Qt::PointingHandCursor);
            connect(chip, &QPushButton::clicked, this, [this, kb]() { toolTargetKbSpin_->setValue(kb); });
            lay->addWidget(chip);
        }
        lay->addStretch(1);
        toolDetailStack_->addWidget(p0);
    }

    // Page 1: ID Card Anti-Theft Security Watermark
    {
        auto* p1 = new QWidget(toolDetailStack_);
        auto* lay = new QHBoxLayout(p1);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(8);
        lay->addWidget(new QLabel(QStringLiteral("防盗水印文案:"), p1));
        toolWatermarkEdit_ = new QLineEdit(QStringLiteral("仅供本次业务办理使用 他用无效"), p1);
        toolWatermarkEdit_->setMinimumWidth(260);
        lay->addWidget(toolWatermarkEdit_, 1);

        lay->addWidget(new QLabel(QStringLiteral("水印浓度:"), p1));
        toolWmOpacitySlider_ = new QSlider(Qt::Horizontal, p1);
        toolWmOpacitySlider_->setRange(8, 80);
        toolWmOpacitySlider_->setValue(28);
        toolWmOpacitySlider_->setMaximumWidth(140);
        toolWmOpacitySpin_ = new QSpinBox(p1);
        toolWmOpacitySpin_->setRange(8, 80);
        toolWmOpacitySpin_->setValue(28);
        toolWmOpacitySpin_->setSuffix(QStringLiteral(" %"));
        connect(toolWmOpacitySlider_, &QSlider::valueChanged, toolWmOpacitySpin_, &QSpinBox::setValue);
        connect(toolWmOpacitySpin_, QOverload<int>::of(&QSpinBox::valueChanged), toolWmOpacitySlider_, &QSlider::setValue);
        lay->addWidget(toolWmOpacitySlider_);
        lay->addWidget(toolWmOpacitySpin_);
        toolDetailStack_->addWidget(p1);
    }

    // Page 2: Subtitle converter & timeline shifter
    {
        auto* p2 = new QWidget(toolDetailStack_);
        auto* lay = new QHBoxLayout(p2);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(8);
        lay->addWidget(new QLabel(QStringLiteral("时间轴平移校准 (正数延后 / 负数提前):"), p2));
        toolSubShiftSpin_ = new QSpinBox(p2);
        toolSubShiftSpin_->setRange(-600000, 600000);
        toolSubShiftSpin_->setSingleStep(250);
        toolSubShiftSpin_->setValue(0);
        toolSubShiftSpin_->setSuffix(QStringLiteral(" ms (毫秒)"));
        lay->addWidget(toolSubShiftSpin_);

        auto* btnReset = new QPushButton(QStringLiteral("归零 (0 ms)"), p2);
        btnReset->setObjectName(QStringLiteral("ChipBtn"));
        connect(btnReset, &QPushButton::clicked, this, [this]() { toolSubShiftSpin_->setValue(0); });
        lay->addWidget(btnReset);

        auto* subNote = new QLabel(QStringLiteral("支持选 TXT 一键剥离时间轴提取纯净台词文稿，自动修复 GBK/Big5 字幕乱码"), p2);
        subNote->setObjectName(QStringLiteral("Muted"));
        lay->addWidget(subNote);
        lay->addStretch(1);
        toolDetailStack_->addWidget(p2);
    }

    // Page 3: Hash Manifest Generator
    {
        auto* p3 = new QWidget(toolDetailStack_);
        auto* lay = new QHBoxLayout(p3);
        lay->setContentsMargins(0, 0, 0, 0);
        auto* note = new QLabel(
            QStringLiteral("将队列中所有任意格式文件流式计算 SHA-256 与 MD5 校验值并汇总为一份清单（超大安装包/视频亦不占内存）"),
            p3);
        note->setObjectName(QStringLiteral("Muted"));
        lay->addWidget(note);
        lay->addStretch(1);
        toolDetailStack_->addWidget(p3);
    }

    vbox->addWidget(toolDetailStack_);

    toolHintLbl_ = new QLabel(w);
    toolHintLbl_->setObjectName(QStringLiteral("Muted"));
    vbox->addWidget(toolHintLbl_);
    return w;
}

void MainWindow::applyTheme() {
    setStyleSheet(buildStylesheet(darkMode_));
    if (themeBtn_) {
        themeBtn_->setText(darkMode_ ? QStringLiteral("☀️ 切换浅色外观") : QStringLiteral("🌙 切换深色外观"));
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
    switchMode(savedMode);
}

void MainWindow::saveSettings() const {
    omni::infra::PortableSettings s = settings_;
    s.darkMode = darkMode_;
    s.lastMode = omni::core::modeToKey(currentMode_);
    s.useCustomDir = (outModeCombo_->currentIndex() == 1);
    s.outDir = outDirEdit_->text().trimmed().toUtf8().toStdString();
    omni::infra::PortableEnv::saveSettings(s);
}

std::string MainWindow::currentSubActionKey() const {
    if (currentMode_ == WorkMode::Document && docActionCombo_) {
        return docActionCombo_->currentData().toString().toStdString();
    }
    if (currentMode_ == WorkMode::Toolbox && toolActionCombo_) {
        return toolActionCombo_->currentData().toString().toStdString();
    }
    return "";
}

void MainWindow::switchMode(WorkMode mode) {
    currentMode_ = mode;
    auto it = navButtons_.find(mode);
    if (it != navButtons_.end() && it->second) {
        it->second->setChecked(true);
    }

    subStack_->setCurrentIndex(static_cast<int>(mode));

    const bool hasComp = (mode == WorkMode::Image || mode == WorkMode::Audio || mode == WorkMode::Video);
    compLabel_->setVisible(hasComp);
    compCombo_->setVisible(hasComp);

    populateModeFormats(mode);
    onCompressionChanged();
    updateDropZoneHints();
    saveSettings();
}

void MainWindow::populateModeFormats(WorkMode mode) {
    targetFmtCombo_->clear();
    targetFmtLbl_->setVisible(true);
    targetFmtCombo_->setVisible(true);
    targetFmtCombo_->setEnabled(true);

    switch (mode) {
        case WorkMode::Image:
            for (const char* f : {"webp", "jpg", "png", "avif", "heic", "ico", "icns", "bmp", "tiff", "gif", "tga", "ppm", "pdf"}) {
                targetFmtCombo_->addItem(QString::fromLatin1(f).toUpper(), QString::fromLatin1(f));
            }
            break;

        case WorkMode::Audio:
            for (const char* f : {"mp3", "m4a", "aac", "flac", "wav", "ogg", "opus", "aiff", "ac3", "wma"}) {
                targetFmtCombo_->addItem(QString::fromLatin1(f).toUpper(), QString::fromLatin1(f));
            }
            break;

        case WorkMode::Video:
            for (const char* f : {"mp4", "mkv", "mov", "webm", "gif", "apng", "avi", "flv", "ts", "wmv"}) {
                targetFmtCombo_->addItem(QString::fromLatin1(f).toUpper(), QString::fromLatin1(f));
            }
            break;

        case WorkMode::Novel:
            targetFmtCombo_->addItem(QStringLiteral("EPUB 电子书 (.epub)"), QStringLiteral("epub"));
            targetFmtCombo_->addItem(QStringLiteral("PDF 精美排版 (.pdf)"), QStringLiteral("pdf"));
            targetFmtCombo_->addItem(QStringLiteral("MOBI 电纸书 (.mobi)"), QStringLiteral("mobi"));
            targetFmtCombo_->addItem(QStringLiteral("TXT 纯文本 (.txt)"), QStringLiteral("txt"));
            targetFmtCombo_->addItem(QStringLiteral("HTML 护眼单页阅读器 (.html)"), QStringLiteral("html"));
            targetFmtCombo_->addItem(QStringLiteral("DOCX Word 文档 (.docx)"), QStringLiteral("docx"));
            targetFmtCombo_->addItem(QStringLiteral("Markdown 笔记 (.md)"), QStringLiteral("md"));
            targetFmtCombo_->addItem(QStringLiteral("FB2 电子书 (.fb2)"), QStringLiteral("fb2"));
            break;

        case WorkMode::Document: {
            const std::string act = currentSubActionKey();
            if (act == "img_to_pdf" || act == "merge_pdf") {
                targetFmtCombo_->addItem(QStringLiteral("PDF 合册文档 (.pdf)"), QStringLiteral("pdf"));
                targetFmtCombo_->setEnabled(false);
            } else if (act == "pdf_to_img") {
                targetFmtCombo_->addItem(QStringLiteral("PNG 高清无损图"), QStringLiteral("png"));
                targetFmtCombo_->addItem(QStringLiteral("JPG 轻量图片"), QStringLiteral("jpg"));
                targetFmtCombo_->addItem(QStringLiteral("WEBP 高压缩图片"), QStringLiteral("webp"));
            } else {
                targetFmtCombo_->addItem(QStringLiteral("CSV 表格 (带BOM防Excel乱码)"), QStringLiteral("csv"));
                targetFmtCombo_->addItem(QStringLiteral("JSON 结构化数组 (.json)"), QStringLiteral("json"));
                targetFmtCombo_->addItem(QStringLiteral("Markdown 表格 (.md)"), QStringLiteral("md"));
                targetFmtCombo_->addItem(QStringLiteral("HTML 可搜索交互报表 (.html)"), QStringLiteral("html"));
                targetFmtCombo_->addItem(QStringLiteral("TSV 制表符分隔 (.tsv)"), QStringLiteral("tsv"));
            }
            break;
        }

        case WorkMode::Toolbox: {
            const std::string act = currentSubActionKey();
            if (act == "exact_kb") {
                targetFmtCombo_->addItem(QStringLiteral("JPG 报名标准格式 (.jpg)"), QStringLiteral("jpg"));
                targetFmtCombo_->addItem(QStringLiteral("WEBP 高清超小格式 (.webp)"), QStringLiteral("webp"));
            } else if (act == "id_watermark") {
                targetFmtCombo_->addItem(QStringLiteral("JPG 水印图片 (.jpg)"), QStringLiteral("jpg"));
                targetFmtCombo_->addItem(QStringLiteral("PNG 无损水印图 (.png)"), QStringLiteral("png"));
                targetFmtCombo_->addItem(QStringLiteral("WEBP 水印图 (.webp)"), QStringLiteral("webp"));
            } else if (act == "subtitle") {
                targetFmtCombo_->addItem(QStringLiteral("SRT 通用字幕 (.srt)"), QStringLiteral("srt"));
                targetFmtCombo_->addItem(QStringLiteral("WebVTT 网页字幕 (.vtt)"), QStringLiteral("vtt"));
                targetFmtCombo_->addItem(QStringLiteral("ASS 特效字幕 (.ass)"), QStringLiteral("ass"));
                targetFmtCombo_->addItem(QStringLiteral("TXT 纯台词文稿提取 (.txt)"), QStringLiteral("txt"));
            } else {
                targetFmtCombo_->addItem(QStringLiteral("CSV 校验清单 (Excel直开)"), QStringLiteral("csv"));
                targetFmtCombo_->addItem(QStringLiteral("TXT 校验报告 (.txt)"), QStringLiteral("txt"));
                targetFmtCombo_->addItem(QStringLiteral("JSON 校验清单 (.json)"), QStringLiteral("json"));
            }
            break;
        }
    }
}

void MainWindow::updateDropZoneHints() {
    switch (currentMode_) {
        case WorkMode::Image:
            modeHeadlineLbl_->setText(QStringLiteral("🖼️ 拖入图片或文件夹 — 支持 15+ 种格式互转、精准画质控制与旋转水印"));
            modeHintLbl_->setText(QStringLiteral("支持输入 PNG / JPG / WEBP / HEIC / AVIF / SVG / BMP / TIFF / GIF / ICO / TGA / PSD 等 · 可切至「自定义精细参数」自由设定 1-100 画质与调色板"));
            break;
        case WorkMode::Audio:
            modeHeadlineLbl_->setText(QStringLiteral("🎵 拖入音乐、录音或视频提取音轨 — 支持 10 种格式、倍速与铃声片段裁剪"));
            modeHintLbl_->setText(QStringLiteral("支持输入 MP3 / FLAC / WAV / M4A / AAC / OGG / OPUS / WMA / AIFF 或直接拖入 MP4/MKV 视频无损抽离音频"));
            break;
        case WorkMode::Video:
            modeHeadlineLbl_->setText(QStringLiteral("🎬 拖入视频批量转码与瘦身 — 支持 H.264/H.265、CRF 精细控码、GIF/APNG 动图与片段截取"));
            modeHintLbl_->setText(QStringLiteral("支持 MP4 / MKV / MOV / AVI / WEBM / FLV / TS / WMV / M4V 等主流封装与高压缩率动图导出"));
            break;
        case WorkMode::Novel:
            modeHeadlineLbl_->setText(QStringLiteral("📚 拖入小说或电子书 — 支持自定义书封、简繁转换、网文净化与多端排版"));
            modeHintLbl_->setText(QStringLiteral("支持 TXT(自动修GBK乱码) / EPUB / MOBI / AZW3 / PDF / DOCX / HTML / MD / FB2 双向互转 · 封面直嵌书架"));
            break;
        case WorkMode::Document: {
            const std::string act = currentSubActionKey();
            if (act == "img_to_pdf") {
                modeHeadlineLbl_->setText(QStringLiteral("📑 多图按序合成为单个 PDF — 合同扫描件、发票、讲义与漫画整理神器"));
                modeHintLbl_->setText(QStringLiteral("拖入多张图片，用上方「⬆️上移 / ⬇️下移」调整页序后点击开始，即可合成一份高清无白边 PDF"));
                if (docHintLbl_) docHintLbl_->setText(QStringLiteral("💡 提示：队列中的所有图片将严格按从上到下的顺序合并进同一个 PDF 文件中。"));
            } else if (act == "merge_pdf") {
                modeHeadlineLbl_->setText(QStringLiteral("📚 多份 PDF 无损合并 — 矢量文字与清晰度 100% 原样保留"));
                modeHintLbl_->setText(QStringLiteral("拖入两份或多份 PDF 文档，支持上下调整拼接顺序，一键合并为一册完整 PDF"));
                if (docHintLbl_) docHintLbl_->setText(QStringLiteral("💡 提示：离线矢量级合并，不经过二次有损重压缩，保持原文档书签与锐利文字。"));
            } else if (act == "pdf_to_img") {
                modeHeadlineLbl_->setText(QStringLiteral("🖼️ PDF 逐页拆分为高清图片 — 每份 PDF 自动生成独立页面图片文件夹"));
                modeHintLbl_->setText(QStringLiteral("拖入 PDF 文档，可选 96 / 150 / 300 DPI 清晰度批量导出 PNG / JPG / WEBP"));
                if (docHintLbl_) docHintLbl_->setText(QStringLiteral("💡 提示：转换完成后双击列表行，可直接打开生成的页面图片文件夹。"));
            } else {
                modeHeadlineLbl_->setText(QStringLiteral("📊 数据表格互转与 Excel 乱码救星 — CSV / TSV / JSON / Markdown / HTML 网页报表"));
                modeHintLbl_->setText(QStringLiteral("自动识别 GBK / GB18030 / UTF-8 编码，导出 CSV 自动附加 UTF-8 BOM 彻底告别 Excel 中文乱码"));
                if (docHintLbl_) docHintLbl_->setText(QStringLiteral("💡 提示：支持将 JSON 接口数据直接转成 Excel 可打开的 CSV，或将 CSV 转成带实时搜索框的单页 HTML 报表。"));
            }
            break;
        }
        case WorkMode::Toolbox: {
            const std::string act = currentSubActionKey();
            if (act == "exact_kb") {
                modeHeadlineLbl_->setText(QStringLiteral("🪪 证件照 / 报名照片精确卡 KB 压缩 — 严格控制输出体积 ≤ 设定上限"));
                modeHintLbl_->setText(QStringLiteral("专为公务员、教资、四六级、签证及政务系统上传设计：二分法自动寻优，在体积达标前提下保留最高画质"));
                if (toolHintLbl_) toolHintLbl_->setText(QStringLiteral("💡 提示：引擎会自动执行「画质二分搜索 + 分辨率阶梯微调」，确保生成文件字节数严格不超标。"));
            } else if (act == "id_watermark") {
                modeHeadlineLbl_->setText(QStringLiteral("🛡️ 身份证 / 营业执照 / 合同全屏防盗水印 — 纯本地离线处理绝不泄密"));
                modeHintLbl_->setText(QStringLiteral("租房、开户、办卡提交证件复印件前一键铺满双色防篡改斜向水印，杜绝证件被盗用网贷或冒名注册"));
                if (toolHintLbl_) toolHintLbl_->setText(QStringLiteral("💡 提示：采用明暗双层抗阈值涂抹笔刷绘制，兼顾白底文档与深色证件区域清晰可读。"));
            } else if (act == "subtitle") {
                modeHeadlineLbl_->setText(QStringLiteral("💬 影视字幕互转与时间轴校准 — 支持 SRT / VTT / ASS 互转与纯台词文稿提取"));
                modeHintLbl_->setText(QStringLiteral("音画不同步一键提前/延后毫秒校准 · 自动修复老字幕 GBK/Big5 乱码 · 可选 TXT 一键提取纯净课程/电影文稿"));
                if (toolHintLbl_) toolHintLbl_->setText(QStringLiteral("💡 提示：1 秒 = 1000 毫秒（ms）；若字幕比声音慢了 1.5 秒，输入 -1500 ms 即可提前对齐。"));
            } else {
                modeHeadlineLbl_->setText(QStringLiteral("🔐 批量文件 SHA-256 / MD5 校验清单 — 下载包验真与项目交付归档"));
                modeHintLbl_->setText(QStringLiteral("可拖入任意类型文件（支持数十 GB 大文件低内存流式计算），一键汇总输出 Excel 可开的 CSV 或标准校验报告"));
                if (toolHintLbl_) toolHintLbl_->setText(QStringLiteral("💡 提示：队列中的所有文件将汇总生成一份包含文件名、字节大小、SHA-256 与 MD5 的校验清单。"));
            }
            break;
        }
    }
}

void MainWindow::onCompressionChanged() {
    const bool isCustom = (compCombo_->currentData().toInt() == static_cast<int>(CompressionLevel::Custom));
    if (imgCustomFrame_) imgCustomFrame_->setVisible(currentMode_ == WorkMode::Image && isCustom);
    if (audCustomFrame_) audCustomFrame_->setVisible(currentMode_ == WorkMode::Audio && isCustom);
    if (vidCustomFrame_) vidCustomFrame_->setVisible(currentMode_ == WorkMode::Video && isCustom);
}

void MainWindow::onImageDimModeChanged() {
    const int v = imgDimCombo_->currentData().toInt();
    imgCustomDimSpin_->setVisible(v == -1);
    imgScaleSpin_->setVisible(v == -2);
}

void MainWindow::pickImageBgColor() {
    const QColor initial(imgBgHex_);
    const QColor chosen = QColorDialog::getColor(initial, this, QStringLiteral("选择透明图片填充背景色"));
    if (chosen.isValid()) {
        imgBgHex_ = chosen.name(QColor::HexRgb).toUpper();
        imgBgColorBtn_->setText(QStringLiteral("🎨 透明底色 ") + imgBgHex_);
    }
}

void MainWindow::onVideoResModeChanged() {
    vidCustomWidthSpin_->setVisible(vidResCombo_->currentData().toInt() == -1);
}

void MainWindow::onNovelCoverModeChanged() {
    const QString mode = novCoverModeCombo_->currentData().toString();
    novPickCoverBtn_->setVisible(mode == QStringLiteral("custom"));
    novCoverPathLbl_->setVisible(mode == QStringLiteral("custom"));
    novPreviewCoverBtn_->setVisible(mode == QStringLiteral("auto"));

    if (mode == QStringLiteral("keep")) {
        novCoverThumbLbl_->setPixmap(QPixmap());
        novCoverThumbLbl_->setText(QStringLiteral("原书\n封面"));
    } else if (mode == QStringLiteral("auto")) {
        const std::string t = novTitleEdit_->text().trimmed().isEmpty()
                                  ? (fileRecords_.empty() ? "示例书名" : omni::core::pathToUtf8(fileRecords_.front().inPath.stem()))
                                  : novTitleEdit_->text().trimmed().toUtf8().toStdString();
        const std::string a = novAuthorEdit_->text().trimmed().isEmpty()
                                  ? "佚名"
                                  : novAuthorEdit_->text().trimmed().toUtf8().toStdString();
        const QImage preview = omni::engines::NovelEngine::generateProceduralCover(t, a);
        novCoverThumbLbl_->setPixmap(QPixmap::fromImage(preview).scaled(48, 68, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else if (mode == QStringLiteral("custom")) {
        if (!novCustomCoverPath_.empty()) {
            const QImage img = omni::engines::ImageEngine::loadAnyImage(novCustomCoverPath_);
            if (!img.isNull()) {
                novCoverThumbLbl_->setPixmap(QPixmap::fromImage(img).scaled(48, 68, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            }
        } else {
            novCoverThumbLbl_->setPixmap(QPixmap());
            novCoverThumbLbl_->setText(QStringLiteral("未选\n图片"));
        }
    }
}

void MainWindow::pickNovelCoverImage() {
    const QString file = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("选择电子书封面图片"),
        QString(),
        QStringLiteral("图片文件 (*.jpg *.jpeg *.png *.webp *.bmp *.heic *.avif);;所有文件 (*.*)"));
    if (file.isEmpty()) return;

    novCustomCoverPath_ = omni::core::qStringToPath(file);
    const QImage img = omni::engines::ImageEngine::loadAnyImage(novCustomCoverPath_);
    if (img.isNull()) {
        QMessageBox::warning(this, QStringLiteral("读取封面失败"), QStringLiteral("无法解码所选图片，请更换一张 JPG/PNG/WEBP 图片。"));
        novCustomCoverPath_.clear();
        return;
    }

    novCoverThumbLbl_->setPixmap(QPixmap::fromImage(img).scaled(48, 68, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    novCoverPathLbl_->setText(omni::core::pathToQString(novCustomCoverPath_.filename()));
    novCoverPathLbl_->setVisible(true);
}

void MainWindow::previewProceduralCover() {
    const std::string title = novTitleEdit_->text().trimmed().isEmpty()
                                  ? (fileRecords_.empty() ? "星河长明" : omni::core::pathToUtf8(fileRecords_.front().inPath.stem()))
                                  : novTitleEdit_->text().trimmed().toUtf8().toStdString();
    const std::string author = novAuthorEdit_->text().trimmed().isEmpty()
                                   ? "佚名"
                                   : novAuthorEdit_->text().trimmed().toUtf8().toStdString();

    const QImage cov = omni::engines::NovelEngine::generateProceduralCover(title, author);
    novCoverThumbLbl_->setPixmap(QPixmap::fromImage(cov).scaled(48, 68, Qt::KeepAspectRatio, Qt::SmoothTransformation));

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("自动生成典雅书封预览"));
    auto* lay = new QVBoxLayout(&dlg);
    auto* imgLbl = new QLabel(&dlg);
    imgLbl->setPixmap(QPixmap::fromImage(cov).scaled(340, 485, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    imgLbl->setAlignment(Qt::AlignCenter);
    auto* tip = new QLabel(QStringLiteral("提示：批量转换时将根据每本书的实际书名与作者自动生成对应专属书封"), &dlg);
    tip->setObjectName(QStringLiteral("Muted"));
    tip->setAlignment(Qt::AlignCenter);
    lay->addWidget(imgLbl);
    lay->addWidget(tip);
    dlg.exec();
}

void MainWindow::onDocumentActionChanged() {
    const std::string act = currentSubActionKey();
    docPagePresetWrap_->setVisible(act == "img_to_pdf");
    docDpiWrap_->setVisible(act == "pdf_to_img");
    docMergedNameWrap_->setVisible(act == "img_to_pdf" || act == "merge_pdf");
    populateModeFormats(WorkMode::Document);
    updateDropZoneHints();
}

void MainWindow::onToolboxActionChanged() {
    const int idx = std::clamp(toolActionCombo_->currentIndex(), 0, 3);
    toolDetailStack_->setCurrentIndex(idx);
    populateModeFormats(WorkMode::Toolbox);
    updateDropZoneHints();
}

void MainWindow::refreshEngineStatus() {
    const auto st = omni::engines::ConversionEngine::inspectHealth();
    if (st.allCoreReady()) {
        engineStatusBtn_->setText(QStringLiteral("🟢 离线引擎全就绪 (点击详情)"));
    } else {
        engineStatusBtn_->setText(QStringLiteral("🟡 离线引擎状态 (点击查看)"));
    }
}

void MainWindow::showEngineReportDialog() {
    const auto st = omni::engines::ConversionEngine::inspectHealth();
    QString report;
    report += QStringLiteral("<h3>OmniConvert Pro v3.0 离线组件自检</h3><ul>");
    report += QStringLiteral("<li>🖼️ <b>图像精修引擎</b>：✅ 就绪（AVIF: %1，HEIC: %2，SVG/ICO/ICNS/PDF: ✅）</li>")
                  .arg(st.avifSupported ? QStringLiteral("✅") : QStringLiteral("需插件"),
                       st.heicSupported ? QStringLiteral("✅") : QStringLiteral("需插件"));
    report += QStringLiteral("<li>🎬 <b>FFmpeg 音视频核心</b>：%1 %2</li>")
                  .arg(st.ffmpegAvailable ? QStringLiteral("✅ 已内置就绪") : QStringLiteral("⚠️ 未检测到 bin/ffmpeg"),
                       st.ffmpegPath.empty() ? QString() : QStringLiteral("<code>(%1)</code>").arg(QString::fromUtf8(st.ffmpegPath.c_str())));
    report += QStringLiteral("<li>📚 <b>小说封面与电子书引擎</b>：✅ EPUB / PDF / MOBI / DOCX / HTML / 简繁互转 原生就绪</li>");
    report += QStringLiteral("<li>📄 <b>PDF 与表格工坊</b>：✅ 多图合PDF / PDF无损合并 / CSV防乱码BOM引擎 就绪</li>");
    report += QStringLiteral("<li>🧰 <b>生活效率百宝箱</b>：✅ 证件照卡KB / 防盗水印 / 字幕校准 / SHA-256 全部离线就绪</li>");
    report += QStringLiteral("<li>💾 <b>便携配置文件</b>：<code>%1</code></li>")
                  .arg(omni::core::pathToQString(st.configPath));
    report += QStringLiteral("</ul>");

    QMessageBox::information(this, QStringLiteral("离线引擎状态诊断"), report);
}

void MainWindow::handleDroppedPaths(const QStringList& rawPaths) {
    if (worker_ && worker_->isRunning()) return;

    std::vector<std::filesystem::path> allFiles;
    std::error_code ec;
    for (const QString& qp : rawPaths) {
        const auto p = omni::core::qStringToPath(qp);
        if (std::filesystem::is_directory(p, ec)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(
                     p, std::filesystem::directory_options::skip_permission_denied, ec)) {
                if (entry.is_regular_file(ec)) {
                    allFiles.push_back(entry.path());
                }
            }
        } else if (std::filesystem::is_regular_file(p, ec)) {
            allFiles.push_back(p);
        }
    }
    if (allFiles.empty()) return;

    const std::string subAct = currentSubActionKey();

    // Smart auto-mode switch ONLY when queue is empty AND the first dropped file is NOT accepted by current mode/sub-action
    if (fileRecords_.empty() && !omni::core::isFileAcceptedInMode(allFiles.front(), currentMode_, subAct)) {
        if (const auto detected = omni::core::detectCategoryForFile(allFiles.front())) {
            switchMode(*detected);
        }
    }

    const std::string updatedSubAct = currentSubActionKey();
    std::vector<std::filesystem::path> accepted;
    for (const auto& f : allFiles) {
        if (omni::core::isFileAcceptedInMode(f, currentMode_, updatedSubAct)) {
            accepted.push_back(f);
        }
    }

    const int added = appendFilesToTable(accepted);
    if (added == 0) {
        summaryLbl_->setText(QStringLiteral("⚠️ 所选文件与当前功能支持的输入格式不匹配，请先切换左侧模块或子功能"));
    } else {
        summaryLbl_->setText(QStringLiteral("已添加 %1 个文件，当前队列共 %2 个待处理项").arg(added).arg(fileRecords_.size()));
    }
}

void MainWindow::browseAddFiles() {
    if (worker_ && worker_->isRunning()) return;
    const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("选择待处理文件"));
    if (!files.isEmpty()) {
        handleDroppedPaths(files);
    }
}

void MainWindow::browseAddFolder() {
    if (worker_ && worker_->isRunning()) return;
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择包含待处理文件的文件夹"));
    if (!dir.isEmpty()) {
        handleDroppedPaths({dir});
    }
}

int MainWindow::appendFilesToTable(const std::vector<std::filesystem::path>& files) {
    int added = 0;
    std::error_code ec;
    for (const auto& f : files) {
        const auto absPath = std::filesystem::weakly_canonical(f, ec);
        const auto& normPath = ec ? f : absPath;

        bool duplicate = false;
        for (const auto& rec : fileRecords_) {
            if (rec.inPath == normPath) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        const std::uintmax_t fsize = std::filesystem::file_size(normPath, ec);
        FileRecord rec;
        rec.inPath = normPath;
        rec.inSize = ec ? 0 : fsize;

        const int row = table_->rowCount();
        table_->insertRow(row);
        fileRecords_.push_back(rec);

        QString extUpper = omni::core::pathToQString(normPath.extension());
        if (extUpper.startsWith('.')) extUpper.remove(0, 1);
        extUpper = extUpper.toUpper();

        auto* nameItem = new QTableWidgetItem(omni::core::pathToQString(normPath.filename()));
        nameItem->setToolTip(omni::core::pathToQString(normPath));
        auto* extItem  = new QTableWidgetItem(extUpper.isEmpty() ? QStringLiteral("FILE") : extUpper);
        extItem->setTextAlignment(Qt::AlignCenter);
        auto* sizeItem = new QTableWidgetItem(QString::fromStdString(omni::core::formatByteSize(rec.inSize)));
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto* outItem  = new QTableWidgetItem(QStringLiteral("—"));
        outItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto* stItem   = new QTableWidgetItem(QStringLiteral("⏳ 待处理"));

        table_->setItem(row, 0, nameItem);
        table_->setItem(row, 1, extItem);
        table_->setItem(row, 2, sizeItem);
        table_->setItem(row, 3, outItem);
        table_->setItem(row, 4, stItem);
        ++added;
    }

    if (added > 0 && currentMode_ == WorkMode::Novel &&
        novCoverModeCombo_->currentData().toString() == QStringLiteral("auto")) {
        onNovelCoverModeChanged();
    }
    return added;
}

void MainWindow::swapTableRows(int rowA, int rowB) {
    if (rowA < 0 || rowB < 0 || rowA >= table_->rowCount() || rowB >= table_->rowCount() || rowA == rowB) return;
    std::swap(fileRecords_[static_cast<std::size_t>(rowA)], fileRecords_[static_cast<std::size_t>(rowB)]);
    for (int col = 0; col < table_->columnCount(); ++col) {
        QTableWidgetItem* itemA = table_->takeItem(rowA, col);
        QTableWidgetItem* itemB = table_->takeItem(rowB, col);
        table_->setItem(rowA, col, itemB);
        table_->setItem(rowB, col, itemA);
    }
}

void MainWindow::moveSelectedRowUp() {
    if (worker_ && worker_->isRunning()) return;
    const int row = table_->currentRow();
    if (row > 0) {
        swapTableRows(row, row - 1);
        table_->selectRow(row - 1);
    }
}

void MainWindow::moveSelectedRowDown() {
    if (worker_ && worker_->isRunning()) return;
    const int row = table_->currentRow();
    if (row >= 0 && row + 1 < table_->rowCount()) {
        swapTableRows(row, row + 1);
        table_->selectRow(row + 1);
    }
}

void MainWindow::removeSelectedRows() {
    if (worker_ && worker_->isRunning()) return;
    auto ranges = table_->selectedRanges();
    std::vector<int> rows;
    for (const auto& r : ranges) {
        for (int i = r.topRow(); i <= r.bottomRow(); ++i) rows.push_back(i);
    }
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());

    for (int r : rows) {
        if (r >= 0 && r < static_cast<int>(fileRecords_.size())) {
            table_->removeRow(r);
            fileRecords_.erase(fileRecords_.begin() + r);
        }
    }
    summaryLbl_->setText(QStringLiteral("当前队列共 %1 个文件").arg(fileRecords_.size()));
}

void MainWindow::clearCompletedRows() {
    if (worker_ && worker_->isRunning()) return;
    for (int r = static_cast<int>(fileRecords_.size()) - 1; r >= 0; --r) {
        if (fileRecords_[static_cast<std::size_t>(r)].done) {
            table_->removeRow(r);
            fileRecords_.erase(fileRecords_.begin() + r);
        }
    }
    summaryLbl_->setText(QStringLiteral("已清理完成项，剩余 %1 个待处理文件").arg(fileRecords_.size()));
}

void MainWindow::clearAllRows() {
    if (worker_ && worker_->isRunning()) return;
    table_->setRowCount(0);
    fileRecords_.clear();
    batchBar_->setValue(0);
    summaryLbl_->setText(QStringLiteral("队列已清空"));
}

void MainWindow::showTableContextMenu(const QPoint& pos) {
    const int row = table_->indexAt(pos).row();
    if (row < 0 || row >= static_cast<int>(fileRecords_.size())) return;

    QMenu menu(this);
    const auto& rec = fileRecords_[static_cast<std::size_t>(row)];

    QAction* actOpenIn = menu.addAction(QStringLiteral("📂 在文件管理器中定位源文件"));
    QAction* actOpenOut = nullptr;
    if (!rec.outPath.empty()) {
        actOpenOut = menu.addAction(QStringLiteral("✨ 定位转换后的输出文件"));
    }
    menu.addSeparator();
    QAction* actRemove = menu.addAction(QStringLiteral("🗑️ 从列表中移除"));

    QAction* chosen = menu.exec(table_->viewport()->mapToGlobal(pos));
    if (chosen == actOpenIn) {
        omni::infra::PortableEnv::revealInFileManager(rec.inPath);
    } else if (chosen && chosen == actOpenOut) {
        omni::infra::PortableEnv::revealInFileManager(rec.outPath);
    } else if (chosen == actRemove) {
        removeSelectedRows();
    }
}

void MainWindow::onRowDoubleClicked(int row, int /*column*/) {
    if (row < 0 || row >= static_cast<int>(fileRecords_.size())) return;
    const auto& rec = fileRecords_[static_cast<std::size_t>(row)];
    if (!rec.outPath.empty()) {
        omni::infra::PortableEnv::revealInFileManager(rec.outPath);
    } else {
        omni::infra::PortableEnv::revealInFileManager(rec.inPath);
    }
}

void MainWindow::onOutModeChanged(int index) {
    const bool custom = (index == 1);
    outDirEdit_->setEnabled(custom);
    btnPickDir_->setEnabled(custom);
    saveSettings();
}

void MainWindow::pickOutputDir() {
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择输出文件夹"), outDirEdit_->text());
    if (!dir.isEmpty()) {
        outDirEdit_->setText(dir);
        saveSettings();
    }
}

void MainWindow::openCurrentOutputDir() {
    if (outModeCombo_->currentIndex() == 1 && !outDirEdit_->text().trimmed().isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(outDirEdit_->text().trimmed()));
        return;
    }
    for (auto it = fileRecords_.rbegin(); it != fileRecords_.rend(); ++it) {
        if (!it->outPath.empty()) {
            omni::infra::PortableEnv::revealInFileManager(it->outPath);
            return;
        }
    }
    if (!fileRecords_.empty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(omni::core::pathToQString(fileRecords_.front().inPath.parent_path())));
    }
}

omni::core::ConversionOptions MainWindow::collectConversionOptions() const {
    omni::core::ConversionOptions opts;
    if (outModeCombo_->currentIndex() == 1 && !outDirEdit_->text().trimmed().isEmpty()) {
        opts.outputDir = omni::core::qStringToPath(outDirEdit_->text().trimmed());
    }
    opts.targetFormat = targetFmtCombo_->currentData().toString().toStdString();
    opts.compLevel = static_cast<CompressionLevel>(compCombo_->currentData().toInt());

    // 1. Image options
    opts.imgCustomQuality = imgQualitySpin_->value();
    opts.imgPngColors = imgPaletteCombo_->currentData().toInt();
    const int dimSel = imgDimCombo_->currentData().toInt();
    if (dimSel == -1) {
        opts.imgMaxDim = imgCustomDimSpin_->value();
    } else if (dimSel == -2) {
        opts.imgMaxDim = 0;
        opts.imgScalePercent = imgScaleSpin_->value();
    } else {
        opts.imgMaxDim = dimSel;
    }
    opts.imgRotateDeg = imgRotateCombo_->currentData().toInt();
    opts.imgFlipHorizontal = imgFlipChk_->isChecked();
    opts.imgGrayscale = imgGrayChk_->isChecked();
    opts.imgStripExif = imgStripExifChk_->isChecked();
    opts.imgBgColor = imgBgHex_.toStdString();
    opts.imgWatermarkText = imgWatermarkEdit_->text().trimmed().toUtf8().toStdString();

    // 2. Audio options
    opts.audCustomBitrateKbps = audBitrateSpin_->value();
    opts.audSampleRate = audSrCombo_->currentData().toInt();
    opts.audChannels = audChCombo_->currentData().toInt();
    opts.audSpeed = audSpeedCombo_->currentData().toDouble();
    opts.audLoudnorm = audLoudnormChk_->isChecked();

    // 3. Video options
    opts.vidCustomCrf = vidCrfSpin_->value();
    const int vRes = vidResCombo_->currentData().toInt();
    opts.vidMaxWidth = (vRes == -1) ? vidCustomWidthSpin_->value() : vRes;
    opts.vidCodec = vidCodecCombo_->currentData().toString().toStdString();
    opts.vidFps = vidFpsCombo_->currentData().toInt();
    opts.vidMute = vidMuteChk_->isChecked();

    if (currentMode_ == WorkMode::Audio) {
        opts.mediaTrimStart = audTrimStartEdit_->text().trimmed().toStdString();
        opts.mediaTrimEnd = audTrimEndEdit_->text().trimmed().toStdString();
    } else if (currentMode_ == WorkMode::Video) {
        opts.mediaTrimStart = vidTrimStartEdit_->text().trimmed().toStdString();
        opts.mediaTrimEnd = vidTrimEndEdit_->text().trimmed().toStdString();
    }

    // 4. Novel options
    const int indentChars = novIndentCombo_->currentData().toInt();
    opts.novAutoIndent = (indentChars > 0);
    opts.novIndentSpaces = indentChars;
    opts.novFontSizePt = novFontSizeSpin_->value();
    opts.novLineHeight = novLineHeightSpin_->value();
    opts.novPdfPreset = novPdfCombo_->currentData().toString().toStdString();
    opts.novTxtEncoding = novEncCombo_->currentData().toString().toStdString();
    opts.novCustomTitle = novTitleEdit_->text().trimmed().toUtf8().toStdString();
    opts.novCustomAuthor = novAuthorEdit_->text().trimmed().toUtf8().toStdString();
    const QString covMode = novCoverModeCombo_->currentData().toString();
    if (covMode == QStringLiteral("custom") && !novCustomCoverPath_.empty()) {
        opts.novCoverPath = novCustomCoverPath_;
    } else if (covMode == QStringLiteral("auto")) {
        opts.novAutoGenerateCover = true;
    }
    opts.novCleanAds = novCleanAdsChk_->isChecked();
    opts.novZhConvert = novZhCombo_->currentData().toString().toStdString();
    opts.novCustomChapterRegex = novRegexEdit_->text().trimmed().toUtf8().toStdString();

    // 5. Document options
    opts.docAction = docActionCombo_->currentData().toString().toStdString();
    opts.docPagePreset = docPagePresetCombo_->currentData().toString().toStdString();
    opts.docPdfDpi = docDpiCombo_->currentData().toInt();
    opts.docMergedName = docMergedNameEdit_->text().trimmed().toUtf8().toStdString();

    // 6. Toolbox options
    opts.toolAction = toolActionCombo_->currentData().toString().toStdString();
    opts.toolTargetKb = toolTargetKbSpin_->value();
    opts.toolWatermarkText = toolWatermarkEdit_->text().trimmed().toUtf8().toStdString();
    opts.toolWatermarkOpacity = toolWmOpacitySpin_->value();
    opts.toolSubtitleShiftMs = toolSubShiftSpin_->value();

    return opts;
}

void MainWindow::onStartOrStopClicked() {
    if (worker_ && worker_->isRunning()) {
        worker_->requestCancel();
        startBtn_->setEnabled(false);
        startBtn_->setText(QStringLiteral("正在安全中止..."));
        return;
    }

    if (fileRecords_.empty()) {
        QMessageBox::information(this, QStringLiteral("列表为空"), QStringLiteral("请先拖入文件或点击上方「➕ 添加文件」。"));
        return;
    }

    const auto opts = collectConversionOptions();
    if (outModeCombo_->currentIndex() == 1 && opts.outputDir.empty()) {
        QMessageBox::warning(this, QStringLiteral("未指定输出目录"), QStringLiteral("请先浏览选择一个输出文件夹，或切回「保存在源文件同级目录」。"));
        return;
    }

    const bool isAgg = omni::core::isAggregateBatchAction(currentMode_, opts);

    std::vector<omni::core::TaskItem> pending;
    for (int i = 0; i < static_cast<int>(fileRecords_.size()); ++i) {
        // For aggregate actions (Merge PDF / Images->PDF / Hash Manifest), include all rows in queue order
        if (isAgg || !fileRecords_[static_cast<std::size_t>(i)].done) {
            fileRecords_[static_cast<std::size_t>(i)].done = false;
            omni::core::TaskItem item;
            item.rowId = i;
            item.inputPath = fileRecords_[static_cast<std::size_t>(i)].inPath;
            item.mode = currentMode_;
            pending.push_back(item);
            if (auto* st = table_->item(i, 4)) st->setText(QStringLiteral("⏳ 排队中..."));
        }
    }

    if (pending.empty()) {
        const auto ans = QMessageBox::question(
            this,
            QStringLiteral("全部已完成"),
            QStringLiteral("列表中的文件均已处理完毕，是否按当前最新参数重新处理一遍？"),
            QMessageBox::Yes | QMessageBox::No);
        if (ans != QMessageBox::Yes) return;
        for (int i = 0; i < static_cast<int>(fileRecords_.size()); ++i) {
            fileRecords_[static_cast<std::size_t>(i)].done = false;
            omni::core::TaskItem item;
            item.rowId = i;
            item.inputPath = fileRecords_[static_cast<std::size_t>(i)].inPath;
            item.mode = currentMode_;
            pending.push_back(item);
        }
    }

    rowProgressMap_.clear();
    completedCount_ = 0;
    totalTasks_ = static_cast<int>(pending.size());
    batchBar_->setValue(0);

    startBtn_->setObjectName(QStringLiteral("StopBtn"));
    startBtn_->setText(QStringLiteral("⏹ 停止处理"));
    style()->unpolish(startBtn_);
    style()->polish(startBtn_);

    summaryLbl_->setText(QStringLiteral("正在高速离线处理 %1 个任务...").arg(totalTasks_));

    worker_ = new omni::core::QtSchedulerBridge(std::move(pending), opts, this);
    connect(worker_, &omni::core::QtSchedulerBridge::taskProgress, this, &MainWindow::onTaskProgress);
    connect(worker_, &omni::core::QtSchedulerBridge::taskFinished, this, &MainWindow::onTaskFinished);
    connect(worker_, &omni::core::QtSchedulerBridge::batchFinished, this, &MainWindow::onBatchFinished);
    connect(worker_, &QThread::finished, worker_, &QObject::deleteLater);
    worker_->start();
}

void MainWindow::onTaskProgress(int rowId, int pct, const QString& statusText) {
    if (rowId >= 0 && rowId < table_->rowCount()) {
        if (auto* item = table_->item(rowId, 4)) {
            item->setText(statusText);
        }
    }
    rowProgressMap_[rowId] = std::clamp(pct, 0, 100);
    if (totalTasks_ > 0) {
        long long sum = 0;
        for (const auto& kv : rowProgressMap_) sum += kv.second;
        batchBar_->setValue(static_cast<int>(sum / totalTasks_));
    }
}

void MainWindow::onTaskFinished(int rowId, bool ok, const QString& outPath, const QString& msg, quint64 outBytes) {
    ++completedCount_;
    rowProgressMap_[rowId] = 100;
    if (totalTasks_ > 0) {
        long long sum = 0;
        for (const auto& kv : rowProgressMap_) sum += kv.second;
        batchBar_->setValue(static_cast<int>(sum / totalTasks_));
    }

    if (rowId < 0 || rowId >= static_cast<int>(fileRecords_.size())) return;
    auto& rec = fileRecords_[static_cast<std::size_t>(rowId)];
    if (ok) {
        rec.done = true;
        rec.outPath = omni::core::qStringToPath(outPath);

        if (auto* outCell = table_->item(rowId, 3)) {
            if (outBytes > 0) {
                QString sizeTxt = QString::fromStdString(omni::core::formatByteSize(outBytes));
                if (rec.inSize > 0 && outBytes < rec.inSize) {
                    const int savedPct = static_cast<int>((rec.inSize - outBytes) * 100 / rec.inSize);
                    sizeTxt += QStringLiteral(" (-%1%)").arg(savedPct);
                }
                outCell->setText(sizeTxt);
            } else {
                outCell->setText(QStringLiteral("已并入总表"));
            }
        }
    }

    if (auto* stCell = table_->item(rowId, 4)) {
        stCell->setText(msg);
        stCell->setToolTip(msg);
    }
}

void MainWindow::onBatchFinished(int successCnt, int failCnt, quint64 savedBytes) {
    worker_ = nullptr;
    startBtn_->setEnabled(true);
    startBtn_->setObjectName(QStringLiteral("PrimaryBtn"));
    startBtn_->setText(QStringLiteral("🚀 开始批量处理"));
    style()->unpolish(startBtn_);
    style()->polish(startBtn_);

    batchBar_->setValue(100);

    QString summary = QStringLiteral("✅ 处理完成：成功 %1 项").arg(successCnt);
    if (failCnt > 0) {
        summary += QStringLiteral("，失败 %1 项").arg(failCnt);
    }
    if (savedBytes > 0) {
        summary += QStringLiteral(" · 累计节省空间 %1").arg(QString::fromStdString(omni::core::formatByteSize(savedBytes)));
    }
    summary += QStringLiteral("（双击任意行即可定位输出文件）");
    summaryLbl_->setText(summary);
}

} // namespace omni::ui
