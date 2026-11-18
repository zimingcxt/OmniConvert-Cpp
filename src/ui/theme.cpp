#include "omni/ui/theme.hpp"

namespace omni::ui {

QString buildStylesheet(bool dark) {
    const QString bgWin       = dark ? QStringLiteral("#0f1117") : QStringLiteral("#f3f5f9");
    const QString bgSide      = dark ? QStringLiteral("#161922") : QStringLiteral("#ffffff");
    const QString bgCard      = dark ? QStringLiteral("#1c202b") : QStringLiteral("#ffffff");
    const QString bgSubCard   = dark ? QStringLiteral("#151923") : QStringLiteral("#f8fafc");
    const QString bgInput     = dark ? QStringLiteral("#242938") : QStringLiteral("#f8fafc");
    const QString fgMain      = dark ? QStringLiteral("#e6edf3") : QStringLiteral("#1e293b");
    const QString fgSub       = dark ? QStringLiteral("#8b949e") : QStringLiteral("#64748b");
    const QString border      = dark ? QStringLiteral("#2d3446") : QStringLiteral("#e2e8f0");
    const QString accent      = dark ? QStringLiteral("#3b82f6") : QStringLiteral("#2563eb");
    const QString accentHover = dark ? QStringLiteral("#2563eb") : QStringLiteral("#1d4ed8");
    const QString dropBg      = dark ? QStringLiteral("#182032") : QStringLiteral("#eff6ff");
    const QString dropBorder  = QStringLiteral("#3b82f6");
    const QString rowAlt      = dark ? QStringLiteral("#181c26") : QStringLiteral("#f8fafc");

    return QStringLiteral(R"(
    QMainWindow, QDialog {
        background-color: %1;
        color: %5;
        font-family: "PingFang SC", "Microsoft YaHei", "Segoe UI", sans-serif;
        font-size: 13px;
    }
    QFrame#Sidebar {
        background-color: %2;
        border-right: 1px solid %7;
    }
    QFrame#Card {
        background-color: %3;
        border: 1px solid %7;
        border-radius: 10px;
    }
    QFrame#SubCard {
        background-color: %13;
        border: 1px dashed %7;
        border-radius: 8px;
    }
    QFrame#DropZone {
        background-color: %10;
        border: 2px dashed %11;
        border-radius: 12px;
    }
    QLabel {
        color: %5;
    }
    QLabel#AppTitle {
        font-size: 18px;
        font-weight: 700;
    }
    QLabel#Muted {
        color: %6;
        font-size: 12px;
    }
    QLabel#SectionTitle {
        font-size: 14px;
        font-weight: 600;
    }
    QLabel#CoverThumb {
        background-color: %4;
        border: 1px solid %7;
        border-radius: 5px;
        color: %6;
        font-size: 11px;
    }
    QPushButton {
        background-color: %4;
        color: %5;
        border: 1px solid %7;
        border-radius: 7px;
        padding: 5px 11px;
        font-weight: 500;
    }
    QPushButton:hover {
        border-color: %8;
    }
    QPushButton:disabled {
        opacity: 0.45;
        color: %6;
    }
    QPushButton#NavBtn {
        text-align: left;
        padding: 10px 13px;
        border-radius: 8px;
        border: none;
        background-color: transparent;
        font-size: 13.5px;
    }
    QPushButton#NavBtn:hover {
        background-color: %4;
    }
    QPushButton#NavBtn:checked {
        background-color: %8;
        color: #ffffff;
        font-weight: 600;
    }
    QPushButton#PrimaryBtn {
        background-color: %8;
        color: #ffffff;
        border: none;
        border-radius: 8px;
        padding: 9px 22px;
        font-size: 14px;
        font-weight: 600;
    }
    QPushButton#PrimaryBtn:hover {
        background-color: %9;
    }
    QPushButton#StopBtn {
        background-color: #ef4444;
        color: #ffffff;
        border: none;
        border-radius: 8px;
        padding: 9px 22px;
        font-size: 14px;
        font-weight: 600;
    }
    QPushButton#ChipBtn {
        padding: 3px 9px;
        font-size: 12px;
        border-radius: 6px;
    }
    QComboBox, QLineEdit, QSpinBox, QDoubleSpinBox {
        background-color: %4;
        color: %5;
        border: 1px solid %7;
        border-radius: 6px;
        padding: 4px 8px;
        min-height: 22px;
    }
    QComboBox:focus, QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus {
        border-color: %8;
    }
    QComboBox QAbstractItemView {
        background-color: %3;
        color: %5;
        selection-background-color: %8;
        selection-color: #ffffff;
        border: 1px solid %7;
    }
    QSlider::groove:horizontal {
        height: 6px;
        background: %7;
        border-radius: 3px;
    }
    QSlider::sub-page:horizontal {
        background: %8;
        border-radius: 3px;
    }
    QSlider::handle:horizontal {
        background: %8;
        width: 14px;
        height: 14px;
        margin: -4px 0;
        border-radius: 7px;
    }
    QTableWidget {
        background-color: %3;
        alternate-background-color: %12;
        color: %5;
        border: 1px solid %7;
        border-radius: 8px;
        gridline-color: %7;
        selection-background-color: %8;
        selection-color: #ffffff;
    }
    QHeaderView::section {
        background-color: %4;
        color: %6;
        border: none;
        border-bottom: 1px solid %7;
        padding: 7px 10px;
        font-weight: 600;
    }
    QProgressBar {
        background-color: %4;
        border: 1px solid %7;
        border-radius: 6px;
        text-align: center;
        color: %5;
        min-height: 18px;
    }
    QProgressBar::chunk {
        background-color: %8;
        border-radius: 5px;
    }
    QCheckBox {
        color: %5;
        spacing: 6px;
    }
    )")
        .arg(bgWin, bgSide, bgCard, bgInput, fgMain, fgSub, border, accent, accentHover, dropBg, dropBorder, rowAlt, bgSubCard);
}

} // namespace omni::ui
