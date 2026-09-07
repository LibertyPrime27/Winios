#!/usr/bin/env python3
"""Rebuild the installer-detection fixtures.

Each one is a minimal PE header -- MZ, an e_lfanew pointing at a PE signature,
an optional-header magic -- with the marker bytes of one installer family
placed inside it. They are not installers and cannot run: identification only
reads bytes, so that is all a fixture has to be.

The markers are the same ones tools/import/setupkind.c looks for, which is the
point: a fixture built from a different string would test nothing.

fake_zipsfx.exe is the exception. Its payload is a real zip, because "can a
self-extracting archive be unpacked" cannot be answered against a fake one.
"""
import io, os, struct, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))

def pe(markers, size=8192, magic=0x10b, tail=b''):
    b = bytearray(size)
    b[0:2] = b'MZ'
    struct.pack_into('<I', b, 0x3C, 0x80)       # e_lfanew
    b[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<H', b, 0x80 + 24, magic) # optional header magic
    off = 0x200
    for m in markers:
        b[off:off + len(m)] = m
        off += len(m) + 16
    return bytes(b) + tail

def write(name, data):
    with open(os.path.join(HERE, name), 'wb') as f:
        f.write(data)
    print(f"  {name:22} {len(data)} bytes")

def zip_payload():
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('game/readme.txt', 'hello from inside a zip sfx\n')
        z.writestr('game/game.exe', b'MZ' + b'\0' * 200)
    return buf.getvalue()

def compressible_zip():
    """A zip whose payload is far larger than the file holding it.

    The point of measuring an archive's *uncompressed* total is that the
    download's size tells you nothing about whether it will fit. The SFX
    fixture above cannot show that -- its 8 KB PE stub dwarfs its 230-byte
    payload -- so this one exists purely to make the difference real: 4 MB of
    highly compressible data in a file of a few KB.
    """
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('big/assets.dat', b'\0' * (4 << 20))
        z.writestr('big/readme.txt', 'four megabytes of nothing, in a few kilobytes\n')
    return buf.getvalue()


def zip64_archive():
    """A zip64 archive: the zip64 EOCD record and its locator sit BETWEEN the
    central directory and the ordinary EOCD.

    Python only emits those records for an archive that genuinely needs them
    (over 4 GB, or more than 65535 entries), so the trailer is assembled here
    instead. The layout is what a real zip64 file has, and it is the layout
    that matters: a reader that finds the directory by subtracting its size
    from the EOCD's position lands 76 bytes late -- 56 for the record, 20 for
    the locator -- and then every local-header offset is wrong.
    """
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('big/a.txt', b'zip64 entry one\n')
        z.writestr('big/b.txt', b'zip64 entry two\n')
    d = bytearray(buf.getvalue())
    i = d.rfind(b'PK\x05\x06')
    nent, cd_size, cd_off = struct.unpack_from('<HII', d, i + 10)

    # zip64 EOCD record, then its locator, then the EOCD with its 32-bit
    # fields saturated so a reader has to go and find the real ones.
    z64 = struct.pack('<IQHHIIQQQQ', 0x06064b50, 44, 45, 45, 0, 0,
                      nent, nent, cd_size, cd_off)
    loc = struct.pack('<IIQI', 0x07064b50, 0, i, 1)
    eocd = bytearray(d[i:])
    struct.pack_into('<HHHHII', eocd, 4, 0, 0, 0xFFFF, 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFF)
    return bytes(d[:i]) + z64 + loc + bytes(eocd)


def comment_trap_zip():
    """An archive whose comment contains an EOCD signature.

    A backwards scan for the signature finds this one first, because a comment
    comes *after* the record it belongs to. The real record is identifiable
    because its declared comment length reaches exactly the end of the file,
    and the decoy's does not.
    """
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('ok.txt', 'the real directory was found\n')
    d = bytearray(buf.getvalue())
    i = d.rfind(b'PK\x05\x06')
    decoy = b'PK\x05\x06' + bytes(18)      # a whole fake record, in the comment
    struct.pack_into('<H', d, i + 20, len(decoy))
    return bytes(d) + decoy


print("installer-detection fixtures:")
write('fake_inno.exe',     pe([b'TSetupLdrWindow', b'Inno Setup 6.2.0']))
write('fake_nsis.exe',     pe([b'Nullsoft Install System v3.08',
                               bytes([0xEF, 0xBE, 0xAD, 0xDE]) + b'NullsoftInst']))
write('fake_is.exe',       pe([b'InstallShield', b'ISSetupPrerequisites']))
write('fake_msi.exe',      pe([b'MsiInstallProduct', b'msiexec']))
write('fake_7zsfx.exe',    pe([b'7zS.sfx', b'!@Install@!UTF-8!']))
write('fake_rarsfx.exe',   pe([b'WinRAR self-extracting archive',
                               bytes([0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00])]))
write('fake_wise.exe',     pe([b'Wise Installation Wizard', b'WiseMain']))
write('fake_sf.exe',       pe([b'Setup Factory 9.0', b'irsetup.exe']))
write('fake_plain.exe',    pe([b'just a game', b'nothing to see']))
write('fake_setupish.exe', pe([b'requireAdministrator']))
write('not_a_pe.bin',      b'this is not a PE at all' * 40)
write('fake_zipsfx.exe',   pe([b'sfx loader'], tail=zip_payload()))
write('compressible.zip',  compressible_zip())
write('zip64.zip',         zip64_archive())
write('comment_trap.zip',  comment_trap_zip())
