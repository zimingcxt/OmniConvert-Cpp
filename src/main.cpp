#include "omni/ui/main_window.hpp"

#include <QApplication>
#include <QIcon>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("OmniConvert"));
    app.setApplicationDisplayName(QStringLiteral("OmniConvert 万能格式工坊 Pro"));
    app.setApplicationVersion(QStringLiteral("3.0.0"));
    app.setWindowIcon(QIcon(QStringLiteral(":/app_icon.png")));

    omni::ui::MainWindow win;
    win.show();
    return app.exec();
}
