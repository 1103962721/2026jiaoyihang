"""Exercise production UIA parameter sync and controller deferral in an isolated Qt process."""
from pathlib import Path
import re
import os
import sys
import subprocess
from NativeQtTestDesktop import run_fixture

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/qt-release/DF_PlayClick_Client_Qt'
OUT = ROOT / 'build/auction-purchase-runtime'
OUT.mkdir(parents=True, exist_ok=True)
make = (BUILD / 'Makefile.Release').read_text()

def variable(name):
    return re.search(r'^' + name + r'\s*=\s*(.*)$', make.replace('\\\n', ' '), re.M).group(1)

objects = [name for name in variable('OBJECTS').split()
           if Path(name.replace('\\', '/')).name.lower() not in ('main.obj', 'clientcontroller.obj')]
(OUT / 'link.rsp').write_text('/NOLOGO /SUBSYSTEM:CONSOLE /OUT:P:/build/auction-purchase-runtime/AuctionQtControl.exe '
    + 'P:/build/auction-purchase-runtime/test.obj ' + ' '.join(objects) + ' ' + variable('LIBS'))
batch = '@echo off\ncall "E:\\VisualStudio\\VisualStudioIDE\\Common7\\Tools\\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul\n'
batch += 'cd /d P:\\build\\qt-release\\DF_PlayClick_Client_Qt\n'
batch += 'cl /nologo /c /std:c++17 /Zc:__cplusplus /permissive- /EHsc /MT /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DQT_STATIC /DQT_WIDGETS_LIB /DQT_GUI_LIB /DQT_CORE_LIB '
batch += variable('INCPATH') + ' /FoP:\\build\\auction-purchase-runtime\\test.obj P:\\Tests\\AuctionPurchaseAutomationTests.cpp\n'
batch += 'if errorlevel 1 exit /b 1\nlink @P:\\build\\auction-purchase-runtime\\link.rsp\n'
(OUT / 'build-test.cmd').write_text(batch)
subprocess.run(['cmd', '/c', str(OUT / 'build-test.cmd')], cwd=BUILD, check=True)
if "--focused" in sys.argv:
    os.environ["CODEX_FOCUSED_SYNC_TEST"] = "1"
if '--save-ready-only' in sys.argv:
    os.environ['CODEX_SAVE_READY_TEST'] = '1'
result = run_fixture(OUT / 'AuctionQtControl.exe', OUT, timeout=240)
print(result.stdout.decode('utf-8', errors='replace'))
print(result.stderr.decode('utf-8', errors='replace'))
assert result.returncode == 0, result.returncode
