#!/usr/bin/env python3
"""Verify factory-card driver/firmware packaging, not hardware compatibility."""
import hashlib
import json
from pathlib import Path
import re
import sys


def validate_supplicant(target):
    """Inspect compiled CLI features without executing target-architecture code."""
    binary = (target / 'usr/sbin/wpa_supplicant').read_bytes()
    # The network worker requests -s. Its switch and help text are compiled
    # together under CONFIG_DEBUG_SYSLOG; unsupported -s exits before any
    # interface or control socket is created. Check the installed binary, so a
    # stale package after changing Kconfig cannot silently pass the image build.
    if b'  -s = log output to syslog instead of stdout\n' not in binary:
        raise ValueError('Installed wpa_supplicant lacks -s support; enable '
                         'BR2_PACKAGE_WPA_SUPPLICANT_DEBUG_SYSLOG and rebuild the package')
    # This diagnostic is only compiled by the Unix control-interface backend.
    # The client and wpa_cli both require that backend, independently of syslog.
    if b'ctrl_iface bind(PF_UNIX) failed' not in binary:
        raise ValueError('Installed wpa_supplicant lacks the Unix control interface')
    return {'syslog_option': True, 'unix_control_interface': True,
            'sha256': hashlib.sha256(binary).hexdigest()}


def validate(target, config):
    settings = dict(re.findall(r'^(CONFIG_\w+)=(.*)$', config.read_text(), re.M))
    required = {
        'CONFIG_B43': 'm', 'CONFIG_B43_SSB': 'y', 'CONFIG_B43_PHY_N': 'y',
        'CONFIG_B43_PHY_LP': 'y', 'CONFIG_SSB': 'm', 'CONFIG_SSB_PCIHOST': 'y',
        'CONFIG_SSB_B43_PCI_BRIDGE': 'y', 'CONFIG_RFKILL': 'y',
        'CONFIG_HP_WMI': 'm', 'CONFIG_FW_LOADER': 'y',
    }
    for name, value in required.items():
        if settings.get(name) != value:
            raise ValueError(f'{name} must be {value}')
    modules = target / 'lib/modules/6.12.109'
    aliases = (modules / 'modules.alias').read_text()
    for pci in ('4315', '432B'):
        if f'alias pci:v000014E4d0000{pci}sv*sd*bc*sc*i* ssb' not in aliases:
            raise ValueError(f'Missing Broadcom {pci} PCI autoload alias')
    for module in ('b43', 'ssb', 'mac80211', 'cfg80211', 'hp-wmi'):
        if not any(modules.rglob(f'{module}.ko*')):
            raise ValueError(f'Missing installed {module} module')
    firmware = {}
    for name in ('ucode15', 'lp0initvals15', 'lp0bsinitvals15',
                 'ucode16_mimo', 'n0initvals16', 'n0bsinitvals16'):
        path = target / 'lib/firmware/b43' / (name + '.fw')
        blob = path.read_bytes()
        # b43 firmware has an eight-byte header, version 1; microcode byte
        # count and initval-entry count use different header-size semantics.
        expected_type = ord('u') if name.startswith('ucode') else ord('i')
        if len(blob) <= 8 or blob[0] != expected_type or blob[1] != 1:
            raise ValueError(f'Invalid b43 firmware header: {path.name}')
        if name.startswith('ucode') and int.from_bytes(blob[4:8], 'big') != len(blob) - 8:
            raise ValueError(f'Truncated b43 microcode: {path.name}')
        firmware[path.name] = hashlib.sha256(blob).hexdigest()
    return {'status': 'packaging-verified-hardware-unqualified',
            'driver': 'b43', 'firmware_source': 'broadcom-wl-5.100.138',
            'firmware_sha256': firmware,
            'supplicant': validate_supplicant(target)}


if __name__ == '__main__':
    try:
        print(json.dumps(validate(Path(sys.argv[1]), Path(sys.argv[2])), indent=2))
    except (OSError, ValueError, IndexError) as error:
        sys.exit(f'HP2140 wireless packaging check failed: {error}')
