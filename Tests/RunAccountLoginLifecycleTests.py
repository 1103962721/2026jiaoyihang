"""Exercise production login scheduling with mocked OS/UI operations, never real accounts.

Requires the existing P:/Q: build mappings. Extracts the unchanged lifecycle
method bodies from source; all platform operations are supplied by the harness.
"""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/account-login-tests'
OUT.mkdir(parents=True, exist_ok=True)
source = (ROOT / 'DF_PlayClick_Client_Qt/AccountLoginAutomation.cpp').read_text(encoding='utf-8-sig')

def method(name):
    match = re.search(r'^.*AccountLoginAutomation::' + re.escape(name) + r'\(', source, re.M)
    assert match, name
    opening = source.index('{', match.start())
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]

methods = '\n'.join(method(name) for name in [
    'AccountLoginAutomation', '~AccountLoginAutomation', 'submit', 'workerLoop',
    'postResult', 'reportProgress', 'reportLog', 'reportFinished', 'start',
    'runLogin', 'cancel', 'cancelAndStopProcesses', 'cancelled'])

harness = r'''
#include "AccountLoginAutomation.h"
#include <QCoreApplication>
#include <QThread>
#include <QElapsedTimer>
#include <QDateTime>
#include <iostream>
#include <cstdlib>
#include <vector>
namespace {
std::mutex mockMutex;
std::vector<QString> events;
std::atomic_bool entered{false}, releaseOld{false}, cleanupEntered{false}, releaseCleanup{false};
std::atomic_int windowChecks{0};
std::atomic_bool failLauncherAfterSteamExit{false};
std::atomic_bool failLoginOnce{false};
std::atomic_int mockGameLaunches{0};
void record(const QString& text) { std::lock_guard<std::mutex> l(mockMutex); events.push_back(text); }
void require(bool ok, const char* message) { if (!ok) { std::cerr << message << '\n'; std::exit(1); } }
bool saw(const QString& text) {
    std::lock_guard<std::mutex> l(mockMutex);
    return std::find(events.begin(), events.end(), text) != events.end();
}
template<class F> void waitFor(F predicate) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < 3000) QThread::msleep(1);
    require(predicate(), "mock timed out");
}
void drainEvents() { for (int i=0; i<20; ++i) { QCoreApplication::processEvents(); QThread::msleep(1); } }
}
bool AccountLoginAutomation::stopProcesses(QString&, const QString& reason) {
    record("cleanup:" + workerTask_->username + ":" + reason);
    if (reason == "hold cleanup") {
        cleanupEntered = true;
        while (!releaseCleanup) QThread::msleep(1);
        record("cleanup returned");
    }
    return !cancelled();
}
bool AccountLoginAutomation::launchSteam(QString&) const {
    const QString original = workerTask_->username;
    record("steam:" + original);
    if (original == "old") {
        entered = true;
        while (!releaseOld) QThread::msleep(1);
        require(workerTask_->username == original, "credentials changed under old worker");
        return false; // Late failure must not kill the replacement's processes.
    }
    if (original == "shutdown") {
        entered = true;
        while (!cancelled()) QThread::msleep(1);
        record("shutdown worker returned");
        return false;
    }
    return true;
}
bool AccountLoginAutomation::performSteamLogin(QString&, bool& steamExited) {
    steamExited = workerTask_->username == "login-exit" && failLoginOnce.exchange(false);
    if (steamExited || workerTask_->username == "post-login-failure") return false;
    return !cancelled();
}
bool AccountLoginAutomation::launchGame(QString&) const { ++mockGameLaunches; record("game:" + workerTask_->username); return !cancelled(); }
bool AccountLoginAutomation::prepareAndStartLauncher(QString&, bool& stopRetrying, bool& steamExited) {
    stopRetrying = false;
    steamExited=failLauncherAfterSteamExit.exchange(false);
    return !steamExited && !cancelled();
}
bool AccountLoginAutomation::waitForGameWindow(int) { ++windowChecks; return !cancelled(); }
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    {
        AccountLoginAutomation login;
        int reports = 0;
        QObject::connect(&login, &AccountLoginAutomation::finished, [&](bool ok, const QString&) {
            require(ok, "stale failure delivered"); ++reports;
        });
        login.start("old", "mock");
        waitFor([] { return entered.load(); });
        login.start("new", "mock");
        QThread::msleep(15);
        require(!saw("steam:new"), "replacement overlapped old worker");
        releaseOld = true;
        waitFor([] { return windowChecks.load() == 1; });
        QThread::msleep(15); // Queue the result but do not deliver it yet.
        login.start("latest", "mock");
        waitFor([] { return windowChecks.load() == 2; });
        drainEvents();
        require(reports == 1, "stale queued completion was not filtered");
        {
            std::lock_guard<std::mutex> l(mockMutex);
            require(std::count_if(events.begin(), events.end(), [](const QString& e) {
                return e.startsWith("cleanup:old:"); }) == 1, "cancelled worker ran failure cleanup");
        }
        std::thread stop([&] { login.cancelAndStopProcesses("hold cleanup"); });
        stop.join();
        waitFor([] { return cleanupEntered.load(); });
        login.start("after-stop", "mock");
        QThread::msleep(15);
        require(!saw("steam:after-stop"), "new login started before cleanup returned");
        releaseCleanup = true;
        waitFor([] { return windowChecks.load() == 3; });
        drainEvents();
        require(reports == 2, "restart did not complete");
        size_t before;
        { std::lock_guard<std::mutex> l(mockMutex); before = events.size(); }
        login.cancel();
        QThread::msleep(15);
        { std::lock_guard<std::mutex> l(mockMutex); require(events.size() == before, "idle cancel killed logged-in processes"); }
    }
    {
        AccountLoginAutomation login;
        std::atomic_bool failed{false};
        QObject::connect(&login, &AccountLoginAutomation::finished, [&](bool ok, const QString&) { failed = !ok; });
        const int launchesBefore = mockGameLaunches.load();
        login.start("post-login-failure", "mock");
        waitFor([&] { QCoreApplication::processEvents(); return failed.load(); });
        {
            std::lock_guard<std::mutex> l(mockMutex);
            require(std::count_if(events.begin(), events.end(), [](const QString& e) {
                return e == "cleanup:post-login-failure:上号开始清理旧进程";
            }) == 1, "post-login launcher failure ran destructive failure cleanup");
        }
        require(mockGameLaunches.load() == launchesBefore, "login timeout sent a game URI");
    }
    {
        AccountLoginAutomation login;
        std::atomic_bool finished{false};
        QObject::connect(&login, &AccountLoginAutomation::finished, [&](bool ok, const QString&) {
            require(ok, "launcher exit did not recover"); finished = true;
        });
        const int launchesBefore = mockGameLaunches.load();
        failLauncherAfterSteamExit = true;
        login.start("launcher-exit", "mock");
        waitFor([&] { QCoreApplication::processEvents(); return finished.load(); });
        require(mockGameLaunches.load() == launchesBefore + 2, "Steam exit did not restart login before retrying game URI");
        {
            std::lock_guard<std::mutex> l(mockMutex);
            require(std::count_if(events.begin(), events.end(), [](const QString& e) {
                return e == "steam:launcher-exit";
            }) == 2, "launcher exit did not relaunch Steam");
        }
    }
    {
        AccountLoginAutomation login;
        std::atomic_bool finished{false};
        QObject::connect(&login, &AccountLoginAutomation::finished, [&](bool ok, const QString&) {
            require(ok, "login-stage Steam exit did not recover"); finished = true;
        });
        failLoginOnce = true;
        login.start("login-exit", "mock");
        waitFor([&] { QCoreApplication::processEvents(); return finished.load(); });
        std::lock_guard<std::mutex> l(mockMutex);
        require(std::count_if(events.begin(), events.end(), [](const QString& e) {
            return e == "steam:login-exit";
        }) == 2, "login-stage exit did not relaunch Steam");
    }
    entered = false;
    {
        AccountLoginAutomation login;
        login.start("shutdown", "mock");
        waitFor([] { return entered.load(); });
    }
    require(saw("shutdown worker returned"), "destructor did not join worker");
    std::cout << "PASS: replacement isolation, stale callback filtering, stop/restart ordering, idle cancel, shutdown join\n";
}
'''
(OUT / 'lifecycle.cpp').write_text(harness + '\n' + methods, encoding='utf-8')
make = (ROOT / 'build/qt-release/DF_PlayClick_Client_Qt/Makefile.Release').read_text()
libs = re.search(r'^LIBS\s*=\s*(.*)', make, re.M).group(1)
libs = re.sub(r'\S+DF_PlayClick_Client_Qt_resource\.res', '', libs)
libs = re.sub(r'\S+Qt6EntryPoint\.lib', '', libs)
(OUT / 'link.rsp').write_text('/NOLOGO /SUBSYSTEM:CONSOLE /OUT:lifecycle.exe lifecycle.obj moc.obj ' + libs)
bat = r'''@echo off
call "E:\VisualStudio\VisualStudioIDE\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
cd /d P:\build\account-login-tests
Q:\bin\moc.exe P:\DF_PlayClick_Client_Qt\AccountLoginAutomation.h -o moc.cpp
if errorlevel 1 exit /b 1
cl /nologo /c /std:c++17 /Zc:__cplusplus /permissive- /EHsc /MT /utf-8 /DQT_STATIC /DQT_CORE_LIB /DNOMINMAX /IQ:\include /IQ:\include\QtCore /IP:\DF_PlayClick_Client_Qt lifecycle.cpp moc.cpp
if errorlevel 1 exit /b 1
link @link.rsp
if errorlevel 1 exit /b 1
lifecycle.exe
'''
(OUT / 'run.cmd').write_text(bat)
subprocess.run(['cmd', '/c', str(OUT / 'run.cmd')], check=True, timeout=90)
