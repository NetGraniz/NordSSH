// Test-only framed stdin driver. Never part of the application runtime.
#include "terminal/TerminalWidget.h"
#include <QApplication>
#include <QImage>
#include <QKeyEvent>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
using namespace nordssh;
static std::uint32_t number(const char* p) {
    return (std::uint32_t(static_cast<unsigned char>(p[0])) << 24) |
        (std::uint32_t(static_cast<unsigned char>(p[1])) << 16) |
        (std::uint32_t(static_cast<unsigned char>(p[2])) << 8) | std::uint32_t(static_cast<unsigned char>(p[3]));
}
static void reply(char kind, const QByteArray& bytes) {
    const auto size = static_cast<std::uint32_t>(bytes.size());
    const std::array<char, 5> header{kind, char(size >> 24), char(size >> 16), char(size >> 8), char(size)};
    std::cout.write(header.data(), header.size()); std::cout.write(bytes.constData(), bytes.size()); std::cout.flush();
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        TerminalWidget widget; widget.resize(widget.sizeHint()); widget.show(); app.processEvents();
        QByteArray sent; bool failed = false;
        widget.setSend([&](const QByteArray& bytes) { sent += bytes; });
        widget.setFailure([&] { failed = true; });
        widget.setPasteSource([] { return QString{}; }, [] { return false; });
        std::array<char, 5> header{};
        while (std::cin.read(header.data(), header.size())) {
            const auto size = number(header.data() + 1);
            if (size > 65536) throw std::runtime_error("Oversized TUI driver request");
            QByteArray body(size, Qt::Uninitialized);
            if (!std::cin.read(body.data(), size)) throw std::runtime_error("Truncated TUI driver request");
            sent.clear();
            switch (header[0]) {
            case 'F': widget.feed(body); break;
            case 'T': {
                const auto text = QString::fromUtf8(body);
                widget.setPasteSource([text] { return text; }, [] { return false; });
                QKeyEvent press(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier);
                QApplication::sendEvent(&widget, &press);
                QKeyEvent release(QEvent::KeyRelease, Qt::Key_V, Qt::ControlModifier);
                QApplication::sendEvent(&widget, &release); break;
            }
            case 'K': {
                if (size != 8) throw std::runtime_error("Bad TUI key request");
                const int key = static_cast<int>(number(body.constData()));
                const auto mods = Qt::KeyboardModifiers::fromInt(static_cast<int>(number(body.constData() + 4)));
                QKeyEvent press(QEvent::KeyPress, key, mods); QApplication::sendEvent(&widget, &press);
                QKeyEvent release(QEvent::KeyRelease, key, mods); QApplication::sendEvent(&widget, &release); break;
            }
            case 'L': {
                if (size > 256) throw std::runtime_error("Literal key batch exceeds fixture limit");
                for (const auto byte : body) {
                    if (byte < 0x20 || byte > 0x7e) throw std::runtime_error("Literal fixture key must be printable ASCII");
                    const QChar character(static_cast<unsigned char>(byte));
                    const int key = character.toUpper().unicode();
                    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, QString(character));
                    QApplication::sendEvent(&widget, &press);
                    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier); QApplication::sendEvent(&widget, &release);
                }
                break;
            }
            case 'Z': {
                if (size != 8) throw std::runtime_error("Bad TUI size request");
                const auto columns = number(body.constData()), rows = number(body.constData() + 4);
                if (!columns || columns > 160 || !rows || rows > 60) throw std::runtime_error("TUI size outside fixture limits");
                const auto hint = widget.sizeHint(); widget.resize(int(columns) * hint.width() / 80, int(rows) * hint.height() / 24);
                app.processEvents(); break;
            }
            case 'C': {
                QString text; const auto frame = widget.snapshot();
                for (int y = 0; y < frame.rows; ++y) {
                    for (int x = 0; x < frame.columns; ++x) text += frame.cells[y * frame.columns + x].text;
                    text += '\n';
                }
                // Force the real raster paint path, not just the parser model.
                QImage image(widget.size(), QImage::Format_ARGB32_Premultiplied); widget.render(&image);
                reply('C', text.contains(QString::fromUtf8(body)) ? "1" : "0"); continue;
            }
            case 'P': reply('P', QApplication::platformName().toUtf8()); continue;
            default: throw std::runtime_error("Unknown TUI driver request");
            }
            app.processEvents();
            if (failed) throw std::runtime_error("TUI terminal adapter failed");
            reply('R', sent);
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
