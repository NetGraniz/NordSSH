#pragma once
#include "terminal/TerminalWidget.h"
#include "ssh/ConnectionManager.h"
#include <QTimer>
class QLabel;
class QLineEdit;
class QPushButton;
class QFrame;

namespace nordssh {
// Native session UI slice. Trust is per connection; persistent vault/trust
// storage is not silently simulated by this engineering application.
class ConnectionPane final : public QWidget {
public:
    explicit ConnectionPane(ssh::ConnectionRuntime& runtime, QWidget* parent = nullptr);
    bool start(const ssh::Endpoint& endpoint, const std::optional<ssh::TrustedHostPin>& saved = {});
    [[nodiscard]] ssh::ConnectionManager& connection() { return *connection_; }
    [[nodiscard]] TerminalWidget& terminal() { return *terminal_; }
private:
    void refresh();
    void submitPassword();
    void inputFailed();
    ssh::ConnectionManager* connection_;
    TerminalWidget* terminal_;
    QLabel *status_, *host_, *authenticationError_;
    QFrame *trust_, *authentication_;
    QLineEdit* password_;
    QPushButton *accept_, *reject_, *authenticate_, *disconnect_;
    QTimer timer_;
    std::optional<ssh::HostPrompt> prompt_;
    bool credentialsSent_ = false, wasActive_ = false, inputFailure_ = false;
};
} // namespace nordssh
