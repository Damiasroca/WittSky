import http.server
import json
import os
import socketserver
import threading
from urllib.parse import parse_qs
from urllib.parse import urlsplit


def _clean_path(raw):
    for scheme in ('http://', 'https://'):
        if raw.startswith(scheme):
            rest = raw[len(scheme):]
            slash = rest.find('/')
            return rest[slash:] if slash >= 0 else '/'
    return raw


class _Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def log_error(self, fmt, *args):
        print('[http] bad request from %s: %s'
              % (self.client_address[0], fmt % args))

    def handle(self):
        print('[http] connection from %s' % (self.client_address[0],), flush=True)
        super().handle()

    @property
    def _cfg(self):
        return self.server.cfg

    def _path_only(self):
        return _clean_path(self.path).split('?', 1)[0]

    def _fw_name(self):
        return os.path.basename(self._cfg['FW_ABS'])

    def _is_version(self, p):
        low = p.lower()
        return 'version' in low and 'info' in low

    def _is_firmware(self, p):
        return p.lstrip('/') == self._fw_name()

    def _firmware_url(self):
        cfg = self._cfg
        host = cfg.get('FW_HOST') or 'oss.ecowitt.net'
        return 'https://%s/%s' % (host, self._fw_name())

    def _send_version(self):
        cfg = self._cfg
        when = (parse_qs(urlsplit(self.path).query).get('time') or [''])[0]
        url = self._firmware_url()
        payload = {
            'code': 0,
            'msg': 'Success',
            'time': when,
            'data': {
                'id': 1,
                'name': cfg['FW_VERSION'],
                'content': cfg['FW_CONTENT'],
                'attach1file': url,
                'attach2file': '',
                'queryintval': cfg['QUERY_INTVAL'],
            },
        }
        body = json.dumps(payload).encode('utf-8')
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(body)
        print('[http] %s version/info -> %s (%s)'
              % (self.client_address[0], cfg['FW_VERSION'], self.path), flush=True)

    def _send_firmware(self, head_only):
        fw = self._cfg['FW_ABS']
        if not os.path.isfile(fw):
            self.send_error(404, 'firmware missing')
            print('[http] firmware requested but missing: %s' % fw)
            return
        size = os.path.getsize(fw)
        self.send_response(200)
        self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Content-Length', str(size))
        self.send_header('Connection', 'close')
        self.end_headers()
        print('[http] %s %s firmware (%d bytes)'
              % (self.client_address[0], 'HEAD' if head_only else 'GET', size))
        if head_only:
            return
        with open(fw, 'rb') as fh:
            while True:
                chunk = fh.read(8192)
                if not chunk:
                    break
                self.wfile.write(chunk)

    def do_GET(self):
        p = self._path_only()
        print('[http] %s GET %s' % (self.client_address[0], self.path), flush=True)
        if self._is_version(p):
            self._send_version()
        elif self._is_firmware(p):
            self._send_firmware(False)
        else:
            self.send_error(404)

    def do_HEAD(self):
        p = self._path_only()
        print('[http] %s HEAD %s' % (self.client_address[0], self.path), flush=True)
        if self._is_firmware(p):
            self._send_firmware(True)
        else:
            self.send_error(404)


class _Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


class HttpOtaServer(threading.Thread):
    def __init__(self, cfg):
        super().__init__(name='http', daemon=True)
        self._cfg = cfg
        self._httpd = None

    def run(self):
        addr = ('0.0.0.0', self._cfg['HTTP_PORT'])
        self._httpd = _Server(addr, _Handler)
        self._httpd.cfg = self._cfg
        print('[http] listening on 0.0.0.0:%d, serving /%s'
              % (self._cfg['HTTP_PORT'], self._fw_name()), flush=True)
        self._httpd.serve_forever()

    def _fw_name(self):
        return os.path.basename(self._cfg['FW_ABS'])

    def stop(self):
        if self._httpd:
            try:
                self._httpd.shutdown()
                self._httpd.server_close()
            except OSError:
                pass
