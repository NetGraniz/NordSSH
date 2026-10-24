#include "ConnectionPane.h"
#include "Theme.h"
#include <QApplication>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPushButton>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {
class Window final : public QMainWindow {
public:
    explicit Window(nordssh::ssh::ConnectionRuntime& runtime) : runtime_(runtime) {
        setWindowTitle("NordSSH"); resize(1100, 740);
        auto* body = new QWidget(this); auto* layout = new QVBoxLayout(body);
        auto* bar = new QHBoxLayout;
        auto* host = new QLineEdit(body); host->setPlaceholderText(tr("Host")); host->setMaxLength(253);
        auto* port = new QSpinBox(body); port->setRange(1, 65535); port->setValue(22); port->setPrefix(tr("Port "));
        auto* user = new QLineEdit(body); user->setPlaceholderText(tr("Username")); user->setMaxLength(128);
        auto* connectButton = new QPushButton(tr("Connect"), body);
        bar->addWidget(host, 2); bar->addWidget(port); bar->addWidget(user, 1); bar->addWidget(connectButton);
        layout->addLayout(bar);
        tabs_ = new QTabWidget(body); tabs_->setTabsClosable(true); tabs_->setDocumentMode(true);
        layout->addWidget(tabs_, 1); setCentralWidget(body);
        QObject::connect(connectButton, &QPushButton::clicked, this, [=, this] {
            auto* pane = new nordssh::ConnectionPane(runtime_, tabs_);
            if (!pane->start({host->text().toStdString(), static_cast<std::uint16_t>(port->value()), user->text().toUtf8().toStdString()})) {
                delete pane; statusBar()->showMessage(tr("Check the host, username and connection limit.")); return;
            }
            const int index = tabs_->addTab(pane, host->text()); tabs_->setCurrentIndex(index);
            statusBar()->clearMessage();
        });
        QObject::connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int index) {
            auto* pane = tabs_->widget(index); tabs_->removeTab(index); delete pane; // No network join.
        });
        QObject::connect(&shutdown_, &QTimer::timeout, this, [this] { if (runtime_.idle()) { shutdown_.stop(); close(); } });
        shutdown_.setInterval(10);
    }
protected:
    void closeEvent(QCloseEvent* event) override {
        if (runtime_.idle()) { event->accept(); return; }
        event->ignore(); runtime_.shutdown(); centralWidget()->setEnabled(false);
        statusBar()->showMessage(tr("Closing connections…")); shutdown_.start();
    }
private:
    nordssh::ssh::ConnectionRuntime& runtime_;
    QTabWidget* tabs_;
    QTimer shutdown_;
};
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName("NordSSH"); app.setOrganizationName("NordSSH.Native");
    app.setStyleSheet(nordssh::NativeStyle);
    nordssh::ssh::ConnectionRuntime runtime;
    Window window(runtime); window.show();
    return app.exec();
}
