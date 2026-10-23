#include "terminal/TerminalWidget.h"
#include <QApplication>
#include <QKeyEvent>
#include <QFocusEvent>
#include <QWheelEvent>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace nordssh;
static void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
static QString firstRow(const TerminalFrame& frame) {
    QString value;
    for(int x=0;x<frame.columns;++x) value+=frame.cells[x].text;
    return value;
}
static void wheel(TerminalWidget& widget,int angle,int pixels=0,bool inverted=false,
                  Qt::KeyboardModifiers modifiers=Qt::NoModifier) {
    QWheelEvent event(QPointF(5,5),QPointF(5,5),QPoint(0,pixels),QPoint(0,angle),
        Qt::NoButton,modifiers,Qt::NoScrollPhase,inverted);
    QApplication::sendEvent(&widget,&event);
}
static void key(TerminalWidget& widget,int value,Qt::KeyboardModifiers modifiers, QEvent::Type type=QEvent::KeyPress) {
    QKeyEvent event(type,value,modifiers);QApplication::sendEvent(&widget,&event);
}
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    try {
        TerminalModel model(40,5);
        for(int i=0;i<20;++i)model.feed("row-"+QByteArray::number(i)+"\r\n");
        require(model.frame().followingOutput,"Initial viewport follows output");
        model.scrollToTop();auto top=model.frame();
        require(firstRow(top).startsWith("row-0")&&!top.followingOutput&&!top.cursorVisible,"Top/history cursor mapping");
        model.scroll(3);require(firstRow(model.frame()).startsWith("row-3"),"Relative history rows");
        model.feed("new-output\r\n");
        require(firstRow(model.frame()).startsWith("row-3"),"Incoming output does not jump a history reader");
        model.scroll(-10000);require(firstRow(model.frame()).startsWith("row-0"),"Scroll clamps to retained history");
        model.scrollToBottom();require(model.frame().followingOutput,"Return to live output");
        model.feed("\x1b[?1049h\x1b[HALT");model.scrollToTop();
        require(model.frame().followingOutput&&firstRow(model.frame()).contains("ALT"),"Alternate screen has no primary history");
        model.feed("\x1b[?1049l");
        bool rejected=false;
        try {model.scroll(std::numeric_limits<int>::min());}catch(const std::invalid_argument&){rejected=true;}
        require(rejected,"Oversized direct API scroll rejected before engine call");

        TerminalWidget widget;
        QByteArray sent;int failures=0;
        widget.setSend([&](const QByteArray& bytes){sent+=bytes;});
        widget.setFailure([&]{++failures;});
        widget.resize(widget.sizeHint());widget.show();app.processEvents();
        for(int i=0;i<80;++i)widget.feed("line-"+QByteArray::number(i)+"\r\n");
        auto active=firstRow(widget.snapshot());
        wheel(widget,60);require(widget.snapshot().followingOutput,"High-resolution wheel waits for a whole detent");
        wheel(widget,60);require(!widget.snapshot().followingOutput&&firstRow(widget.snapshot())!=active,"Two partial deltas scroll once");
        auto historic=firstRow(widget.snapshot());
        widget.feed("new-line\r\n");require(firstRow(widget.snapshot())==historic,"Widget preserves viewed history during output");
        require(sent.isEmpty(),"Local history navigation never sends bytes to SSH");
        widget.setPasteSource([]{return QString("one\ntwo");},[]{return false;});
        key(widget,Qt::Key_V,Qt::ControlModifier);
        require(!widget.snapshot().followingOutput&&firstRow(widget.snapshot())==historic&&sent.isEmpty(),
            "Cancelled paste preserves viewed history and writes nothing");
        key(widget,Qt::Key_End,Qt::ShiftModifier);
        key(widget,Qt::Key_End,Qt::NoModifier,QEvent::KeyRelease);
        require(widget.snapshot().followingOutput&&sent.isEmpty(),"Shift+End local press/release");
        wheel(widget,120,0,true);
        require(!widget.snapshot().followingOutput,"OS wheel direction isn't inverted twice");
        key(widget,Qt::Key_C,Qt::ControlModifier);
        require(widget.snapshot().followingOutput&&sent==QByteArray(1,'\x03'),"Ctrl+C returns to output and sends one interrupt");
        sent.clear();
        widget.feed("\x1b[>3u"); // Kitty reports key releases as well as presses.
        key(widget,Qt::Key_PageUp,Qt::ShiftModifier);
        QFocusEvent lost(QEvent::FocusOut);QApplication::sendEvent(&widget,&lost);
        key(widget,Qt::Key_PageUp,Qt::NoModifier,QEvent::KeyRelease);
        require(!widget.snapshot().followingOutput&&sent.isEmpty(),"Local navigation release never leaks into Kitty protocol");
        widget.feed("\x1b[<u");
        key(widget,Qt::Key_End,Qt::ShiftModifier);key(widget,Qt::Key_End,Qt::NoModifier,QEvent::KeyRelease);
        widget.feed("\x1b[?1000h");
        wheel(widget,120);require(widget.snapshot().followingOutput,"Remote mouse mode is not reinterpreted as history");
        wheel(widget,120,0,false,Qt::ShiftModifier);require(!widget.snapshot().followingOutput,"Shift explicitly overrides remote mouse mode for local history");
        widget.feed("\x1b[?1000l");
        widget.setPasteSource([]{return QString("captured-paste");},[]{return false;});
        key(widget,Qt::Key_V,Qt::ControlModifier);
        require(widget.snapshot().followingOutput&&sent=="captured-paste","Accepted paste follows output exactly once");
        wheel(widget,std::numeric_limits<int>::min(),std::numeric_limits<int>::max());
        require(failures==0&&widget.isEnabled(),"Extreme wheel deltas remain bounded without disabling terminal");
        std::cout<<"Native bounded history/wheel/navigation gate passed; remote mouse reporting and desktop wheel are not asserted\n";
        return EXIT_SUCCESS;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return EXIT_FAILURE;}
}
