"""Link the production controller/Qt objects into an isolated Qt UIA fixture."""
from pathlib import Path
import re
import subprocess
import sys
import os
from NativeQtTestDesktop import run_fixture

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/qt-release/DF_PlayClick_Client_Qt'
OUT = ROOT / 'build/auction-controller-runtime'
OUT.mkdir(parents=True, exist_ok=True)
make = (BUILD / 'Makefile.Release').read_text()

def variable(name):
    return re.search(r'^' + name + r'\s*=\s*(.*)$', make.replace('\\\n', ' '), re.M).group(1)

objects = [name for name in variable('OBJECTS').split()
           if Path(name.replace('\\', '/')).name.lower() not in ('main.obj', 'clientcontroller.obj')]
(OUT / 'link.rsp').write_text('/NOLOGO /SUBSYSTEM:CONSOLE /OUT:P:/build/auction-controller-runtime/AuctionQtControl.exe '
    + 'P:/build/auction-controller-runtime/test.obj ' + ' '.join(objects) + ' ' + variable('LIBS'))
batch = '@echo off\ncall "E:\\VisualStudio\\VisualStudioIDE\\Common7\\Tools\\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul\n'
batch += 'cd /d P:\\build\\qt-release\\DF_PlayClick_Client_Qt\n'
batch += 'cl /nologo /c /std:c++17 /Zc:__cplusplus /permissive- /EHsc /MT /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DQT_STATIC /DQT_WIDGETS_LIB /DQT_GUI_LIB /DQT_CORE_LIB /DAUCTION_CONTROLLER_RUNTIME '
batch += variable('INCPATH') + ' /FoP:\\build\\auction-controller-runtime\\test.obj P:\\Tests\\AuctionControlAutomationTests.cpp\n'
batch += 'if errorlevel 1 exit /b 1\nlink @P:\\build\\auction-controller-runtime\\link.rsp\n'
(OUT / 'build-test.cmd').write_text(batch)
subprocess.run(['cmd', '/c', str(OUT / 'build-test.cmd')], cwd=BUILD, check=True)
if '--sell-target-only' in sys.argv:
    os.environ['CODEX_SELL_TARGET_TEST'] = '1'
    result = run_fixture(OUT / 'AuctionQtControl.exe', OUT, timeout=60)
else:
    result = subprocess.run([str(OUT / 'AuctionQtControl.exe')] + (["--prepare-retry"] if "--prepare-retry" in sys.argv else []), cwd=OUT, timeout=120, capture_output=True)
print(result.stdout.decode('utf-8', errors='replace'))
print(result.stderr.decode('utf-8', errors='replace'))
assert result.returncode == 0, result.returncode
