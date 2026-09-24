import os

_DEFAULTS = {
    'HTTP_PORT': '80',
    'DNS_PORT': '53',
    'DHCP_PORT': '67',
    'LEASE_TIME': '3600',
    'PREFIX': '24',
    'FW_VERSION': 'V9.9.9',
    'FW_CONTENT': 'custom firmware',
    'FW_HOST': 'oss.ecowitt.net',
    'QUERY_INTVAL': '3600',
    'AUTO_TRIGGER': 'false',
    'CAMERA_WEB_PORT': '80',
}

_REQUIRED = ('SSID', 'PSK', 'AP_IP', 'OTA_HOST', 'FW_PATH')


def project_root():
    return os.path.dirname(os.path.abspath(__file__))


def load(path=None):
    if path is None:
        path = os.path.join(project_root(), 'settings.env')
    if not os.path.isfile(path):
        raise FileNotFoundError('settings.env not found: %s' % path)
    cfg = dict(_DEFAULTS)
    with open(path, 'r', encoding='utf-8') as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            key, sep, val = line.partition('=')
            if not sep:
                continue
            cfg[key.strip()] = val.strip()
    for key in _REQUIRED:
        if not cfg.get(key):
            raise ValueError('missing required setting: %s' % key)
    cfg['PREFIX'] = int(cfg['PREFIX'])
    cfg['LEASE_TIME'] = int(cfg['LEASE_TIME'])
    for key in ('HTTP_PORT', 'DNS_PORT', 'DHCP_PORT', 'CAMERA_WEB_PORT', 'QUERY_INTVAL'):
        cfg[key] = int(cfg[key])
    cfg['AUTO_TRIGGER'] = cfg['AUTO_TRIGGER'].strip().lower() in ('1', 'true', 'yes', 'on')
    cfg['FW_ABS'] = firmware_path(cfg)
    return cfg


def firmware_path(cfg):
    fw = cfg['FW_PATH']
    if not os.path.isabs(fw):
        fw = os.path.join(project_root(), fw)
    return os.path.normpath(fw)
