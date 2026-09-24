import ctypes
import socket
import struct
import threading
import time


def _udp_connreset_off(sock):
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

_MAGIC = b'\x63\x82\x53\x63'
_DISCOVER = 1
_OFFER = 2
_REQUEST = 3
_ACK = 5
_TYPES = {
    1: 'DISCOVER',
    2: 'OFFER',
    3: 'REQUEST',
    4: 'DECLINE',
    5: 'ACK',
    6: 'NAK',
    7: 'RELEASE',
    8: 'INFORM',
}


def _log(msg):
    print('[dhcp] ' + msg, flush=True)


def _type_name(msg_type):
    return _TYPES.get(msg_type, 'TYPE%d' % msg_type)


def _ip_to_int(ip):
    return struct.unpack('>I', socket.inet_aton(ip))[0]


def _int_to_ip(value):
    return socket.inet_ntoa(struct.pack('>I', value))


def _mask_from_prefix(prefix):
    if prefix == 0:
        return '0.0.0.0'
    return _int_to_ip((0xFFFFFFFF << (32 - prefix)) & 0xFFFFFFFF)


def _parse_options(data):
    opts = {}
    i = data.find(_MAGIC)
    if i < 0:
        return opts
    i += 4
    while i < len(data):
        code = data[i]
        if code == 255:
            break
        if code == 0:
            i += 1
            continue
        length = data[i + 1]
        opts[code] = data[i + 2:i + 2 + length]
        i += 2 + length
    return opts


def _mac_str(chaddr):
    return ':'.join('%02x' % b for b in chaddr[:6])


class DhcpServer(threading.Thread):
    def __init__(self, cfg, on_lease=None):
        super().__init__(name='dhcp', daemon=True)
        self._port = cfg['DHCP_PORT']
        self._server_ip = cfg['AP_IP']
        self._mask = _mask_from_prefix(cfg['PREFIX'])
        self._lease_time = cfg['LEASE_TIME']
        self._pool = list(range(_ip_to_int(cfg['DHCP_START']),
                                _ip_to_int(cfg['DHCP_END']) + 1))
        self._on_lease = on_lease
        self.leases = {}
        self._sock = None
        self._stop = threading.Event()

    def run(self):
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self._sock.bind(('', self._port))
        self._sock.settimeout(1.0)
        _udp_connreset_off(self._sock)
        _log('listening on 0.0.0.0:%d, pool %s..%s, gateway/dns %s'
             % (self._port, _int_to_ip(self._pool[0]), _int_to_ip(self._pool[-1]), self._server_ip))
        while not self._stop.is_set():
            try:
                data, addr = self._sock.recvfrom(1024)
            except socket.timeout:
                continue
            except ConnectionResetError:
                continue
            except OSError:
                if self._stop.is_set():
                    break
                continue
            self._handle(data, addr)

    def _allocate(self, mac):
        if mac in self.leases:
            return self.leases[mac]['ip']
        taken = {v['ip'] for v in self.leases.values()}
        for value in self._pool:
            ip = _int_to_ip(value)
            if ip not in taken:
                return ip
        return None

    def _client_note(self, mac, opts):
        host = opts.get(12, b'').split(b'\x00', 1)[0].decode('ascii', 'replace')
        vendor = opts.get(60, b'').split(b'\x00', 1)[0].decode('ascii', 'replace')
        bits = ['mac=%s' % mac]
        if host:
            bits.append('hostname=%r' % host)
        if vendor:
            bits.append('vendor=%r' % vendor)
        if 50 in opts and len(opts[50]) == 4:
            bits.append('requested=%s' % socket.inet_ntoa(opts[50]))
        return ' '.join(bits)

    def _handle(self, data, addr):
        src = '%s:%d' % (addr[0], addr[1])
        if len(data) < 240 or data[0] != 1:
            op = data[0] if data else '-'
            _log('%s ignored packet len=%d op=%s' % (src, len(data), op))
            return
        xid = data[4:8]
        chaddr = data[28:44]
        mac = _mac_str(chaddr)
        opts = _parse_options(data)
        msg_type = opts.get(53, b'\x00')[0]
        who = self._client_note(mac, opts)
        if msg_type == _DISCOVER:
            ip = self._allocate(mac)
            if not ip:
                _log('%s DISCOVER %s -> FAIL pool exhausted' % (src, who))
                return
            if self._reply(xid, chaddr, ip, _OFFER):
                _log('%s DISCOVER %s -> OFFER %s' % (src, who, ip))
        elif msg_type == _REQUEST:
            ip = self._allocate(mac)
            if not ip:
                _log('%s REQUEST %s -> FAIL pool exhausted' % (src, who))
                return
            self.leases[mac] = {'ip': ip, 'ts': time.time()}
            if self._reply(xid, chaddr, ip, _ACK):
                _log('%s REQUEST %s -> ACK %s (dns %s)' % (src, who, ip, self._server_ip))
            if self._on_lease:
                try:
                    self._on_lease(mac, ip)
                except Exception as exc:
                    _log('lease callback failed for %s: %s' % (mac, exc))
        else:
            _log('%s %s %s (no reply)' % (src, _type_name(msg_type), who))

    def _reply(self, xid, chaddr, your_ip, msg_type):
        pkt = struct.pack('>BBBB', 2, 1, 6, 0)
        pkt += xid
        pkt += struct.pack('>HH', 0, 0x8000)
        pkt += b'\x00\x00\x00\x00'
        pkt += socket.inet_aton(your_ip)
        pkt += socket.inet_aton(self._server_ip)
        pkt += b'\x00\x00\x00\x00'
        pkt += chaddr + b'\x00' * (16 - len(chaddr))
        pkt += b'\x00' * 192
        pkt += _MAGIC
        pkt += bytes([53, 1, msg_type])
        pkt += bytes([54, 4]) + socket.inet_aton(self._server_ip)
        pkt += bytes([51, 4]) + struct.pack('>I', self._lease_time)
        pkt += bytes([1, 4]) + socket.inet_aton(self._mask)
        pkt += bytes([3, 4]) + socket.inet_aton(self._server_ip)
        pkt += bytes([6, 4]) + socket.inet_aton(self._server_ip)
        pkt += bytes([255])
        try:
            self._sock.sendto(pkt, ('255.255.255.255', 68))
        except OSError as exc:
            _log('FAIL %s send to 255.255.255.255:68: %s' % (_type_name(msg_type), exc))
            return False
        return True

    def stop(self):
        self._stop.set()
        if self._sock:
            try:
                self._sock.close()
            except OSError:
                pass
