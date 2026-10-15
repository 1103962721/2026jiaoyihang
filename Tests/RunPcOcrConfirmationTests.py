"""Run the production PC OCR loops with deterministic OCR/input/wait doubles.

Extract the actual local lambdas/loops so tests exercise controller code without
sending mouse input to a real game or waiting for network OCR.
"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/pc-ocr-confirmation'
OUT.mkdir(parents=True, exist_ok=True)
source = (ROOT / 'DF_PlayClick_Client_Qt/ClientController.cpp').read_text(encoding='utf-8-sig')
start = source.index('    const auto findWithScroll =')
end = source.index('\n    SetForegroundWindow', start)
scroll = source[start:end]
panels = []
for function in ('executeBossListingSequence', 'executeListingSequence'):
    start = source.index('bool ClientController::' + function + '(')
    start = source.index('            OcrRegionConfig panelQuantityRegion;', start)
    end = source.index('            if (!click(inputPoint)', start)
    panels.append(source[start:end])
assert 'clickFoundText(listRegion, itemBounds)' in source

cpp = r'''
#include <QtCore>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <functional>
struct POINT { int x, y; };
constexpr int WHEEL_DELTA = 120;
struct OcrRegionConfig {
    bool enabled=false;
    int x=0,y=0,width=0,height=0,referenceWidth=0,referenceHeight=0;
};
static void check(bool ok, const char* message) {
    if (!ok) { fprintf(stderr,"FAIL %s\n",message); std::exit(1); }
}
struct Config {
    QString loadUmiOcrPath() { return {}; }
    int loadUmiOcrPort() { return 0; }
};
struct Sample { bool found; QRect bounds; QString text; };
struct Fixture {
    Config config_;
    std::vector<Sample> samples;
    std::vector<int> waits, wheels;
    std::vector<OcrRegionConfig> regions;
    QStringList events;
    int reads=0, captures=0, clicks=0;
    bool cancelled=false, cancelWait=false;
    QString expectedKeyword;
    struct Runner {
        Fixture* f;
        bool findTextBounds(const QString&, const QString&, int, const QString& keyword,
            QRect& bounds, QString&, bool exact) {
            check(keyword==f->expectedKeyword && exact,"confirmation must re-read same exact item");
            check(f->reads<int(f->samples.size()),"unexpected extra OCR");
            f->events << "ocr";
            const auto s=f->samples[f->reads++];
            if(s.found) bounds=s.bounds;
            return s.found;
        }
        QString recognizeImage(const QString&, const QString&, int, QString&) {
            check(f->reads<int(f->samples.size()),"panel retry did not terminate");
            return f->samples[f->reads++].text;
        }
    } umiOcrRunner_{this};
    struct Input {
        Fixture* f;
        bool scrollReferencePoint(int,POINT,int,int,int delta,QString&) {
            f->wheels.push_back(delta); f->events << "scroll"; return true;
        }
    } input_{this};
    int boundHwnd_=1;
    QString captureOcrRegion(const OcrRegionConfig& r,const QString&,QString&) {
        regions.push_back(r); ++captures; events << "capture";
        return {}; // replaced below with a non-file token; QFile::remove is harmless
    }
    bool waitForAutomation(int ms,QString&) {
        waits.push_back(ms); events << QString("wait%1").arg(ms);
        if(cancelWait && ms==3000) { cancelled=true; return false; }
        return true;
    }
    void connectionLogAppended(const QString&) {}
    bool find(const QString& keyword,QRect& bounds) {
        expectedKeyword=keyword;
        QString errorText;
        constexpr int ReferenceWidth=2560,ReferenceHeight=1440;
        OcrRegionConfig listRegion; listRegion.x=119; listRegion.y=246;
        listRegion.width=460; listRegion.height=1152;
        const auto stopIfRequested=[&](){return cancelled;};
        // SCROLL
        return findWithScroll(keyword,QStringLiteral("test_item"),bounds);
    }
    bool panel(int mode) {
        QString errorText;
        constexpr int ReferenceWidth=2560,ReferenceHeight=1440;
        const POINT panelPoint{1930,700};
        QString filePrefix="listing";
        int attempt=0;
        const auto stopIfRequested=[&](){return cancelled;};
        const auto click=[&](POINT){ ++clicks; return true; };
        if(mode==0) { /* PANEL0 */ }
        else { /* PANEL1 */ }
        return true;
    }
};
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    const QRect oldPosition(10,20,70,20), newPosition(10,100,70,20);
    for(const QString& item : {QStringLiteral("weapon"),QStringLiteral("backpack"),QStringLiteral("egg")}) {
        Fixture f; f.samples={{true,oldPosition,{}},{true,newPosition,{}}};
        QRect result;
        check(f.find(item,result) && result==newPosition,"must use second OCR bounds after scroll settles");
        check(f.waits==std::vector<int>{3000} && f.wheels.empty(),"wait exactly 3s without scrolling before confirmation");
        check(f.events==QStringList{"capture","ocr","wait3000","capture","ocr"},"fresh capture after delay");
    }
    {
        Fixture f; f.samples={{true,oldPosition,{}},{false,{},{}},{true,oldPosition,{}},{true,newPosition,{}}};
        QRect result;
        check(f.find("weapon",result) && result==newPosition,"target gone at confirmation must search again");
        check(f.wheels==std::vector<int>{-600},"failed confirmation must take original scroll path");
        check(f.waits==std::vector<int>{3000,150,3000},"every new candidate requires a new 3s confirmation");
    }
    {
        Fixture f; f.cancelWait=true; f.samples={{true,oldPosition,{}}};
        QRect result;
        check(!f.find("weapon",result) && f.reads==1 && f.wheels.empty(),"cancel wait prevents second OCR/scroll/click");
    }
    for(int mode : {0,1}) {
        for(const QString& text : {QStringLiteral("使用售位：9/15"),QStringLiteral("使用售位：11/15")}) {
            Fixture f; f.samples={{true,{},text}};
            check(f.panel(mode) && f.clicks==0,"single/double digit panel current must continue immediately");
            for(const auto& r:f.regions)
                check(r.x==1786 && r.y==608 && r.width==292 && r.height==115
                    && r.referenceWidth==2560 && r.referenceHeight==1440,"PC panel crop/reference mismatch");
        }
        Fixture f; f.samples={{true,{},"1/15"},{true,{},"11/15"}};
        check(f.panel(mode) && f.clicks==1,"X=1 stays blocked; X=11 stops retrying");
    }
    puts("PASS both PC panel crops, 9/15 and 11/15 accepted, 1/15 retried");
    puts("PASS weapon/backpack/egg fresh capture after 3000ms; moved bounds used");
    puts("PASS missing confirmation resumes scrolling; cancellation aborts without extra action");
}
'''
cpp = cpp.replace('return {}; // replaced below with a non-file token; QFile::remove is harmless',
                  'return QStringLiteral(":/mock-frame");')
cpp = cpp.replace('// SCROLL', scroll).replace('/* PANEL0 */', panels[0]).replace('/* PANEL1 */', panels[1])
(OUT / 'PcOcrConfirmationTests.cpp').write_text(cpp, encoding='utf-8-sig')
(OUT / 'PcOcrConfirmationTests.pro').write_text('''TEMPLATE = app
TARGET = PcOcrConfirmationTests
QT = core
CONFIG += console c++17 release
CONFIG -= debug app_bundle
QMAKE_CXXFLAGS += /utf-8
SOURCES += PcOcrConfirmationTests.cpp
''')
bat = OUT / 'run.cmd'
bat.write_text('''@echo off
call "E:\\VisualStudio\\VisualStudioIDE\\Common7\\Tools\\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
cd /d P:\\build\\pc-ocr-confirmation
Q:\\bin\\qmake.exe PcOcrConfirmationTests.pro
if errorlevel 1 exit /b 1
nmake /nologo
if errorlevel 1 exit /b 1
release\\PcOcrConfirmationTests.exe
''')
subprocess.run(['cmd','/c',str(bat)],cwd=OUT,check=True)
