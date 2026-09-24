import socket
import struct
import threading

_PROTO_TCP = 6
_PROTO_UDP = 17
_HTTP_MARK = (b'GET ', b'POST ', b'HEAD ', b'PUT ')
_QUIET_DNS = ('pool.ntp.org', 'time.windows.com', 'time.nist.gov')


def _log(msg):
    print('[tap] ' + msg, flush=True)


def _ip(raw):
    return socket.inet_ntoa(raw)


def _in_subnet(ip, network, prefix):
    mask = (0xFFFFFFFF << (32 - prefix)) & 0xFFFFFFFF
    return (_unpack(ip) & mask) == (_unpack(network) & mask)


def _unpack(ip):
    return struct.unpack('>I', socket.inet_aton(ip))[0]


def _qname(data, offset):
    labels = []
    hops = 0
    while hops < 16 and offset < len(data):
        length = data[offset]
        if length == 0:
            return '.'.join(labels)
        if length & 0xC0:
            return '.'.join(labels) + '.?'
        offset += 1
        labels.append(data[offset:offset + length].decode('ascii', 'replace'))
        offset += length
        hops += 1
    return '.'.join(labels)


def _http_line(payload):
    if not payload.startswith(_HTTP_MARK):
        return None
    head, _, rest = payload.partition(b'\r\n')
    try:
        line = head.decode('ascii')
    except UnicodeError:
        return None
    host = ''
    for row in rest.split(b'\r\n'):
        if row.lower().startswith(b'host:'):
            host = row.split(b':', 1)[1].strip().decode('ascii', 'replace')
            break
    if host:
        return '%s Host=%s' % (line, host)
    return line


def describe(packet, ap_ip, prefix):
    if len(packet) < 20 or packet[0] >> 4 != 4:
        return None
    ihl = (packet[0] & 0x0F) * 4
    if len(packet) < ihl + 4:
        return None
    proto = packet[9]
    src = _ip(packet[12:16])
    dst = _ip(packet[16:20])
    if src == ap_ip or not _in_subnet(src, ap_ip, prefix):
        return None
    seg = packet[ihl:]
    sport, dport = struct.unpack('>HH', seg[:4])
    if proto == _PROTO_UDP and (sport == 53 or dport == 53) and sport not in (67, 68):
        dns = seg[8:]
        if len(dns) < 12:
            return None
        name = _qname(dns, 12)
        if name.rstrip('.').lower().endswith(_QUIET_DNS):
            return None
        return '%s -> %s UDP/%d DNS %s' % (src, dst, dport, name)
    if proto != _PROTO_TCP:
        return None
    flags = seg[13]
    offset = ((seg[12] >> 4) & 0x0F) * 4
    payload = seg[offset:]
    http = _http_line(payload)
    if http:
        return '%s -> %s TCP/%d %s' % (src, dst, dport, http)
    if flags & 0x02 and not flags & 0x10:
        return '%s -> %s TCP SYN port %d' % (src, dst, dport)
    return None


class TrafficTap(threading.Thread):
    def __init__(self, cfg):
        super().__init__(name='tap', daemon=True)
        self._ip = cfg['AP_IP']
        self._prefix = cfg['PREFIX']
        self._sock = None
        self._stop = threading.Event()

    def run(self):
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_IP)
            sock.bind((self._ip, 0))
            sock.ioctl(socket.SIO_RCVALL, socket.RCVALL_ON)
            sock.settimeout(1.0)
        except OSError as exc:
            _log('FAIL capture on %s needs an Administrator prompt: %s' % (self._ip, exc))
            return
        self._sock = sock
        _log('capturing every packet from %s/%d (DNS to any resolver, TCP to any host)'
             % (self._ip, self._prefix))
        while not self._stop.is_set():
            try:
                packet = sock.recvfrom(65535)[0]
            except socket.timeout:
                continue
            except OSError:
                break
            line = describe(packet, self._ip, self._prefix)
            if line:
                _log(line)

    def stop(self):
        self._stop.set()
        if self._sock:
            try:
                self._sock.ioctl(socket.SIO_RCVALL, socket.RCVALL_OFF)
                self._sock.close()
            except OSError:
                pass
