# SPDX-License-Identifier: GPL-3.0-or-later
"""BossQuickDock Receiver: plays the PS5's live Opus audio on this PC.

The window is a local page with the look of the console's BossQuickDock page,
opened in Microsoft Edge app mode; it talks to this process on 127.0.0.1 only
(Host check + per-run token). The audio engine (receiver_core) and the console
client (console_control) are the ones from PC Receiver Control 1.2, unchanged.
"""
import argparse
import array
import collections
import ipaddress
import json
import os
import secrets
import socket
import subprocess
import sys
import threading
import time
import traceback
import urllib.request
import webbrowser
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
# The embeddable Python in python\ only searches its own folder: add ours.
sys.path[:0] = [str(HERE)] + ([str(HERE / 'deps')] if (HERE / 'deps').is_dir() else [])
from receiver_core import Receiver  # noqa: E402
from console_control import ConsoleClient, ConsoleError  # noqa: E402

VERSION = '1.0'
UI_PORT = 18099              # fixed: a second launch finds the running app
CONSOLE_HTTP = 8090
BUFFERS = (30, 50, 100, 150)
ASSETS = {'mini.jpg': 'image/jpeg', 'hero.jpg': 'image/jpeg', 'icon.png': 'image/png',
          'font.woff2': 'font/woff2'}
OLD_SETTINGS = HERE.parent / 'BossQuickDock-PC-Receiver-Control' / 'receiver-settings.json'
DATA = Path(os.environ.get('LOCALAPPDATA') or HERE) / 'BossQuickDock Receiver'
DEV = False                  # --dev: allows a console on 127.0.0.1 (mock tests)
BIND = '0.0.0.0'             # --dev listens on 127.0.0.1 only


def log(*parts):
    try:
        DATA.mkdir(parents=True, exist_ok=True)
        with (DATA / 'app.log').open('a', encoding='utf-8') as f:
            f.write(time.strftime('%Y-%m-%d %H:%M:%S ') + ' '.join(str(p) for p in parts) + '\n')
    except OSError:
        pass


def route_ip(target):
    """This PC's address on the route to target. Nothing is sent."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect((target, 9))
            return s.getsockname()[0]
    except OSError:
        return ''


def valid_ip(text):
    try:
        a = ipaddress.IPv4Address(str(text or '').strip())
    except ValueError:
        return ''
    if a.is_multicast or a.is_unspecified or (a.is_loopback and not DEV) or str(a).endswith('.255'):
        return ''
    return str(a)


def error_code(e):
    """Console errors come as English text; the page shows them by code."""
    if isinstance(e, ConsoleError) and e.status == 404:
        return 'old_version', ''
    text = str(e)
    for key, code in (('Console unavailable', 'ps5_unreachable'), ('recording is active', 'capture_busy'),
                      ('Another capture is active', 'capture_busy'), ('still disconnecting', 'bt_disconnecting'),
                      ('could not start', 'live_failed'), ('capture changed', 'live_failed')):
        if key in text:
            return code, text
    return 'error', text


class App:
    def __init__(self):
        self.lock = threading.RLock()
        self.settings = dict(ps5='', port=18098, buffer_ms=50, output='auto', lang='')
        self.load_settings()
        self.receiver = None
        self.audio = None
        self.audio_error = ''
        self.audio_retry_at = 0
        self.audio_retry_delay = 1
        self.audio_stats = collections.Counter()
        self.pcm = collections.deque()
        self.pcm_lock = threading.Lock()
        self.audible = False
        self.levels = collections.deque(maxlen=10)
        self.listen_since = 0
        self.rx_count = 0
        self.rx_at = 0
        self.busy = ''
        self.msg = dict(code='', detail='', kind='', n=0)
        self.console = {}
        self.console_ps5 = ''
        self.polling = False
        self.next_poll = 0
        self.last_request = time.monotonic()
        self.bye_at = 0
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    # ---------- settings ----------
    def load_settings(self):
        path = DATA / 'settings.json'
        try:
            saved = json.loads(path.read_text(encoding='utf-8'))
        except (OSError, ValueError):
            saved = {}
            try:   # first run on a PC that had PC Receiver Control: keep its PS5 address
                old = json.loads(OLD_SETTINGS.read_text(encoding='utf-8'))
                saved = dict(ps5=old.get('source', ''), port=old.get('port', 18098))
            except (OSError, ValueError, AttributeError):
                pass
        if isinstance(saved, dict):
            self.apply_settings(saved, save=False)

    def apply_settings(self, new, save=True):
        with self.lock:
            s = dict(self.settings)
            if 'ps5' in new:
                s['ps5'] = valid_ip(new['ps5'])
            if 'port' in new:
                try:
                    port = int(new['port'])
                except (TypeError, ValueError):
                    port = 0
                if 1024 <= port <= 65535 and port != UI_PORT:
                    s['port'] = port
            if new.get('buffer_ms') in BUFFERS:
                s['buffer_ms'] = new['buffer_ms']
            if isinstance(new.get('output'), str) and new['output']:
                s['output'] = new['output'][:200]
            if new.get('lang') in ('', 'pt', 'en'):
                s['lang'] = new['lang']
            changed = {k for k in s if s[k] != self.settings[k]}
            self.settings = s
            if 'ps5' in changed:
                self.console = {}
                self.next_poll = 0
        if save:
            try:
                DATA.mkdir(parents=True, exist_ok=True)
                tmp = DATA / 'settings.tmp'
                tmp.write_text(json.dumps(self.settings, indent=2), encoding='utf-8')
                tmp.replace(DATA / 'settings.json')
            except OSError as e:
                log('settings', e)
        return changed

    def set_settings(self, body):
        changed = self.apply_settings(body)
        if self.receiver and changed & {'output', 'buffer_ms', 'port'}:
            if changed & {'buffer_ms', 'port'}:
                self.restart_receiver()      # keeps the console stream; ~0.3 s gap
            else:
                try:
                    self.open_audio()
                except Exception as e:
                    self.audio_error = str(e)
                    self.say('audio_failed', str(e), 'bad')
        return 200, {'ok': True, 'settings': self.settings}

    # ---------- audio out ----------
    def outputs(self):
        try:
            import sounddevice as sd
            apis = sd.query_hostapis()
            devs = [(d['name'], apis[d['hostapi']]['name']) for d in sd.query_devices() if d['max_output_channels'] >= 2]
        except Exception as e:
            log('outputs', e)
            return {'outputs': [], 'error': str(e)}
        wasapi = [d for d in devs if d[1] == 'Windows WASAPI']
        return {'outputs': [dict(id=n + ' [' + a + ']', name=n) for n, a in (wasapi or devs)]}

    def output_candidates(self):
        import sounddevice as sd
        apis = sd.query_hostapis()
        choice = self.settings['output']
        if choice != 'auto':
            for i, d in enumerate(sd.query_devices()):
                if d['max_output_channels'] >= 2 and d['name'] + ' [' + apis[d['hostapi']]['name'] + ']' == choice:
                    return [i]
        # Auto (or a device that is gone): the Windows default through WASAPI,
        # then the plain default output, which resamples if it has to.
        cands = [a['default_output_device'] for a in apis
                 if a['name'] == 'Windows WASAPI' and a['default_output_device'] >= 0]
        return cands + [None]

    def open_audio(self):
        import sounddevice as sd
        self.close_audio()
        last = None
        for index in self.output_candidates():
            stream = None
            try:
                stream = sd.RawOutputStream(samplerate=48000, channels=2, dtype='int16', blocksize=480,
                                            latency='low', device=index, callback=self.callback)
                stream.start()
            except Exception as e:
                last = e
                if stream is not None:
                    try:
                        stream.close()
                    except Exception:
                        pass
                continue
            self.clear_pcm()
            self.audio = stream
            self.audio_error = ''
            self.audio_retry_delay = 1
            return
        raise last or RuntimeError('No audio output')

    def close_audio(self):
        if self.audio:
            try:
                self.audio.stop()
                self.audio.close()
            except Exception:
                pass
            self.audio = None

    def clear_pcm(self):
        with self.pcm_lock:
            self.pcm.clear()
            self.audible = False

    def push(self, pcm):
        a = array.array('h')
        a.frombytes(pcm)
        peak = max(max(a), -min(a)) / 32768 if a else 0.0
        with self.pcm_lock:
            if len(self.pcm) >= 4:
                self.pcm.popleft()
                self.audio_stats['queue_trims'] += 1
            self.pcm.append(pcm)
            self.levels.append(peak)

    def callback(self, outdata, frames, timing, status):
        if status.output_underflow:
            self.audio_stats['device_underflows'] += 1
        with self.pcm_lock:
            if self.pcm:
                pcm = self.pcm.popleft()
                self.audible = True
            else:
                pcm = bytes(frames * 4)
                self.audio_stats['silence_callbacks'] += int(self.audible)
        outdata[:] = pcm if len(pcm) == frames * 4 else bytes(frames * 4)

    # ---------- receiver ----------
    def start_listening(self):
        s = self.settings
        self.audio_stats = collections.Counter()
        self.open_audio()
        r = Receiver(BIND, s['port'], s['ps5'], s['buffer_ms'], self.push, self.clear_pcm)
        r.start()
        self.receiver = r
        self.listen_since = time.monotonic()
        self.rx_count = 0
        self.rx_at = 0

    def stop_listening(self):
        r, self.receiver = self.receiver, None
        if r:
            r.stop()
            self.last_snapshot = r.snapshot()
        self.close_audio()
        self.clear_pcm()
        with self.pcm_lock:
            self.levels.clear()

    def restart_receiver(self):
        s = self.settings
        if self.receiver:
            self.receiver.stop()
        self.clear_pcm()
        self.receiver = Receiver(BIND, s['port'], s['ps5'], s['buffer_ms'], self.push, self.clear_pcm)
        self.receiver.start()

    # ---------- console status ----------
    def poll_soon(self):
        self.next_poll = 0

    def poll(self, ps5):
        info = dict(reachable=False, native=None, version='', state='', device='', live=False,
                    live_here=False, live_dest='', recording=False, code='')
        try:
            c = ConsoleClient(ps5, CONSOLE_HTTP, timeout=2)
            st = c.status()
            info.update(reachable=True, version=st.get('version') or '', state=st.get('state') or '',
                        device=st.get('device') or '', paused=bool(st.get('paused')))
            try:
                n = c.native()
            except ConsoleError as e:
                if e.status != 404:
                    raise
                info['native'] = False
            else:
                live = bool(n.get('active')) and n.get('mode') == 'live'
                info.update(native=True, live=live, recording=bool(n.get('active')) and not live,
                            live_dest=str(n.get('destination') or '') if live else '',
                            live_here=live and n.get('destination') == route_ip(ps5) and n.get('port') == self.settings['port'])
        except ConsoleError as e:
            info['code'] = error_code(e)[0]
        except Exception as e:
            info['code'] = 'error'
            log('poll', e)
        finally:
            with self.lock:
                if self.settings['ps5'] == ps5:
                    self.console = info
                    self.console_ps5 = ps5
                self.polling = False

    # ---------- actions ----------
    def say(self, code, detail='', kind='info'):
        with self.lock:
            self.msg = dict(code=code, detail=str(detail)[:300], kind=kind, n=self.msg['n'] + 1)

    def act(self, name, body):
        if name not in ('listen', 'stop', 'bluetooth', 'find'):
            return 404, {'error': 'unknown action'}
        with self.lock:
            if self.busy:
                return 409, {'error': 'busy'}
            self.busy = name
        threading.Thread(target=self.run_action, args=(name, body), daemon=True).start()
        return 200, {'ok': True}

    def run_action(self, name, body):
        try:
            getattr(self, 'do_' + name)(body)
        except ConsoleError as e:
            code, detail = error_code(e)
            self.say(code, detail, 'bad')
        except Exception as e:
            log('action', name, traceback.format_exc())
            self.say('error', str(e), 'bad')
        finally:
            with self.lock:
                self.busy = ''
            self.poll_soon()

    def do_listen(self, body):
        if 'ps5' in body:
            self.apply_settings({'ps5': body['ps5']})
        ps5 = self.settings['ps5']
        if not ps5:
            return self.say('need_ip', '', 'bad')
        pc = route_ip(ps5)
        if not pc:
            return self.say('no_network', '', 'bad')
        client = ConsoleClient(ps5, CONSOLE_HTTP)
        client.status()                      # unreachable PS5 -> ps5_unreachable
        client.native()                      # older BossQuickDock -> 404 -> old_version
        r = self.receiver
        if r and (r.source != ps5 or r.port != self.settings['port']):
            self.stop_listening()
        if not self.receiver:
            try:
                self.start_listening()
            except Exception as e:
                log('audio', traceback.format_exc())
                self.stop_listening()
                return self.say('audio_failed', str(e), 'bad')
        end = time.monotonic() + 2
        while not self.receiver.listening:
            if time.monotonic() > end:
                self.stop_listening()
                return self.say('udp_busy', '', 'bad')
            time.sleep(.05)
        try:
            res = client.start_live(pc, self.settings['port'])
        except Exception:
            self.stop_listening()
            raise
        self.say('already' if res.get('already_active') else 'waiting_audio' if res.get('awaiting_data') else 'started', '', 'good')

    def do_stop(self, body):
        ps5 = self.settings['ps5']
        if ps5 and self.console.get('live_here', True):
            try:
                ConsoleClient(ps5, CONSOLE_HTTP).stop_live()
            except ConsoleError as e:      # PS5 gone or nothing to stop: still stop here
                log('stop', e)
        self.stop_listening()
        self.say('stopped', '', 'good')

    def do_bluetooth(self, body):
        ps5 = self.settings['ps5']
        if not ps5:
            return self.say('need_ip', '', 'bad')
        res = ConsoleClient(ps5, CONSOLE_HTTP).return_bluetooth()
        self.stop_listening()
        self.say('bt_connected' if res.get('connected') else 'bt_requested', '', 'good')

    def do_find(self, body):
        mine = route_ip('8.8.8.8') or route_ip(self.settings['ps5'] or '192.168.1.1')
        if not mine or mine.startswith('127.'):
            return self.say('no_network', '', 'bad')
        net = ipaddress.IPv4Network(mine + '/24', strict=False)
        hosts = [str(h) for h in net.hosts() if str(h) != mine]
        if self.settings['ps5'] in hosts:   # check the saved address first
            hosts.remove(self.settings['ps5'])
            hosts.insert(0, self.settings['ps5'])

        def probe(ip):
            try:
                with self.opener.open('http://%s:%d/api/status' % (ip, CONSOLE_HTTP), timeout=.8) as r:
                    j = json.loads(r.read(65536))
            except Exception:
                return None
            if isinstance(j, dict) and 'version' in j and 'state' in j:
                return ip, str(j.get('version') or '')
            return None

        with ThreadPoolExecutor(64) as ex:
            found = [f for f in ex.map(probe, hosts) if f]
        if not found:
            return self.say('not_found', str(net), 'bad')
        ip, ver = found[0]
        self.apply_settings({'ps5': ip})
        self.say('found', ip + (' · BossQuickDock ' + ver if ver else ''), 'good')

    # ---------- state for the page ----------
    def touch(self):
        self.last_request = time.monotonic()

    def maintain(self):
        now = time.monotonic()
        r = self.receiver
        if r and not self.busy:
            try:
                ok = self.audio is not None and self.audio.active
            except Exception:
                ok = False
            if not ok and now >= self.audio_retry_at:
                try:
                    self.open_audio()
                    self.audio_stats['output_restarts'] += 1
                except Exception as e:
                    self.audio_error = str(e)
                    self.audio_retry_at = now + self.audio_retry_delay
                    self.audio_retry_delay = min(5, self.audio_retry_delay * 2)
            snap = r.snapshot()
            if snap.get('received', 0) != self.rx_count:
                self.rx_count = snap.get('received', 0)
                self.rx_at = now
        with self.lock:
            ps5 = self.settings['ps5']
            go = ps5 and not self.polling and now >= self.next_poll
            if go:
                self.polling = True
                self.next_poll = now + 3
        if go:
            threading.Thread(target=self.poll, args=(ps5,), daemon=True).start()

    def state(self):
        now = time.monotonic()
        r = self.receiver
        snap = r.snapshot() if r else {}
        with self.pcm_lock:
            level = max(self.levels) if self.levels else 0.0
            pcm_ms = len(self.pcm) * 10
        receiving = bool(r) and self.rx_at and now - self.rx_at < 1.0
        if self.busy:
            phase = {'listen': 'connecting', 'stop': 'stopping', 'bluetooth': 'bt', 'find': 'searching'}[self.busy]
        elif r and receiving and snap.get('playing'):
            phase = 'playing'
        elif r:
            phase = 'waiting'
        else:
            phase = 'idle'
        try:
            dev_ms = round(self.audio.latency * 1000) if self.audio else 0
        except Exception:
            dev_ms = 0
        with self.lock:
            return dict(
                version=VERSION, settings=self.settings, phase=phase, busy=self.busy, msg=self.msg,
                pc_ip=route_ip(self.settings['ps5']) if self.settings['ps5'] else route_ip('8.8.8.8'),
                listening=bool(r), level=round(level, 3), audio_error=self.audio_error,
                console=self.console if self.console_ps5 == self.settings['ps5'] else {},
                stats=dict(
                    pc_ms=(self.settings['buffer_ms'] + pcm_ms + dev_ms) if r else 0,
                    received=snap.get('received', 0), decoded=snap.get('decoded', 0),
                    lost=snap.get('missing', 0) + snap.get('late', 0),
                    invalid=snap.get('invalid', 0), decode_errors=snap.get('decode_errors', 0),
                    rebuffers=sum(snap.get(k, 0) for k in ('gap_rebuffers', 'late_rebuffers', 'underrun_rebuffers',
                                                           'overflow_rebuffers', 'stalls')),
                    underflows=self.audio_stats.get('device_underflows', 0),
                    uptime_s=int(now - self.listen_since) if r else 0))

    def report(self):
        r = self.receiver
        return dict(app='BossQuickDock Receiver ' + VERSION, time=time.strftime('%Y-%m-%d %H:%M:%S'),
                    settings=self.settings, console=self.console,
                    receiver=r.snapshot() if r else getattr(self, 'last_snapshot', {}),
                    audio=dict(self.audio_stats), audio_error=self.audio_error, end_to_end_latency_measured=False)

    def shutdown(self):
        # Closing the window stops the PS5 sending to this PC; Bluetooth is left as it is.
        if self.receiver and self.console.get('live_here'):
            try:
                ConsoleClient(self.settings['ps5'], CONSOLE_HTTP, timeout=1.5).stop_live(timeout=3)
            except Exception as e:
                log('shutdown', e)
        self.stop_listening()


APP = None
TOKEN = secrets.token_hex(16)


class Handler(BaseHTTPRequestHandler):
    server_version = 'BQDReceiver'

    def log_message(self, *args):
        pass

    def send(self, code, body, ctype='application/json; charset=utf-8', extra=None):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('Cache-Control', 'no-store' if not ctype.startswith(('image', 'font')) else 'max-age=86400')
        if ctype.startswith('text/html'):
            self.send_header('Content-Security-Policy', "default-src 'self'; img-src 'self' data:; style-src 'self' "
                             "'unsafe-inline'; script-src 'self' 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def host_ok(self):
        return self.headers.get('Host', '') in ('127.0.0.1:%d' % UI_PORT, 'localhost:%d' % UI_PORT)

    def token_ok(self, given):
        return isinstance(given, str) and secrets.compare_digest(given, TOKEN)

    def do_GET(self):
        if not self.host_ok():
            return self.send(403, {'error': 'host'})
        path = self.path.split('?', 1)[0]
        if path == '/':
            page = (HERE / 'web' / 'index.html').read_text(encoding='utf-8')
            return self.send(200, page.replace('{{TOKEN}}', TOKEN).replace('{{VERSION}}', VERSION), 'text/html; charset=utf-8')
        if path.startswith('/a/') and path[3:] in ASSETS:
            return self.send(200, (HERE / 'web' / 'assets' / path[3:]).read_bytes(), ASSETS[path[3:]])
        if path.startswith('/api/'):
            if not self.token_ok(self.headers.get('X-App-Token')):
                return self.send(403, {'error': 'token'})
            APP.touch()
            if path == '/api/state':
                return self.send(200, APP.state())
            if path == '/api/outputs':
                return self.send(200, APP.outputs())
            if path == '/api/report':
                return self.send(200, json.dumps(APP.report(), indent=2))
        return self.send(404, {'error': 'not found'})

    def do_POST(self):
        if not self.host_ok():
            return self.send(403, {'error': 'host'})
        try:
            n = int(self.headers.get('Content-Length') or 0)
            body = json.loads(self.rfile.read(n) or b'{}') if 0 <= n <= 65536 else None
        except (ValueError, UnicodeError):
            body = None
        if not isinstance(body, dict):
            return self.send(400, {'error': 'body'})
        path = self.path.split('?', 1)[0]
        if not self.token_ok(self.headers.get('X-App-Token') or body.get('token')):
            return self.send(403, {'error': 'token'})
        if path == '/api/bye':          # page closed or reloaded (sendBeacon)
            APP.bye_at = time.monotonic()
            return self.send(200, {'ok': True})
        APP.touch()
        if path == '/api/settings':
            return self.send(*APP.set_settings(body))
        if path.startswith('/api/action/'):
            return self.send(*APP.act(path[len('/api/action/'):], body))
        return self.send(404, {'error': 'not found'})


def find_edge():
    for base in (os.environ.get('ProgramFiles(x86)'), os.environ.get('ProgramFiles'), os.environ.get('LOCALAPPDATA')):
        if base:
            p = Path(base) / 'Microsoft' / 'Edge' / 'Application' / 'msedge.exe'
            if p.is_file():
                return p
    return None


def open_window(url):
    """Edge app mode in its own profile: a plain window, and the process ends
    when the window closes. Otherwise the default browser."""
    edge = find_edge()
    if edge:
        try:
            return subprocess.Popen([str(edge), '--app=' + url, '--user-data-dir=' + str(DATA / 'window'),
                                     '--no-first-run', '--no-default-browser-check', '--window-size=1320,860',
                                     '--disable-features=Translate'])
        except OSError as e:
            log('edge', e)
    webbrowser.open(url)
    return None


class Server(ThreadingHTTPServer):
    # Windows lets a second socket bind a port that has SO_REUSEADDR, so a
    # second copy would share it with the first. Exclusive: the bind fails
    # and the second copy only shows the running one.
    allow_reuse_address = False
    daemon_threads = True

    def server_bind(self):
        if hasattr(socket, 'SO_EXCLUSIVEADDRUSE'):
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        super().server_bind()


def main():
    global APP, DEV, DATA, BIND, UI_PORT
    p = argparse.ArgumentParser()
    p.add_argument('--no-window', action='store_true', help='do not open a window (tests)')
    p.add_argument('--dev', action='store_true', help='allow a console on 127.0.0.1 (mock tests)')
    p.add_argument('--data', help='settings folder (tests)')
    p.add_argument('--ui-port', type=int, help='page port (tests, so they never meet the real app)')
    a = p.parse_args()
    DEV = a.dev
    if DEV:
        BIND = '127.0.0.1'
    if a.data:
        DATA = Path(a.data)
    if a.ui_port:
        UI_PORT = a.ui_port
    url = 'http://127.0.0.1:%d/' % UI_PORT
    try:
        server = Server(('127.0.0.1', UI_PORT), Handler)
    except OSError:                          # already running: just show it
        if not a.no_window:
            open_window(url)
        return
    APP = App()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    proc = None if a.no_window else open_window(url)
    log('start', VERSION, 'window' if proc else 'browser' if not a.no_window else 'none')
    try:
        while True:
            time.sleep(.25)
            APP.maintain()
            now = time.monotonic()
            idle = now - APP.last_request
            if APP.bye_at and now - APP.bye_at > 4 and APP.last_request < APP.bye_at:
                break                        # page closed and not reloaded
            if proc is not None and proc.poll() is not None and idle > 15:
                break                        # window gone
            if not a.no_window and idle > 900:
                break                        # nobody looking for 15 minutes
    except KeyboardInterrupt:
        pass
    finally:
        APP.shutdown()
        server.shutdown()
        log('exit')


if __name__ == '__main__':
    main()
