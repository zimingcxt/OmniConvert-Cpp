#include "omni/ui/theme.hpp"

namespace omni::ui {

QString buildStylesheet(bool dark) {
    const QString bgWin       = dark ? QStringLiteral("#0f1117") : QStringLiteral("#f3f5f9");
    const QString bgSide      = dark ? QStringLiteral("#161922") : QStringLiteral("#ffffff");
    const QString bgCard      = dark ? QStringLiteral("#1c202b") : QStringLiteral("#ffffff");
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
    QMainWindow, QWidget {
        background-color: %1;
        color: %2;
        font-family: "PingFang SC", "Hiragino Sans GB", "Microsoft YaHei UI", "Microsoft YaHei", system-ui, sans-serif;
        font-size: 13px;
    }
    QFrame#Sidebar {
        background-color: %3;
        border-right: 1px solid %4;
    }
    QFrame#Card {
        background-color: %5;
        border: 1px solid %4;
        border-radius: 12px;
    }
    QFrame#DropZone {
        background-color: %6;
        border: 2px dashed %7;
        border-radius: 12px;
    }
    QLabel {
        background: transparent;
    }
    QLabel#AppTitle {
        font-size: 18px;
        font-weight: 700;
    }
    QLabel#Muted {
        color: %8;
        font-size: 12px;
    }
    QLabel#SectionTitle {
        font-size: 14px;
        font-weight: 600;
    }
    QPushButton {
        background-color: %9;
        color: %2;
        border: 1px solid %4;
        border-radius: 8px;
        padding: 7px 14px;
        font-weight: 500;
    }
    QPushButton:hover {
        border-color: %10;
    }
    QPushButton:disabled {
        color: %8;
    }
    QPushButton#NavBtn {
        text-align: left;
        padding: 11px 14px;
        border-radius: 9px;
        border: 1px solid transparent;
        background: transparent;
        font-size: 13px;
    }
    QPushButton#NavBtn:hover {
        background-color: %9;
    }
    QPushButton#NavBtn:checked {
        background-color: %10;
        color: #ffffff;
        font-weight: 600;
    }
    QPushButton#PrimaryBtn {
        background-color: %10;
        color: #ffffff;
        border: none;
        border-radius: 9px;
        padding: 10px 24px;
        font-size: 14px;
        font-weight: 600;
    }
    QPushButton#PrimaryBtn:hover {
        background-color: %11;
    }
    QPushButton#StopBtn {
        background-color: #ef4444;
        color: #ffffff;
        border: none;
        border-radius: 9px;
        padding: 10px 24px;
        font-size: 14px;
        font-weight: 600;
    }
    QComboBox, QLineEdit {
        background-color: %9;
        color: %2;
        border: 1px solid %4;
        border-radius: 7px;
        padding: 6px 10px;
        min-height: 20px;
    }
    QComboBox:focus, QLineEdit:focus {
        border-color: %10;
    }
    QTableWidget {
        background-color: %5;
        alternate-background-color: %12;
        border: 1px solid %4;
        border-radius: 10px;
        gridline-color: transparent;
        selection-background-color: %10;
        selection-color: #ffffff;
    }
    QHeaderView::section {
        background-color: %9;
        color: %8;
        border: none;
        border-bottom: 1px solid %4;
        padding: 8px 10px;
        font-weight: 600;
    }
    QProgressBar {
        background-color: %9;
        border: 1px solid %4;
        border-radius: 6px;
        text-align: center;
        height: 16px;
        font-size: 11px;
    }
    QProgressBar::chunk {
        background-color: %10;
        border-radius: 5px;
    }
    QCheckBox {
        background: transparent;
        spacing: 6px;
    }
    )")
        .arg(bgWin, fgMain, bgSide, border, bgCard, dropBg, dropBorder, fgSub, bgInput, accent, accentHover, rowAlt);
}

} // namespace omni::ui
