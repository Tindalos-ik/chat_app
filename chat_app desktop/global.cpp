#include "global.h"
#include <QChar>

/*void(QWidget*) 表示：接受一个 QWidget* 参数，返回 void
 *  这是一个"刷新样式"的操作，流程如下：
 * 1. unpolish：清理旧的样式设置（移除样式表、颜色、字体等）
 * 2. polish：重新应用样式设置（重新读取样式表、应用QSS等）*/
std::function<void(QWidget*)> repolish = [](QWidget* w){
    w->style()->unpolish(w);
    w->style()->polish(w);
};


std::function<QString(QString)> xorString = [](QString input){
    QString res = input;
    int len = input.length();
    len = len % 255;
    for(int i=0; i<len; i++){
        //对每个字符进行xor操作
        //注意：这里假设字符都是ASCII，因此直接转换为QChar
        res[i] = QChar(static_cast<ushort>(input[i].unicode() ^ static_cast<ushort>(len)));
    }
    return res;
};

QString gate_url_prefix = "";

