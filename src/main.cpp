#include "omni/ui/main_window.hpp"

#include <QApplication>
#include <QIcon>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setStyle(QStringLiteral("Fusion"));
    app.setApplicationName(QStringLiteral("OmniConvert"));
    app.setApplicationDisplayName(QStringLiteral("OmniConvert 万能格式工坊"));
    app.setApplicationVersion(QStringLiteral("2.0.0"));
    app.setWindowIcon(QIcon(QStringLiteral(":/app_icon.png")));

    omni::ui::MainWindow win;
    win.show();
    return app.exec();
}
