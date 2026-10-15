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
    flattened = make.replace('\\\n', ' ')
    return re.search(r'^' + name + r'\s*=\s*(.*)', flattened, re.M).group(1)

# The build directory can contain stale objects from PCsoftf. Link only objects
# belonging to the current branch's generated makefile.
objects = variable('OBJECTS').split()
objects = [x for x in objects if Path(x).name.lower() not in ('main.obj', 'clientcontroller.obj', 'mainwindow.obj')]
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
    assert all(size == (288, 108) for size in image_sizes[1:]), image_sizes
    print('PASS actual OCR uploads have correct client crop and 3x X/N dimensions:', image_sizes)
    print('PASS mock OCR requests use two CPU threads:', len(requests))
finally:
    server.shutdown()

# Exercise the actual boss loop with two remaining listings, including a failed
# quantity OCR after confirmation. All adb calls still target the isolated mock.
trace_path = OUT / 'dismount-trace.txt'
trace_path.write_text('')
timing_path = OUT / 'claim-timing.txt'
timing_path.write_text('')
flow_state = {'dismounts': 0, 'panels': 0, 'prices': 0, 'cleared': False, 'bad_sample': False}
marker_mode = False
recovery_mode = False
recovery_started = False
last_zero_ms = 0
return_state = {'market_started': False, 'second_depart': False, 'market_found': False}
class DismountOcr(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_POST(self):
        global last_zero_ms, recovery_started
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        png = base64.b64decode(request['base64'])
        size = struct.unpack('>II', png[16:24])
        text = {
            (737,440): '', (576,102): '仓库', (101,34): '转移全部',
            (126,36): '携带物资 装备配置', (431,41): '交易行', (57,142): '出售',
            (166,64): '上架物品', (215,47): '上架',
            (361,108): '领取完成', (190,66): '出发', (211,53): '方案',
            (126,40): '键配装 公', (288,108): '10543903'
        }.get(size)
        if size == (372,147):
            flow_state['panels'] += 1
            text = '使用售位：1/' if flow_state['panels'] == 1 else '使用售位：6/'
        elif size == (169,28):
            flow_state['prices'] += 1
            assert flow_state['prices'] <= 2 or flow_state['cleared'], 'relisting before empty'
            # A delayed phone stream first shows only a prefix. Re-read without retyping.
            text = {1: '10', 2: '100', 3: '8', 4: '80'}[flow_state['prices']]
        elif size == (187,41):
            flow_state['dismounts'] += 1
            text = '下架'
        elif size == (232,76):
            text = '等待上架' if marker_mode and flow_state['dismounts'] >= 2 else '下架'
            if text == '等待上架': flow_state['cleared'] = True
            if recovery_mode:
                recovery_started = True
                text = ''
        elif size == (309,87):
            if recovery_started:
                text = '上架数量0/15'
                last_zero_ms = time.time() * 1000
            elif flow_state['dismounts'] < 2:
                text = f"上架数量{2-flow_state['dismounts']}/7"
            elif not flow_state['bad_sample']:
                flow_state['bad_sample'] = True
                text = ''
            else:
                if not marker_mode: flow_state['cleared'] = True
                text = '上架数量8/15 +' if marker_mode and flow_state['prices'] <= 2 else '上架数量日/7'
                last_zero_ms = time.time() * 1000
        elif size == (361,108):
            taps = trace_path.read_text(encoding='utf-8').splitlines().count('shell input tap 516 79')
            text = '当前有1笔交易可领取' if taps < 2 else '领取完成'
        elif size == (190,66):
            if return_state['market_started']:
                return_state['second_depart'] = True
            text = '出发' if return_state['market_found'] else '整理'
        elif size == (231,54):
            return_state['market_started'] = True
            if return_state['second_depart']:
                return_state['market_found'] = True
                text = '交易行'
            else:
                text = ''
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
        MOCK_ADB_TRACE=str(trace_path),MOCK_ADB_TIMING=str(timing_path),MOCK_DISMOUNT_FLOW='1')
    result = subprocess.run([str(exe)],cwd=OUT,env=env,timeout=120,capture_output=True)
    print(result.stdout.decode(errors='replace'))
    logs = result.stderr.decode('utf-8',errors='replace')
    if result.returncode: print(logs)
    assert result.returncode == 0,result.returncode
    assert flow_state == {'dismounts':2,'panels':2,'prices':4,'cleared':True,'bad_sample':True},flow_state
    trace = trace_path.read_text(encoding='utf-8').splitlines()
    assert trace.count('shell input text 100') == 1 and trace.count('shell input text 80.00') == 1, 'partial OCR caused retyping'
    assert sum('KEYCODE_DEL' in line for line in trace) == 10, 'partial OCR caused extra clearing'
    print('PASS delayed price display 10->100 and 8->80: re-read without clearing or retyping')
    zero = next(i for i,s in enumerate(trace) if '上架数量日/7' in s)
    relist = trace.index('shell input tap 600 137')
    assert zero < relist,'relist click preceded zero quantity'
    for tap in ['shell input tap 445 204','shell input tap 785 529']:
        assert trace.count(tap) == 2,(tap,trace)
    assert trace.count('shell input tap 1132 399') == 3,'initial X=1 must retry once; X=6 must pass; relist opens once'
    assert '当前值 X=1' in logs and '当前值 X=6' in logs
    print('PASS expanded panel crop 124x49 at 3x; missing denominator 1/ stays blocked, 6/ continues without extra clicks')
    assert any('ocr (576, 102): 仓库' in s for s in trace),'warehouse upload must be enlarged 3x'
    assert '下架后仍有 1 件' in logs
    assert '跳过 X!=1 检查' in logs
    print('PASS two dismount cycles before relist; failed count retries; price 100->80; initial panel checked, relist panel opened without quantity gate')
    assert trace.count('shell input tap 516 79') == 2, 'claim timeout must retry click exactly once before success'
    first_claim_ms = int(next(s.split()[0] for s in timing_path.read_text().splitlines()
        if s.endswith('shell input tap 516 79')))
    assert first_claim_ms - last_zero_ms >= 490, (first_claim_ms,last_zero_ms)
    assert '将重新点击领取' in logs
    assert trace.count('shell input tap 29 34') == 2, 'only initial return and recognized market may trigger back'
    assert trace.count('shell input tap 67 119') == 1, 'return retry must not repeat claim confirmation'
    assert 'shell input tap 626 593' not in trace, 'old confirmation coordinate must not be used'
    assert trace.count('shell input tap 900 674') == 1
    assert '未找到交易行，不点击返回' in logs
    market = next(i for i,s in enumerate(trace) if 'ocr (231, 54): 交易行' in s)
    back = [i for i,s in enumerate(trace) if s == 'shell input tap 29 34'][-1]
    depart = trace.index('shell input tap 900 674')
    assert market < back < depart
    print('PASS depart timeout + missing market retries without click; recognized market returns once then departure completes')
    print('PASS zero quantity waits at least 500ms before claim; missed click retries after 5s OCR timeout; amount/ready reach server')

    marker_mode = True
    flow_state = {'dismounts': 0, 'panels': 0, 'prices': 0, 'cleared': False, 'bad_sample': False}
    return_state = {'market_started': False, 'second_depart': False, 'market_found': False}
    trace_path.write_text('')
    timing_path.write_text('')
    result = subprocess.run([str(exe)],cwd=OUT,env=env,timeout=120,capture_output=True)
    logs = result.stderr.decode('utf-8',errors='replace')
    if result.returncode: print(logs)
    assert result.returncode == 0,result.returncode
    trace = trace_path.read_text(encoding='utf-8').splitlines()
    assert flow_state['dismounts'] == 2 and flow_state['cleared'],flow_state
    assert trace.count('shell input tap 445 204') == 2,trace
    assert trace.count('shell input tap 785 529') == 2,trace
    assert any('上架数量8/15 +' in s for s in trace),trace
    assert trace.count('shell input text 80.00') == 1,trace
    assert trace.index('ocr (232, 76): 等待上架') < trace.index('shell input tap 600 137')
    assert '已识别到等待上架' in logs
    print('PASS waiting-to-list marker enters original discounted relist after two dismounts despite an 8/15 OCR sample')

    marker_mode = False
    recovery_mode = True
    flow_state = {'dismounts': 0, 'panels': 0, 'prices': 0, 'cleared': False, 'bad_sample': False}
    return_state = {'market_started': True, 'second_depart': True, 'market_found': True}
    trace_path.write_text('')
    result = subprocess.run([str(exe)],cwd=OUT,env=env,timeout=120,capture_output=True)
    logs = result.stderr.decode('utf-8',errors='replace')
    if result.returncode: print(logs)
    assert result.returncode == 0,result.returncode
    trace = trace_path.read_text(encoding='utf-8').splitlines()
    assert '回查上架数量' in logs
    assert flow_state['dismounts'] == 0 and flow_state['prices'] == 2,flow_state
    assert 'shell input tap 600 137' not in trace
    assert 'shell input tap 445 204' not in trace
    assert trace.count('shell input tap 516 79') == 2
    print('PASS first dismount timeout -> reread zero -> claim without discount or relisting naturally sold items')
finally:
    flow_server.shutdown()

# Both price-prompt entry points must report stable funds and end the round.
for mode, page in [(m, p) for m in ('initial', 'warehouse_retry') for p in ('none', 'loadout', 'equipment', 'both')] + [('missing_loadout', 'equipment')]:
    prompt_trace = OUT / f'prompt-{mode}-{page}-trace.txt'
    prompt_trace.write_text('')
    state = {'screens': 0, 'amounts': 0, 'warehouses': 0, 'page_reads': 0, 'loadouts': 0, 'nav_start': 0}
    class PromptOcr(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args): pass
        def do_POST(self):
            request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            size = struct.unpack('>II',base64.b64decode(request['base64'])[16:24])
            if size == (737,440):
                state['screens'] += 1
                prompt_screen = 2 if mode == 'warehouse_retry' else 1
                text = '确认实现方案' if state['screens'] == prompt_screen else ''
            elif size == (576,102):
                state['warehouses'] += 1
                text = ''
            elif size == (168,51):
                state['page_reads'] += 1
                if state['page_reads'] == 1: state['nav_start'] = time.monotonic()
                text = '一键配装' if page in ('loadout', 'both') and state['page_reads'] >= 2 else ''
            elif size == (282,54):
                text = '装备配置 保存方案' if page in ('equipment', 'both') and state['page_reads'] >= 2 else '保存方案 仓库物品'
            elif size == (126,40):
                state['loadouts'] += 1
                text = '' if mode == 'missing_loadout' else '键配装 公'
            elif size == (288,108):
                if page == 'none':
                    assert time.monotonic() - state['nav_start'] >= 4.9, 'fallback before 5s timeout'
                state['amounts'] += 1
                text = '哈夫币 35.247.070 可在交易行！'
            else:
                raise AssertionError(f'prompt branch continued listing: {size}')
            body=json.dumps({'code':100,'data':text}).encode()
            self.send_response(200)
            self.send_header('Content-Length',str(len(body)))
            self.end_headers()
            self.wfile.write(body)
    prompt_server=http.server.ThreadingHTTPServer(('127.0.0.1',0),PromptOcr)
    threading.Thread(target=prompt_server.serve_forever,daemon=True).start()
    try:
        env=dict(os.environ,MOCK_OCR_PORT=str(prompt_server.server_port),MOCK_ADB_DELAY='1',
            MOCK_ADB_TRACE=str(prompt_trace),MOCK_PROMPT_FLOW='1')
        if mode == 'missing_loadout': env['MOCK_PROMPT_FAILURE'] = '一键配装'
        result=subprocess.run([str(exe)],cwd=OUT,env=env,timeout=30,capture_output=True)
        if result.returncode: print(result.stderr.decode('utf-8',errors='replace'))
        assert result.returncode == 0,result.returncode
        trace=prompt_trace.read_text(encoding='utf-8').splitlines()
        if mode == 'missing_loadout':
            assert trace == ['shell input tap 802 50','shell input tap 790 665'],trace
            assert state['amounts'] == 0,state
            print('PASS equipment page matched but loadout timed out: no funds or ready report')
            continue
        expected = ['shell input tap 802 50']
        if page == 'equipment': expected += ['shell input tap 790 665', 'shell input tap 116 315']
        expected += ['shell input tap 1097 34']
        assert trace == expected,trace
        assert state['page_reads'] == 2 if page != 'none' else state['page_reads'] > 2,state
        assert state['loadouts'] == (1 if page == 'equipment' else 0),state
        assert state['amounts'] == 3,state
        assert (state['warehouses'] == 0) == (mode == 'initial'),state
        logs=result.stderr.decode('utf-8',errors='replace')
        assert 'adb_warehouse_retry' not in logs
        print(f'PASS {mode}, page={page}: page-specific navigation -> three stable amounts -> reports -> end')
    finally:
        prompt_server.shutdown()

# Cancellation during the 500ms retry delay must prevent the next device click.
cancel_trace=OUT / 'claim-cancel-trace.txt'
cancel_trace.write_text('')
class ClaimCancelOcr(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args): pass
    def do_POST(self):
        request=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        size=struct.unpack('>II',base64.b64decode(request['base64'])[16:24])
        text={
            (737,440): '', (576,102): '仓库', (101,34): '转移全部',
            (126,36): '携带物资 装备配置', (431,41): '交易行', (57,142): '出售',
            (166,64): '上架物品', (372,147): '使用售位：6/', (169,28): '100',
            (215,47): '上架', (309,87): '+ 上架数量日/15',
            (361,108): '领取完成' if return_cancel else '当前有1笔交易可领取',
            (190,66): '整理', (231,54): ''
        }[size]
        body=json.dumps({'code':100,'data':text}).encode()
        self.send_response(200)
        self.send_header('Content-Length',str(len(body)))
        self.end_headers()
        self.wfile.write(body)
cancel_server=http.server.ThreadingHTTPServer(('127.0.0.1',0),ClaimCancelOcr)
return_cancel = False
threading.Thread(target=cancel_server.serve_forever,daemon=True).start()
try:
    env=dict(os.environ,MOCK_OCR_PORT=str(cancel_server.server_port),MOCK_ADB_DELAY='1',
        MOCK_ADB_TRACE=str(cancel_trace),MOCK_DISMOUNT_FLOW='1',MOCK_CLAIM_CANCEL='1')
    result=subprocess.run([str(exe)],cwd=OUT,env=env,timeout=35,capture_output=True)
    if result.returncode: print(result.stderr.decode('utf-8',errors='replace'))
    assert result.returncode == 0,result.returncode
    trace=cancel_trace.read_text(encoding='utf-8').splitlines()
    assert trace.count('shell input tap 516 79') == 1,trace
    assert 'shell input tap 67 119' not in trace
    print('PASS claim retry cancelled during 500ms wait: no second claim tap or continuation')
    return_cancel = True
    cancel_trace.write_text('')
    env.pop('MOCK_CLAIM_CANCEL')
    env['MOCK_RETURN_CANCEL']='1'
    result=subprocess.run([str(exe)],cwd=OUT,env=env,timeout=45,capture_output=True)
    if result.returncode: print(result.stderr.decode('utf-8',errors='replace'))
    assert result.returncode == 0,result.returncode
    trace=cancel_trace.read_text(encoding='utf-8').splitlines()
    assert trace.count('shell input tap 29 34') == 1
    assert trace.count('shell input tap 67 119') == 1
    assert 'shell input tap 900 674' not in trace
    print('PASS prefixed plus and 日/15 reaches claim; departure recovery cancellation prevents further back/depart taps')
finally:
    cancel_server.shutdown()

