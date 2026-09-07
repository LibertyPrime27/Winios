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
