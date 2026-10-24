#include "ConnectionPane.h"
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <openssl/crypto.h>
#include <cstring>

namespace nordssh {
namespace {
QLabel* label(QWidget* parent, const char* name) {
    auto* result = new QLabel(parent);
    result->setObjectName(name); result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true); result->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return result;
}
QString statusText(const ssh::ConnectionStatus& status) {
    using ssh::AttemptPhase; using ssh::AttemptReason;
    switch (status.phase) {
    case AttemptPhase::Connecting: return QObject::tr("Connecting…");
    case AttemptPhase::HostVerification: return QObject::tr("Verify the server key before signing in.");
    case AttemptPhase::Authentication: return QObject::tr("Authentication");
    case AttemptPhase::Active: return QObject::tr("Connected");
    case AttemptPhase::Closing: return QObject::tr("Closing…");
    default: break;
    }
    switch (status.reason) {
    case AttemptReason::ChangedKey: return QObject::tr("Server key changed. Connection blocked. Verify the change independently.");
    case AttemptReason::Rejected: return QObject::tr("Server key rejected.");
    case AttemptReason::Timeout: return QObject::tr("Connection timed out.");
    case AttemptReason::Authentication: return QObject::tr("Authentication failed. Reconnect to try again.");
    case AttemptReason::Network: return QObject::tr("SSH connection failed.");
    case AttemptReason::Protocol: return QObject::tr("Server identity did not match this connection.");
    case AttemptReason::Cancelled: return QObject::tr("Connection closed.");
    case AttemptReason::RemoteClosed: return QObject::tr("Server closed the connection.");
    default: return QObject::tr("Disconnected");
    }
}
}
ConnectionPane::ConnectionPane(ssh::ConnectionRuntime& runtime, QWidget* parent)
    : QWidget(parent), connection_(new ssh::ConnectionManager(runtime, this)), terminal_(new TerminalWidget(this)) {
    auto* layout = new QVBoxLayout(this);
    auto* heading = new QHBoxLayout;
    status_ = label(this, "connectionStatus");
    disconnect_ = new QPushButton(tr("Disconnect"), this);
    disconnect_->setObjectName("disconnectSession");
    heading->addWidget(status_, 1); heading->addWidget(disconnect_);
    layout->addLayout(heading);
    trust_ = new QFrame(this); trust_->setObjectName("hostVerification");
    trust_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* trustLayout = new QVBoxLayout(trust_);
    host_ = label(trust_, "hostFingerprint"); trustLayout->addWidget(host_);
    auto* decisions = new QHBoxLayout;
    accept_ = new QPushButton(tr("Trust this connection"), trust_); accept_->setObjectName("acceptHostKey");
    reject_ = new QPushButton(tr("Reject"), trust_); reject_->setObjectName("rejectHostKey");
    decisions->addWidget(accept_); decisions->addWidget(reject_); decisions->addStretch(); trustLayout->addLayout(decisions);
    layout->addWidget(trust_);
    authentication_ = new QFrame(this); authentication_->setObjectName("passwordAuthentication");
    authentication_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* authLayout = new QHBoxLayout(authentication_);
    auto* passwordLabel = label(authentication_, "passwordLabel"); passwordLabel->setText(tr("SSH password"));
    password_ = new QLineEdit(authentication_); password_->setObjectName("sshPassword");
    password_->setEchoMode(QLineEdit::Password); password_->setMaxLength(4096); passwordLabel->setBuddy(password_);
    authenticate_ = new QPushButton(tr("Sign in"), authentication_); authenticate_->setObjectName("submitSshPassword");
    authLayout->addWidget(passwordLabel); authLayout->addWidget(password_, 1); authLayout->addWidget(authenticate_);
    layout->addWidget(authentication_);
    authenticationError_ = label(this, "authenticationError"); authenticationError_->hide();
    layout->addWidget(authenticationError_); layout->addWidget(terminal_, 1);
    trust_->hide(); authentication_->hide(); terminal_->setEnabled(false);
    QObject::connect(accept_, &QPushButton::clicked, this, [this] {
        if (prompt_) connection_->approve(prompt_->attempt, prompt_->challenge);
        refresh();
    });
    QObject::connect(reject_, &QPushButton::clicked, this, [this] {
        if (prompt_) connection_->reject(prompt_->attempt);
        refresh();
    });
    QObject::connect(disconnect_, &QPushButton::clicked, this, [this] { connection_->cancel(); refresh(); });
    QObject::connect(authenticate_, &QPushButton::clicked, this, [this] { submitPassword(); });
    QObject::connect(password_, &QLineEdit::returnPressed, this, [this] { submitPassword(); });
    terminal_->setSend([this](const QByteArray& bytes) {
        // Final remote output can still contain VT queries after EOF. There is
        // no live peer to answer then; do not label that as failed user input.
        if (connection_->status().phase != ssh::AttemptPhase::Active) return;
        if (!connection_->send(std::as_bytes(std::span(bytes.constData(), static_cast<std::size_t>(bytes.size())))) &&
            connection_->status().phase == ssh::AttemptPhase::Active) inputFailed();
    });
    terminal_->setResize([this](int columns, int rows) { (void)connection_->resize(columns, rows); });
    terminal_->setFailure([this] { inputFailed(); });
    QObject::connect(&timer_, &QTimer::timeout, this, [this] { refresh(); });
    timer_.setInterval(16);
    refresh();
}
bool ConnectionPane::start(const ssh::Endpoint& endpoint, const std::optional<ssh::TrustedHostPin>& saved) {
    const auto frame = terminal_->snapshot();
    if (!connection_->start(endpoint, saved, {}, frame.columns, frame.rows)) return false;
    credentialsSent_ = wasActive_ = inputFailure_ = false;
    password_->clear(); prompt_.reset();
    authenticationError_->clear(); authenticationError_->hide();
    timer_.start(); refresh(); return true;
}
void ConnectionPane::submitPassword() {
    const auto current = connection_->status();
    if (!current.request || current.phase != ssh::AttemptPhase::Authentication || credentialsSent_) return;
    auto bytes = password_->text().toUtf8();
    if (bytes.isEmpty() || bytes.size() > 4096) {
        OPENSSL_cleanse(bytes.data(), static_cast<std::size_t>(bytes.size()));
        authenticationError_->setText(tr("Enter an SSH password of at most 4096 UTF-8 bytes."));
        authenticationError_->show(); return;
    }
    vault::SecretBytes secret(static_cast<std::size_t>(bytes.size()));
    std::memcpy(secret.bytes().data(), bytes.constData(), secret.bytes().size());
    OPENSSL_cleanse(bytes.data(), static_cast<std::size_t>(bytes.size()));
    password_->clear();
    credentialsSent_ = connection_->authenticate(*current.request,
        {ssh::Credentials::Kind::Password, std::move(secret), vault::SecretBytes(0), {}});
    authenticationError_->setText(credentialsSent_ ? QString() : tr("Password was not submitted. Check the connection status."));
    authenticationError_->setVisible(!credentialsSent_);
    refresh();
}
void ConnectionPane::inputFailed() {
    inputFailure_ = true; connection_->cancel();
    status_->setText(tr("Terminal input could not be queued. Connection closed; reconnect before sending more commands."));
}
void ConnectionPane::refresh() {
    const auto current = connection_->status();
    if (!inputFailure_) status_->setText(statusText(current));
    prompt_ = connection_->hostPrompt();
    trust_->setVisible(prompt_.has_value());
    if (prompt_) {
        const auto& identity = prompt_->identity;
        const auto fingerprint = QByteArray(reinterpret_cast<const char*>(identity.sha256.data()), 32)
            .toBase64(QByteArray::OmitTrailingEquals);
        host_->setText(tr("Host: %1:%2\nUser: %3\nAlgorithm: %4\nSHA256:%5")
            .arg(QString::fromStdString(identity.endpoint.host)).arg(identity.endpoint.port)
            .arg(QString::fromUtf8(identity.endpoint.user)).arg(QString::fromStdString(identity.algorithm))
            .arg(QString::fromLatin1(fingerprint)));
    }
    const bool authentication = current.phase == ssh::AttemptPhase::Authentication && !credentialsSent_;
    const bool wasAuthentication = authentication_->isVisible();
    authentication_->setVisible(authentication);
    if (authentication && !wasAuthentication) password_->setFocus();
    if (!authentication) password_->clear();
    const bool active = current.phase == ssh::AttemptPhase::Active;
    terminal_->setEnabled(active && !inputFailure_);
    if (active && !wasActive_) terminal_->setFocus();
    wasActive_ = active;
    disconnect_->setEnabled(current.request.has_value() && !current.settled && current.phase != ssh::AttemptPhase::Closing);
    // Bound work per frame; leave the rest in the backpressured mailbox.
    bool drained = false;
    for (unsigned batch = 0; batch < 4; ++batch) {
        auto chunk = connection_->takeOutput();
        if (!chunk) { drained = true; break; }
        const auto bytes = chunk->bytes();
        terminal_->feed(QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size())));
    }
    if (current.settled && drained) timer_.stop();
}
} // namespace nordssh
