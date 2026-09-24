import ctypes
import socket
import struct
import threading


def _udp_connreset_off(sock):
    # Python's socket.ioctl rejects SIO_UDP_CONNRESET. Call Winsock directly.
    try:
        flag = ctypes.c_ulong(0)
        returned = ctypes.c_ulong(0)
        rc = ctypes.windll.ws2_32.WSAIoctl(
            sock.fileno(), 0x9800000C,
            ctypes.byref(flag), ctypes.sizeof(flag),
            None, 0, ctypes.byref(returned), None, None)
        if rc != 0:
            raise OSError(ctypes.get_last_error())
    except (OSError, AttributeError):
        pass

_TYPE_A = 1
_CLASS_IN = 1
_QUIET_DNS = ('pool.ntp.org', 'time.windows.com', 'time.nist.gov')


def _quiet_dns(name):
    return name.rstrip('.').lower().endswith(_QUIET_DNS)


def _parse_qname(data, offset):
    labels = []
    while True:
        length = data[offset]
        offset += 1
        if length == 0:
            break
        labels.append(data[offset:offset + length].decode('ascii', 'replace'))
        offset += length
    return '.'.join(labels), offset


def _build_response(query, qname_end, ip_bytes, answer):
    txid = query[0:2]
    question = query[12:qname_end + 4]
    if answer:
        flags = b'\x81\x80'
        ancount = 1
    else:
        flags = b'\x81\x83'
        ancount = 0
    header = txid + flags + struct.pack('>HHHH', 1, ancount, 0, 0)
    body = header + question
    if answer:
        body += b'\xc0\x0c'
        body += struct.pack('>HHIH', _TYPE_A, _CLASS_IN, 60, 4)
        body += ip_bytes
    return body


class DnsServer(threading.Thread):
    def __init__(self, cfg):
        super().__init__(name='dns', daemon=True)
        self._host = cfg['AP_IP']
        self._port = cfg['DNS_PORT']
        names = [cfg['OTA_HOST']]
        fw_host = cfg.get('FW_HOST', '').strip()
        if fw_host and fw_host.lower() not in (n.lower() for n in names):
            names.append(fw_host)
        self._spoof = tuple(n.rstrip('.').lower() for n in names)
        self._ip_bytes = socket.inet_aton(cfg['AP_IP'])
        self._sock = None
        self._stop = threading.Event()

    def run(self):
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind(('0.0.0.0', self._port))
        self._sock.settimeout(1.0)
        _udp_connreset_off(self._sock)
        print('[dns] listening on 0.0.0.0:%d, %s A -> %s'
              % (self._port, ', '.join(self._spoof), self._host), flush=True)
        while not self._stop.is_set():
            try:
                data, addr = self._sock.recvfrom(512)
            except socket.timeout:
                continue
            except ConnectionResetError:
                continue
            except OSError:
                if self._stop.is_set():
                    break
                continue
            self._handle(data, addr)

    def _handle(self, data, addr):
        if len(data) < 13:
            return
        try:
            qname, end = _parse_qname(data, 12)
            qtype = struct.unpack('>H', data[end:end + 2])[0]
        except (IndexError, struct.error):
            return
        name = qname.rstrip('.').lower()
        wants = name in self._spoof and qtype == _TYPE_A
        if wants:
            print('[dns] %s A %s -> %s' % (addr[0], qname, self._host), flush=True)
        elif not _quiet_dns(name):
            print('[dns] %s type %s %s -> NXDOMAIN' % (addr[0], qtype, qname), flush=True)
        reply = _build_response(data, end, self._ip_bytes, wants)
        try:
            self._sock.sendto(reply, addr)
        except OSError:
            pass

    def stop(self):
        self._stop.set()
        if self._sock:
            try:
                self._sock.close()
            except OSError:
                pass