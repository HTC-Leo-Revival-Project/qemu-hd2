#!/usr/bin/env python3
"""Build a raw HTC HD2 NAND image with MAGLDR installed, as after flashing
magldr.nbh (MAGLDR 1.13) with HSPL: the NBH's OS section in the OS area
(block 0x212) and the factory WiFi/BT MAC addresses.  MAGLDR restarts the
radio from NAND before it boots an OS from the SD card, so add the radio:

  hd2_mkmagldr.py magldr.nbh -o nand-magldr.img
  hd2_radio.py RUU_signed.nbh --write-nand nand-magldr.img
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hd2_mknand as m  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('nbh', help='magldr.nbh')
    ap.add_argument('--wifi-mac', default='00:23:76:48:44:32')
    ap.add_argument('--bt-mac', default='00:23:76:48:44:33')
    ap.add_argument('-o', '--output', required=True)
    a = ap.parse_args()
    secs, start, count = m.extract_nbh_os(a.nbh)
    nand = m.Nand(a.output)
    m.write_factory_macs(nand, m.parse_mac(a.wifi_mac), m.parse_mac(a.bt_mac))
    for i, s in enumerate(secs):
        nand.write_page(m.ROM_BLOCK * m.PPB + i, s)
    nand.close()
    print('wrote %s: %d OS-area pages from %s' % (a.output, len(secs), a.nbh))


if __name__ == '__main__':
    main()
