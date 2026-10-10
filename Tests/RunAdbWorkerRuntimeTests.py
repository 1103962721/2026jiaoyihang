"""Build/run against Release objects with mock adb and OCR, never a real device."""
from pathlib import Path
import http.server
import json
import os
import re
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/qt-release/DF_PlayClick_Client_Qt'
OUT = ROOT / 'build/adb-worker-runtime'
OUT.mkdir(parents=True, exist_ok=True)
make = (BUILD / 'Makefile.Release').read_text()
def variable(name):
    return re.search(r'^' + name + r'\s*=\s*(.*(?:\\\n.*)*)', make, re.M).group(1).replace('\\\n', ' ')

objects = ['release/' + p.name for p in (BUILD / 'release').glob('*.obj')]
objects = [x for x in objects if Path(x).name.lower() not in ('main.obj', 'clientcontroller.obj')]
libs = variable('LIBS')
includes = variable('INCPATH')
source = ROOT / 'Tests/AdbWorkerRuntimeTests.cpp'
obj = OUT / 'test.obj'
exe = OUT / 'AdbWorkerRuntimeTests.exe'
rsp = OUT / 'link.rsp'
rsp.write_text('/NOLOGO /SUBSYSTEM:CONSOLE /OUT:P:/build/adb-worker-runtime/AdbWorkerRuntimeTests.exe P:/build/adb-worker-runtime/test.obj ' + ' '.join(objects) + ' ' + libs)
bat = OUT / 'build-test.cmd'
bat.write_text('@echo off\ncall "E:\\VisualStudio\\VisualStudioIDE\\Common7\\Tools\\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul\n'
    + 'cd /d P:\\build\\qt-release\\DF_PlayClick_Client_Qt\n'
    + 'cl /nologo /c /std:c++17 /Zc:__cplusplus /permissive- /EHsc /MT /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DQT_STATIC /DQT_WIDGETS_LIB /DQT_GUI_LIB /DQT_CORE_LIB '
    + includes + ' /FoP:\\build\\adb-worker-runtime\\test.obj P:\\Tests\\AdbWorkerRuntimeTests.cpp\n'
    + 'if errorlevel 1 exit /b 1\nlink @P:\\build\\adb-worker-runtime\\link.rsp\n')
subprocess.run(['cmd', '/c', str(bat)], cwd=BUILD, check=True)

requests = []
class MockOcr(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        requests.append(request['options'])
        time.sleep(1)
        body = json.dumps({'code': 100, 'data': '12345'}).encode()
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), MockOcr)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    env = dict(os.environ, MOCK_OCR_PORT=str(server.server_port))
    result = subprocess.run([str(exe)], cwd=OUT, env=env, timeout=30, capture_output=True)
    print(result.stdout.decode(errors='replace'))
    print(result.stderr.decode(errors='replace'))
    assert result.returncode == 0, result.returncode
    assert len(requests) >= 4, len(requests)
    assert all(r['ocr.cpuThreads'] == 2 for r in requests)
    print('PASS mock OCR requests use two CPU threads:', len(requests))
finally:
    server.shutdown()

