#include "terminal/TerminalWidget.h"
#include <QApplication>
#include <QFocusEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace nordssh;
static void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
static QByteArray mouse(TerminalModel& model, QEvent::Type type, QPointF position,
    Qt::MouseButton button = Qt::LeftButton, Qt::MouseButtons held = Qt::LeftButton,
    Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QMouseEvent event(type,position,position,button,held,mods);return model.mouse(event);
}
static void mouse(TerminalWidget& widget, QEvent::Type type, Qt::MouseButton button,
    Qt::MouseButtons held, Qt::KeyboardModifiers mods=Qt::NoModifier, QPointF position={5,5}) {
    QMouseEvent event(type,position,position,button,held,mods);QApplication::sendEvent(&widget,&event);
}
static void wheel(TerminalWidget& widget, int angle, Qt::KeyboardModifiers mods=Qt::NoModifier) {
    QWheelEvent event({5,5},{5,5},{},{0,angle},Qt::NoButton,mods,Qt::NoScrollPhase,false);
    QApplication::sendEvent(&widget,&event);
}
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    try {
        TerminalModel model(80,24);model.resize(80,24,10,20);
        require(mouse(model,QEvent::MouseButtonPress,{25,45}).isEmpty(),"No reports without terminal opt-in");
        model.feed("\x1b[?1000h\x1b[?1006h");
        require(mouse(model,QEvent::MouseButtonPress,{25,45})=="\x1b[<0;3;3M","SGR left press uses cell coordinates");
        require(mouse(model,QEvent::MouseButtonRelease,{25,45},Qt::LeftButton,Qt::NoButton)=="\x1b[<0;3;3m","SGR release");
        require(mouse(model,QEvent::MouseButtonPress,{25,45},Qt::MiddleButton,Qt::MiddleButton,Qt::ControlModifier|Qt::AltModifier)=="\x1b[<25;3;3M","Middle and modifier mapping");
        require(mouse(model,QEvent::MouseMove,{35,45},Qt::NoButton,Qt::LeftButton).isEmpty(),"Normal mode suppresses motion");
        model.feed("\x1b[?1002h");
        require(mouse(model,QEvent::MouseMove,{45,45},Qt::NoButton,Qt::LeftButton)=="\x1b[<32;5;3M","Button drag tracking");
        require(mouse(model,QEvent::MouseMove,{46,46},Qt::NoButton,Qt::LeftButton).isEmpty(),"Same cell motion deduplicates");
        require(mouse(model,QEvent::MouseMove,{55,45},Qt::NoButton,Qt::NoButton).isEmpty(),"Button mode suppresses hover");
        model.feed("\x1b[?1003h");
        require(mouse(model,QEvent::MouseMove,{65,45},Qt::NoButton,Qt::NoButton)=="\x1b[<35;7;3M","Any mode permits hover");
        model.feed("\x1b[?1016h");
        require(mouse(model,QEvent::MouseButtonPress,{25,45})=="\x1b[<0;25;45M","SGR pixel coordinates use logical surface geometry");
        model.feed("\x1b[?1016l\x1b[?1006l\x1b[?1003l\x1b[?1000h");
        require(mouse(model,QEvent::MouseButtonPress,{25,45})==QByteArray::fromHex("1b5b4d202323"),"Legacy X10 byte encoding");
        require(mouse(model,QEvent::MouseButtonRelease,{25,45},Qt::LeftButton,Qt::NoButton)==QByteArray::fromHex("1b5b4d232323"),"Legacy release button 3");
        bool rejected=false;
        try {static_cast<void>(mouse(model,QEvent::MouseButtonPress,{std::numeric_limits<double>::quiet_NaN(),1}));}
        catch(const std::invalid_argument&){rejected=true;}
        require(rejected,"Nonfinite mouse position rejected before C ABI");
        model.feed("\x1b[?1006h");
        model.resize(100,40,5,10);
        require(mouse(model,QEvent::MouseButtonPress,{25,45})=="\x1b[<0;6;5M","Mouse geometry follows terminal resize");
        QWheelEvent direct({5,5},{5,5},{},{0,120},Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
        require(model.wheel(direct,1)=="\x1b[<64;2;1M"&&model.wheel(direct,-1)=="\x1b[<65;2;1M","Wheel direction buttons");
        rejected=false;try{static_cast<void>(model.wheel(direct,33));}catch(const std::invalid_argument&){rejected=true;}
        require(rejected,"Direct wheel batch is bounded");

        TerminalWidget widget;widget.resize(widget.sizeHint());widget.show();app.processEvents();
        QByteArray sent;int pastes=0,failures=0;
        widget.setSend([&](const QByteArray& bytes){sent+=bytes;});
        widget.setFailure([&]{++failures;});
        widget.setPasteSource([&]{++pastes;return QString("synthetic-paste");},[]{return false;});
        widget.feed("\x1b[?1000h\x1b[?1006h");
        mouse(widget,QEvent::MouseButtonPress,Qt::LeftButton,Qt::LeftButton);
        mouse(widget,QEvent::MouseButtonRelease,Qt::LeftButton,Qt::NoButton);
        require(sent=="\x1b[<0;1;1M\x1b[<0;1;1m","Widget sends paired remote clicks exactly once");sent.clear();
        wheel(widget,60);require(sent.isEmpty(),"Partial remote wheel waits for detent");
        wheel(widget,60);require(sent=="\x1b[<64;1;1M","Partial remote wheel accumulates once");sent.clear();
        wheel(widget,120,Qt::ShiftModifier);require(sent.isEmpty(),"Shift wheel remains local");
        mouse(widget,QEvent::MouseButtonPress,Qt::LeftButton,Qt::LeftButton,Qt::ShiftModifier);
        mouse(widget,QEvent::MouseButtonRelease,Qt::LeftButton,Qt::NoButton);
        require(sent.isEmpty(),"Shift gesture release cannot leak remote bytes");
        mouse(widget,QEvent::MouseButtonPress,Qt::RightButton,Qt::RightButton);
        mouse(widget,QEvent::MouseButtonRelease,Qt::RightButton,Qt::NoButton);
        require(pastes==1&&sent=="synthetic-paste","Default right-click remains single paste in mouse mode");sent.clear();
        widget.setRightClickPaste(false);
        mouse(widget,QEvent::MouseButtonPress,Qt::RightButton,Qt::RightButton);
        mouse(widget,QEvent::MouseButtonRelease,Qt::RightButton,Qt::NoButton);
        require(pastes==1&&sent=="\x1b[<2;1;1M\x1b[<2;1;1m","Disabled right paste routes right clicks to TUI");sent.clear();
        mouse(widget,QEvent::MouseButtonPress,Qt::LeftButton,Qt::LeftButton);
        QFocusEvent lost(QEvent::FocusOut);QApplication::sendEvent(&widget,&lost);
        mouse(widget,QEvent::MouseButtonRelease,Qt::LeftButton,Qt::NoButton);
        require(sent=="\x1b[<0;1;1M\x1b[<0;1;1m","Focus loss releases once, late release suppressed");sent.clear();
        mouse(widget,QEvent::MouseButtonPress,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier,{-10,-10});
        mouse(widget,QEvent::MouseButtonRelease,Qt::LeftButton,Qt::NoButton);
        require(sent.isEmpty(),"Rejected outside press cannot invent a release");
        widget.feed("\x1b[?1002h");
        widget.setRightClickPaste(true);
        mouse(widget,QEvent::MouseButtonPress,Qt::RightButton,Qt::RightButton);sent.clear();
        mouse(widget,QEvent::MouseMove,Qt::NoButton,Qt::RightButton,Qt::NoModifier,{25,25});
        mouse(widget,QEvent::MouseButtonRelease,Qt::RightButton,Qt::NoButton);
        require(sent.isEmpty(),"Paste-button drag never becomes a remote drag");
        mouse(widget,QEvent::MouseButtonPress,Qt::LeftButton,Qt::LeftButton,Qt::ShiftModifier);
        mouse(widget,QEvent::MouseMove,Qt::NoButton,Qt::LeftButton,Qt::NoModifier,{25,25});
        mouse(widget,QEvent::MouseButtonRelease,Qt::LeftButton,Qt::NoButton);
        require(sent.isEmpty(),"Local gesture stays local after Shift is released");
        wheel(widget,std::numeric_limits<int>::max());
        require(sent.size()<=32*128&&failures==0,"Hostile wheel event has bounded protocol output");
        auto* closing = new TerminalWidget;
        closing->feed("\x1b[?1000h\x1b[?1006h");
        closing->setSend([closing](const QByteArray&){delete closing;});
        mouse(*closing,QEvent::MouseButtonPress,Qt::LeftButton,Qt::LeftButton);
        std::cout<<"Native mouse encoder/routing gate passed; real desktop and fullscreen TUI acceptance pending\n";
        return EXIT_SUCCESS;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return EXIT_FAILURE;}
}
