"""Writes deterministic archive fixtures to tests/fixtures/archives (tracked).

Python's zipfile/tarfile/gzip act as an independent encoder for aicmd's own decoder.
Run: python scripts/make-archive-fixtures.py
"""
import bz2, gzip, io, lzma, os, shutil, struct, subprocess, tarfile, tempfile, time, zipfile, zlib
from pathlib import Path

out = Path(__file__).resolve().parent.parent / 'tests' / 'fixtures' / 'archives'
out.mkdir(parents=True, exist_ok=True)
STAMP = (2024, 5, 6, 7, 8, 10)
EPOCH = 1715000000


def big_text(n):
    # Repetitive but varied: exercises dynamic Huffman blocks and long back-references.
    words = ['alpha', 'beta', 'gamma', 'delta', 'commander', 'panel', 'archive', 'search', 'lane', 'folder']
    lines, x = [], 12345
    while sum(map(len, lines)) < n:
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        lines.append(f'{len(lines):07d} ' + ' '.join(words[(x >> s) % 10] for s in range(0, 30, 3)) + '\n')
    return ''.join(lines).encode()[:n]


def zinfo(name, method=zipfile.ZIP_DEFLATED):
    i = zipfile.ZipInfo(name, STAMP)
    i.compress_type = method
    i.external_attr = 0o644 << 16
    return i


with zipfile.ZipFile(out / 'sample.zip', 'w') as z:
    z.writestr(zinfo('readme.txt'), b'hello from a zip\n' * 20)
    z.writestr(zinfo('stored.bin', zipfile.ZIP_STORED), bytes(range(256)) * 4)
    d = zinfo('docs/'); d.external_attr = (0o40755 << 16) | 0x10; z.writestr(d, b'')
    z.writestr(zinfo('docs/guide.md'), b'# Guide\n')
    z.writestr(zinfo('deep/a/b/c.txt'), b'implicit parents')          # no explicit folder entries
    z.writestr(zinfo('日本語/名前.txt'), '日本語'.encode())  # UTF-8 flag set by zipfile
    z.writestr(zinfo('../evil.txt'), b'zip slip')                      # must be ignored
    z.writestr(zinfo('empty.txt'), b'')
    with z.open(zinfo('zip64.txt'), 'w', force_zip64=True) as f:      # ZIP64 local extra
        f.write(b'zip64 entry\n')
# CP437 name (no UTF-8 flag): 0x81 is u-umlaut in CP437.
raw = io.BytesIO()
with zipfile.ZipFile(raw, 'w') as z:
    z.writestr(zinfo('cp437-X.txt'), b'cp437')
data = bytearray(raw.getvalue())
for i in range(len(data) - 11):
    if data[i:i + 11] == b'cp437-X.txt':
        data[i + 6] = 0x81
(out / 'cp437.zip').write_bytes(bytes(data))

text = big_text(400_000)
with zipfile.ZipFile(out / 'big.zip', 'w') as z:
    for level in (1, 6, 9):
        z.writestr(zinfo(f'level{level}.txt'), text, compresslevel=level)
    z.writestr(zinfo('random.bin'), bytes((i * 2654435761 >> 13) & 255 for i in range(40_000)))

def add(t, name, payload=None, kind=tarfile.REGTYPE, link=''):
    i = tarfile.TarInfo(name); i.mtime = EPOCH; i.type = kind; i.linkname = link
    if kind == tarfile.DIRTYPE: i.mode = 0o755
    i.size = len(payload) if payload is not None else 0
    t.addfile(i, io.BytesIO(payload) if payload is not None else None)

long_name = 'long/' + 'x' * 120 + '/file.txt'
for fmt, suffix in ((tarfile.GNU_FORMAT, 'gnu'), (tarfile.PAX_FORMAT, 'pax')):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w', format=fmt) as t:
        add(t, 'dir', kind=tarfile.DIRTYPE)
        add(t, 'dir/one.txt', b'one\n')
        add(t, long_name, b'long name\n')
        add(t, 'über/日本.txt', '日本'.encode())
        add(t, 'dir/link', kind=tarfile.SYMTYPE, link='one.txt')
        add(t, 'big.txt', text[:100_000])
        add(t, 'last.txt', b'last')
    (out / f'sample-{suffix}.tar').write_bytes(buf.getvalue())
    if suffix == 'pax':
        (out / 'sample.tar.gz').write_bytes(gzip.compress(buf.getvalue(), mtime=EPOCH))
# A two-member gzip stream (concatenated) holding one tar.
tarbytes = (out / 'sample-gnu.tar').read_bytes()
half = len(tarbytes) // 2
(out / 'multi.tgz').write_bytes(gzip.compress(tarbytes[:half], mtime=EPOCH) + gzip.compress(tarbytes[half:], mtime=EPOCH))

# Raw deflate vectors for the decoder unit test: stored, fixed and dynamic blocks.
vectors = []
for level, payload in ((0, b'stored block'), (1, b'abcabcabcabcabc fixed'), (9, text[:50_000])):
    c = zlib.compressobj(level, zlib.DEFLATED, -15)
    vectors.append((payload, c.compress(payload) + c.flush()))
with open(out / 'deflate.bin', 'wb') as f:
    for payload, packed in vectors:
        f.write(struct.pack('<II', len(payload), len(packed)) + packed + struct.pack('<I', zlib.crc32(payload)))
# TAR inside xz / bzip2 streams (read through 7z.dll), encoded by Python's lzma and bz2.
(out / 'sample.tar.xz').write_bytes(lzma.compress(tarbytes, format=lzma.FORMAT_XZ))
(out / 'sample.tar.bz2').write_bytes(bz2.compress(tarbytes, 9))

# 7z archives need 7-Zip itself; set SEVENZIP to a 7z.exe (scripts/setup-7zip.ps1 extracts 26.06).
# RAR fixtures (rar4.rar, rar5.rar) are libarchive test files; see tests/fixtures/archives/NOTICE.md.
seven = os.environ.get('SEVENZIP')
if seven:
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / 'src'
        for rel, payload in (('readme.txt', b'hello from a 7z\n' * 20), ('docs/guide.md', b'# Guide\n'),
                             ('日本語/名前.txt', '日本語'.encode()), ('empty.txt', b''), ('big.txt', text)):
            f = src / rel; f.parent.mkdir(parents=True, exist_ok=True); f.write_bytes(payload)
            os.utime(f, (EPOCH, EPOCH))
        (src / 'emptydir').mkdir()
        for name, extra in (('sample.7z', ['-ms=on', '-mx=9']), ('encrypted.7z', ['-psecret']), ('header-encrypted.7z', ['-psecret', '-mhe=on'])):
            target = out / name
            if target.exists(): target.unlink()
            subprocess.run([seven, 'a', '-t7z', '-bd', '-y', str(target), '.'] + extra, cwd=src, check=True, stdout=subprocess.DEVNULL)
print('fixtures in', out)
