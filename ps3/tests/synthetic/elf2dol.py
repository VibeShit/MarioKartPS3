#!/usr/bin/env python3
"""Minimal ELF32 big-endian -> GameCube/Wii DOL converter for the synthetic test."""
import struct
import sys


def main(src, dst):
    data = open(src, 'rb').read()
    assert data[:4] == b'\x7fELF' and data[4] == 1 and data[5] == 2, 'need ELF32 BE'
    e_entry = struct.unpack_from('>I', data, 0x18)[0]
    e_shoff = struct.unpack_from('>I', data, 0x20)[0]
    e_shentsize, e_shnum = struct.unpack_from('>HH', data, 0x2E)

    texts, datas, bss = [], [], None
    for i in range(e_shnum):
        _, sh_type, flags, addr, off, size = struct.unpack_from('>6I', data, e_shoff + i * e_shentsize)
        if not (flags & 2) or size == 0:  # SHF_ALLOC
            continue
        if sh_type == 8:  # SHT_NOBITS
            if bss is None:
                bss = (addr, size)
            else:
                lo = min(bss[0], addr)
                bss = (lo, max(bss[0] + bss[1], addr + size) - lo)
        elif flags & 4:  # SHF_EXECINSTR
            texts.append((addr, data[off:off + size]))
        else:
            datas.append((addr, data[off:off + size]))
    assert len(texts) <= 7 and len(datas) <= 11

    header = bytearray(0x100)
    body = bytearray()
    offset = 0x100

    def place(slot_off, slot_addr, slot_size, idx, addr, seg):
        nonlocal offset
        pad = (-len(seg)) % 32
        struct.pack_into('>I', header, slot_off + idx * 4, offset)
        struct.pack_into('>I', header, slot_addr + idx * 4, addr)
        struct.pack_into('>I', header, slot_size + idx * 4, len(seg) + pad)
        body.extend(seg)
        body.extend(b'\0' * pad)
        offset += len(seg) + pad

    for i, (addr, seg) in enumerate(texts):
        place(0x00, 0x48, 0x90, i, addr, seg)
    for i, (addr, seg) in enumerate(datas):
        place(0x1C, 0x64, 0xAC, i, addr, seg)
    if bss:
        struct.pack_into('>II', header, 0xD8, bss[0], bss[1])
    struct.pack_into('>I', header, 0xE0, e_entry)
    open(dst, 'wb').write(bytes(header) + bytes(body))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
