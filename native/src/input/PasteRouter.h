#pragma once
#include <QObject>
#include <functional>
#include <memory>

class QKeyEvent;

namespace nordssh {
// Install on the actual terminal key/mouse receiver, not a whole window or an
// outer container with a child viewport/focus proxy. Rebind when that changes.
// This is the only paste shortcut route: do not add a QAction/QShortcut for paste.
// The action reads QClipboard on demand, validates and confirms multiline input,
// then sends it through the terminal encoder. This router never reads clipboard.
class PasteRouter final : public QObject {
public:
    PasteRouter(QObject* terminalWidget, std::function<void()> requestPaste);
protected:
    bool eventFilter(QObject* target, QEvent* event) override;
private:
    static bool isPaste(const QKeyEvent& key);
    void requestOnce();
    std::function<void()> requestPaste_;
    std::shared_ptr<bool> busy_ = std::make_shared<bool>(false);
};
} // namespace nordssh
