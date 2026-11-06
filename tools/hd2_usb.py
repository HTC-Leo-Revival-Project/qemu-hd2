#!/usr/bin/env python3
"""Talk to a (real or emulated) HD2 over USB with pyusb/libusb only.

No ttyUSB0, no iPAC/usbser driver, no adb or fastboot binary: this is the
fallback for hosts where the kernel's serial driver for the HD2 is broken.

  hd2_usb.py list                      every interface of every candidate device
  hd2_usb.py fastboot getvar version   one fastboot command (cLK/HSPL fastboot, class ff/42/03)
  hd2_usb.py adb-shell [command]       a shell in a booted Android (class ff/42/01), no auth
  hd2_usb.py serial [-t SEC] [hex]     raw bulk endpoints of the first interface that has a pair
                                       (SPL/MAGLDR serial mode, WinMo iPAC): dumps what comes in

The device is found by class, not by VID/PID: HTC 0bb4:*, Google 18d1:0d02 (cLK
fastboot) and 18d1:4e*/0bb4:0c01 (adb) all work.  --vid/--pid pin one down.
Needs libusb access (udev rule, or run with sudo).
"""
import argparse
import struct
import sys
import time
import zlib

import usb.core
import usb.util

CANDIDATES = [(0x0BB4, None), (0x18D1, 0x0D02), (0x18D1, 0x4E11), (0x18D1, 0x4E12)]
TIMEOUT = 5000


def find(args):
    if args.vid:
        d = usb.core.find(idVendor=args.vid, idProduct=args.pid) if args.pid else usb.core.find(idVendor=args.vid)
        if d:
            return d
        sys.exit('no such device')
    for vid, pid in CANDIDATES:
        d = usb.core.find(idVendor=vid, idProduct=pid) if pid else usb.core.find(idVendor=vid)
        if d:
            return d
    sys.exit('no HD2 found (is it on the bus? lsusb)')


def interfaces(dev):
    for cfg in dev:
        for intf in cfg:
            eps = {'in': None, 'out': None}
            for ep in intf:
                if usb.util.endpoint_type(ep.bmAttributes) != usb.util.ENDPOINT_TYPE_BULK:
                    continue
                k = 'in' if usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_IN else 'out'
                eps[k] = eps[k] or ep
            yield intf, eps['in'], eps['out']


def pick(dev, cls, sub, proto):
    for intf, ein, eout in interfaces(dev):
        if (intf.bInterfaceClass, intf.bInterfaceSubClass, intf.bInterfaceProtocol) == (cls, sub, proto) and ein and eout:
            return intf, ein, eout
    return None


def claim(dev, intf):
    try:
        if dev.is_kernel_driver_active(intf.bInterfaceNumber):
            dev.detach_kernel_driver(intf.bInterfaceNumber)
    except (usb.core.USBError, NotImplementedError):
        pass
    usb.util.claim_interface(dev, intf.bInterfaceNumber)


def cmd_list(dev, args):
    try:
        name = usb.util.get_string(dev, dev.iProduct) if dev.iProduct else ''
    except (usb.core.USBError, ValueError):
        name = '(no string descriptors)'      # the emulated gadget has none for some modes
    print('%04x:%04x  %s' % (dev.idVendor, dev.idProduct, name))
    for intf, ein, eout in interfaces(dev):
        print('  if %d alt %d class %02x/%02x/%02x  in=%s out=%s' % (
            intf.bInterfaceNumber, intf.bAlternateSetting, intf.bInterfaceClass, intf.bInterfaceSubClass,
            intf.bInterfaceProtocol, hex(ein.bEndpointAddress) if ein else '-', hex(eout.bEndpointAddress) if eout else '-'))


def cmd_fastboot(dev, args):
    p = pick(dev, 0xFF, 0x42, 0x03)
    if not p:
        sys.exit('no fastboot interface (ff/42/03)')
    intf, ein, eout = p
    claim(dev, intf)
    w = args.words
    # the wire form is "getvar:product", "flash:boot", "erase:cache"; "oem ..." keeps its space
    cmd = w[0] + ':' + ' '.join(w[1:]) if w[0] in ('getvar', 'flash', 'erase', 'download', 'upload') and len(w) > 1 \
        else ' '.join(w)
    eout.write(cmd.encode(), TIMEOUT)
    while True:
        r = bytes(ein.read(4096, 30000))
        tag, text = r[:4].decode('latin1'), r[4:].decode('latin1', 'replace')
        print('%s %s' % (tag, text))
        if tag in ('OKAY', 'FAIL'):
            return 0 if tag == 'OKAY' else 1
        if tag == 'DATA':
            return 0


A_CNXN, A_OPEN, A_OKAY, A_CLSE, A_WRTE = 0x4E584E43, 0x4E45504F, 0x59414B4F, 0x45534C43, 0x45545257


def adb_send(eout, cmd, a0, a1, data=b''):
    # header and payload are two transfers: the guest's f_adb queues a 24-byte read for the
    # header and then one for the payload length, so one 24+N transfer would overflow the first
    eout.write(struct.pack('<6I', cmd, a0, a1, len(data), sum(data) & 0xFFFFFFFF, cmd ^ 0xFFFFFFFF), TIMEOUT)
    if data:
        eout.write(data, TIMEOUT)


def adb_recv(ein, timeout):
    h = bytes(ein.read(24, timeout))
    cmd, a0, a1, ln, _, _ = struct.unpack('<6I', h)
    data = bytes(ein.read(ln, timeout)) if ln else b''
    return cmd, a0, a1, data


def cmd_adb_shell(dev, args):
    p = pick(dev, 0xFF, 0x42, 0x01)
    if not p:
        sys.exit('no adb interface (ff/42/01): is Android up? (lsusb / hd2_usb.py list)')
    intf, ein, eout = p
    claim(dev, intf)
    adb_send(eout, A_CNXN, 0x01000000, 4096, b'host::features=shell_v2,cmd\0')
    cmd, _, _, data = adb_recv(ein, 10000)
    if cmd != A_CNXN:
        sys.exit('adb: expected CNXN, got %08x (an AUTH device needs the real adb)' % cmd)
    svc = 'shell:' + ' '.join(args.words) if args.words else 'shell:'
    adb_send(eout, A_OPEN, 1, 0, svc.encode() + b'\0')
    cmd, remote, _, _ = adb_recv(ein, 10000)
    if cmd != A_OKAY:
        sys.exit('adb: shell refused')
    if not args.words:
        import select, threading

        def reader():
            while True:
                try:
                    c, a0, a1, d = adb_recv(ein, 0)
                except usb.core.USBError:
                    return
                if c == A_WRTE:
                    sys.stdout.write(d.decode('utf8', 'replace'))
                    sys.stdout.flush()
                    adb_send(eout, A_OKAY, 1, remote)
                elif c == A_CLSE:
                    return
        threading.Thread(target=reader, daemon=True).start()
        for line in sys.stdin:
            adb_send(eout, A_WRTE, 1, remote, line.encode())
        return 0
    while True:
        c, a0, a1, d = adb_recv(ein, 60000)
        if c == A_WRTE:
            sys.stdout.write(d.decode('utf8', 'replace'))
            adb_send(eout, A_OKAY, 1, remote)
        elif c == A_CLSE:
            adb_send(eout, A_CLSE, 1, remote)
            return 0


def cmd_serial(dev, args):
    chosen = None
    for intf, ein, eout in interfaces(dev):
        if ein and eout:
            chosen = (intf, ein, eout)
            break
    if not chosen:
        sys.exit('no interface with a bulk IN/OUT pair')
    intf, ein, eout = chosen
    claim(dev, intf)
    if args.hex:
        eout.write(bytes.fromhex(args.hex), TIMEOUT)
    end = time.time() + args.t
    while time.time() < end:
        try:
            d = bytes(ein.read(512, 500))
        except usb.core.USBTimeoutError:
            continue
        sys.stdout.write(d.decode('latin1'))
        sys.stdout.flush()
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--vid', type=lambda x: int(x, 16))
    ap.add_argument('--pid', type=lambda x: int(x, 16))
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('list')
    f = sub.add_parser('fastboot'); f.add_argument('words', nargs=argparse.REMAINDER)
    a = sub.add_parser('adb-shell'); a.add_argument('words', nargs=argparse.REMAINDER)
    s = sub.add_parser('serial'); s.add_argument('-t', type=float, default=3); s.add_argument('hex', nargs='?')
    args = ap.parse_args()
    dev = find(args)
    sys.exit({'list': cmd_list, 'fastboot': cmd_fastboot, 'adb-shell': cmd_adb_shell, 'serial': cmd_serial}[args.cmd](dev, args) or 0)


if __name__ == '__main__':
    main()
