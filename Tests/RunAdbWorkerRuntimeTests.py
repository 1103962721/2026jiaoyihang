"""Build/run against Release objects with mock adb and OCR, never a real device."""
from pathlib import Path
import http.server
import json
import os
import re
import subprocess
import threading
import time
import base64
import struct

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
image_sizes = []
class MockOcr(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        requests.append(request['options'])
        png = base64.b64decode(request['base64'])
        image_sizes.append(struct.unpack('>II', png[16:24]))
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
    assert image_sizes[0] == (309, 87), image_sizes
    assert all(size == (110, 58) for size in image_sizes[1:]), image_sizes
    print('PASS actual OCR uploads have correct client crop and 3x X/N dimensions:', image_sizes)
    print('PASS mock OCR requests use two CPU threads:', len(requests))
finally:
    server.shutdown()

# Exercise the actual boss loop with two remaining listings, including a failed
# quantity OCR after confirmation. All adb calls still target the isolated mock.
trace_path = OUT / 'dismount-trace.txt'
trace_path.write_text('')
flow_state = {'dismounts': 0, 'panels': 0, 'prices': 0, 'cleared': False, 'bad_sample': False}
class DismountOcr(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        png = base64.b64decode(request['base64'])
        size = struct.unpack('>II', png[16:24])
        text = {
            (737,440): '', (576,102): '仓库', (101,34): '转移全部',
            (126,36): '携带物资 装备配置', (431,41): '交易行', (57,142): '出售',
            (166,64): '上架物品', (215,47): '上架', (232,76): '下架',
            (361,108): '领取完成', (190,66): '出发', (211,53): '方案',
            (126,40): '键配装 公', (110,58): '10543903'
        }.get(size)
        if size == (291,90):
            flow_state['panels'] += 1
            text = '使用售位：2' if flow_state['panels'] == 1 else '使用售位：1'
        elif size == (169,28):
            flow_state['prices'] += 1
            assert flow_state['prices'] == 1 or flow_state['cleared'], 'relisting before empty'
            text = '100' if flow_state['prices'] == 1 else '80'
        elif size == (187,41):
            flow_state['dismounts'] += 1
            text = '下架'
        elif size == (309,87):
            if flow_state['dismounts'] < 2:
                text = f"上架数量{2-flow_state['dismounts']}/7"
            elif not flow_state['bad_sample']:
                flow_state['bad_sample'] = True
                text = ''
            else:
                flow_state['cleared'] = True
                text = '上架数量日/7'
        assert text is not None, size
        with trace_path.open('a',encoding='utf-8') as trace:
            trace.write(f'ocr {size}: {text}\n')
        body = json.dumps({'code':100,'data':text}).encode()
        self.send_response(200)
        self.send_header('Content-Length',str(len(body)))
        self.end_headers()
        self.wfile.write(body)

flow_server = http.server.ThreadingHTTPServer(('127.0.0.1',0),DismountOcr)
threading.Thread(target=flow_server.serve_forever,daemon=True).start()
try:
    env = dict(os.environ,MOCK_OCR_PORT=str(flow_server.server_port),MOCK_ADB_DELAY='1',
        MOCK_ADB_TRACE=str(trace_path),MOCK_DISMOUNT_FLOW='1')
    result = subprocess.run([str(exe)],cwd=OUT,env=env,timeout=85,capture_output=True)
    print(result.stdout.decode(errors='replace'))
    logs = result.stderr.decode('utf-8',errors='replace')
    if result.returncode: print(logs)
    assert result.returncode == 0,result.returncode
    assert flow_state == {'dismounts':2,'panels':1,'prices':2,'cleared':True,'bad_sample':True},flow_state
    trace = trace_path.read_text(encoding='utf-8').splitlines()
    zero = next(i for i,s in enumerate(trace) if '上架数量日/7' in s)
    relist = trace.index('shell input tap 600 137')
    assert zero < relist,'relist click preceded zero quantity'
    for tap in ['shell input tap 445 204','shell input tap 785 529']:
        assert trace.count(tap) == 2,(tap,trace)
    assert trace.count('shell input tap 1132 399') == 2,'relist must still open price panel'
    assert any('ocr (576, 102): 仓库' in s for s in trace),'warehouse upload must be enlarged 3x'
    assert '下架后仍有 1 件' in logs
    assert '跳过 X!=1 检查' in logs
    print('PASS two dismount cycles before relist; failed count retries; price 100->80; initial panel checked, relist panel opened without quantity gate')
finally:
    flow_server.shutdown()

