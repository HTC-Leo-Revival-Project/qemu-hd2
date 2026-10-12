#!/usr/bin/env python3
"""Write the factory WiFi/Bluetooth MAC addresses into an existing HD2 NAND
image (block 505), without touching anything else.
usage: hd2_setmac.py nand.img [wifi_mac [bt_mac]]"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hd2_mknand as m


class Patch(m.Nand):
    def __init__(self, path):
        self.f = open(path, 'r+b')


img = sys.argv[1]
wifi = m.parse_mac(sys.argv[2] if len(sys.argv) > 2 else '00:23:76:48:44:32')
bt = m.parse_mac(sys.argv[3] if len(sys.argv) > 3 else '00:23:76:48:44:33')
n = Patch(img)
m.write_factory_macs(n, wifi, bt)
n.close()
print('MACs written to', img)
