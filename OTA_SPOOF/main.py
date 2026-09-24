import os
import sys
import time

if __package__ in (None, ''):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    __package__ = 'OTA_SPOOF'

from . import settings
from .dhcp import DhcpServer
from .dns import DnsServer
from .http_ota import HttpOtaServer
from .tap import TrafficTap


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else None
    cfg = settings.load(path)
    fw = cfg['FW_ABS']
    fw_host = cfg.get('FW_HOST') or 'oss.ecowitt.net'
    print('[core] AP_IP=%s  spoof=%s  offer=%s' % (cfg['AP_IP'], cfg['OTA_HOST'], cfg['FW_VERSION']),
          flush=True)
    print('[core] attach1file=https://%s/%s' % (fw_host, os.path.basename(fw)),
          flush=True)
    if os.path.isfile(fw):
        print('[core] firmware %s (%d bytes), served as /%s' % (fw, os.path.getsize(fw), os.path.basename(fw)),
              flush=True)
    else:
        print('[core] FAIL firmware missing: %s' % fw, flush=True)
    qi = cfg['QUERY_INTVAL']
    if 300 <= qi <= 86368:
        print('[core] queryintval=%d' % qi, flush=True)
    else:
        print('[core] WARN queryintval=%d is outside 300..86368; camera ignores it' % qi, flush=True)
    print('[core] camera version check is GET /api/ota/v1/version/info on %s port %d'
          % (cfg['OTA_HOST'], cfg['HTTP_PORT']), flush=True)

    services = [TrafficTap(cfg), DhcpServer(cfg), DnsServer(cfg), HttpOtaServer(cfg)]
    for svc in services:
        svc.start()

    print('[core] running. Press Ctrl+C to stop.')
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print('\n[core] stopping...')
    finally:
        for svc in reversed(services):
            svc.stop()
        for svc in services:
            svc.join(timeout=2)
    print('[core] stopped.')


if __name__ == '__main__':
    main()
