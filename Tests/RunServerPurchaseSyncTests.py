"""Link real server widgets and networking, then verify sync and render the approved layout."""
from pathlib import Path
import re
import sys
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/qt-release/DF_PlayClick_Server_Qt'
OUT = ROOT / 'build/server-purchase-runtime'
OUT.mkdir(parents=True, exist_ok=True)
make = (BUILD / 'Makefile.Release').read_text()

def variable(name):
    return re.search(r'^' + name + r'\s*=\s*(.*)$', make.replace('\\\n', ' '), re.M).group(1)

objects = [name for name in variable('OBJECTS').split()
           if Path(name.replace('\\', '/')).name.lower() not in ('main.obj', 'mainwindow.obj', 'mainwindowpurchaseui.obj', 'serverconnectionmanager.obj')]
(OUT / 'link.rsp').write_text('/NOLOGO /SUBSYSTEM:CONSOLE /OUT:P:/build/server-purchase-runtime/ServerPurchaseSyncTests.exe '
    + 'P:/build/server-purchase-runtime/test.obj ' + ' '.join(objects) + ' ' + variable('LIBS'))
batch = '@echo off\ncall "E:\\VisualStudio\\VisualStudioIDE\\Common7\\Tools\\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul\n'
batch += 'cd /d P:\\build\\qt-release\\DF_PlayClick_Server_Qt\n'
batch += 'cl /nologo /c /std:c++17 /Zc:__cplusplus /permissive- /EHsc /MT /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DQT_STATIC /DQT_WIDGETS_LIB /DQT_GUI_LIB /DQT_CORE_LIB '
batch += variable('INCPATH') + ' /FoP:\\build\\server-purchase-runtime\\test.obj P:\\Tests\\ServerPurchaseSyncTests.cpp\n'
batch += 'if errorlevel 1 exit /b 1\nlink @P:\\build\\server-purchase-runtime\\link.rsp\n'
(OUT / 'build-test.cmd').write_text(batch)
subprocess.run(['cmd', '/c', str(OUT / 'build-test.cmd')], cwd=BUILD, check=True)
fixture = next((arg for arg in sys.argv if arg in ("--qt-auto-relist", "--qt-force-restart", "--manual-success-only")), str(ROOT / 'Tests/Fixtures/AuctionAmmoOptions.json'))
result = subprocess.run([str(OUT / 'ServerPurchaseSyncTests.exe'), fixture], cwd=OUT, timeout=60, capture_output=True)
print(result.stdout.decode('utf-8', errors='replace'))
print(result.stderr.decode('utf-8', errors='replace'))
assert result.returncode == 0, result.returncode
