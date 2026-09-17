from pathlib import Path
import re, zlib, random

root = Path(__file__).resolve().parents[1]
ino = (root / 'TamaPoke.ino').read_text(encoding='utf-8')
sd = (root / 'sdmon.cpp').read_text(encoding='utf-8')
html = (root / 'TamaPoke-KO-OneClick-Installer.html').read_text(encoding='utf-8')

m = re.search(r'^#define\s+FW_VERSION\s+"([^"]+)"', ino, re.M)
assert m and re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', m.group(1)), m.group(1) if m else 'missing'
fw_version = m.group(1)

# Game/debug output remains non-blocking outside explicit transfer work.
assert 'Serial.setTxTimeoutMs(0);' in ino

# v4 capability/preflight + backward compatible legacy PUT.
assert 'SDINFO OK proto=4' in sd
assert 'if (line.startsWith("PUT4 "))' in sd
assert 'if (line.startsWith("PUT "))' in sd
assert 'if (line.startsWith("PUTSTAT "))' in sd
assert 'if (line.startsWith("DEL "))' in sd

# Framed data protocol: exact-length accumulator, per-block CRC, numbered ACK,
# duplicate ACK-loss recovery, explicit END commit and abort control frame.
for needle in [
    'memcmp(hdr, "BLK4", 4)',
    'memcmp(hdr, "END4", 4)',
    'memcmp(hdr, "ABT4", 4)',
    'while (got < len)',
    'lastProgress = millis()',
    '0xEDB88320UL',
    'String("ACK ") + seq',
    'String("NAK ") + seq + " CRC"',
    'if ((uint32_t)seq < nextSeq)',
    'if ((uint32_t)seq > nextSeq)',
    'fileCrcState ^ 0xFFFFFFFFUL',
    'sdRememberPut4(path, size, actualFileCrc)',
]:
    assert needle in sd, needle

# Interrupted transfers write only to a temporary file. Commit keeps a backup
# of the old valid target until the new rename has succeeded.
for needle in [
    'String tmpPath = path + ".part"',
    'String bakPath = path + ".bak4"',
    'SD_MMC.rename(path.c_str(), bakPath.c_str())',
    'SD_MMC.rename(tmpPath.c_str(), path.c_str())',
    'if (!committed && SD_MMC.exists(tmpPath.c_str())) SD_MMC.remove(tmpPath.c_str())',
]:
    assert needle in sd, needle

# Browser must require proto4 and use PUT4 frames rather than the legacy '#'
# data ACK.  Each block gets retry logic; the whole file gets one recovery retry.
for needle in [
    'Number(pm[1]) < 4',
    'PUT4 ${name} ${data.length} ${BLOCK} ${hex32(fileCrc)}',
    "makePut4Frame('BLK4', seq, chunk)",
    "makePut4Frame('END4', blocks, null, fileCrc)",
    "makePut4Frame('ABT4', 0, null, 0)",
    'async function waitBlockReply',
    'for (let attempt = 1; attempt <= 4; attempt++)',
    'if (reply.kind === \'ack\')',
    'if (reply.kind === \'fatal\')',
    'async function verifyLastPut4',
    'for (let fileAttempt = 1; fileAttempt <= 2; fileAttempt++)',
    'DONE4 응답은 누락됐지만 SD 최종 CRC 확인 성공',
]:
    assert needle in html, needle
assert "waitFor('#', 15000)" not in html, 'installer still uses legacy raw-block # ACK'
im = re.search(r'const\s+FW_VERSION\s*=\s*["\']([^"\']+)["\']', html)
assert im and im.group(1).startswith(fw_version + '-ko-'), im.group(1) if im else 'installer version missing'

# CRC32 polynomial agrees with standard IEEE CRC32 used by browser/firmware.
sample = b'TamaPoke PUT4 regression\x00\x01\xff'
assert zlib.crc32(sample) & 0xffffffff == 0xE04B4E4F

# Model the exact-read property with deliberately fragmented USB chunks.  This
# is the failure mode seen on-device: one 2048-byte browser write can arrive as
# many smaller CDC reads.  Accumulation must reconstruct the original block.
rng = random.Random(3628)
data = bytes(rng.randrange(256) for _ in range(2048))
parts=[]; i=0
while i < len(data):
    n=min(len(data)-i, rng.randint(1,173))
    parts.append(data[i:i+n]); i+=n
rebuilt=b''.join(parts)
assert rebuilt == data and (zlib.crc32(rebuilt)&0xffffffff)==(zlib.crc32(data)&0xffffffff)

# Stateful model of retry semantics: CRC failure must not advance the write
# cursor, while an ACK-loss duplicate must ACK without writing twice.
class RxModel:
    def __init__(self):
        self.next_seq=0; self.out=bytearray()
    def block(self, seq, payload, advertised_crc):
        if (zlib.crc32(payload)&0xffffffff) != advertised_crc:
            return 'NAK'
        if seq < self.next_seq:
            return 'ACK'
        if seq > self.next_seq:
            return 'NAK'
        self.out.extend(payload); self.next_seq += 1
        return 'ACK'

rx=RxModel()
a=b'A'*2048; b=b'B'*731
assert rx.block(0,a,zlib.crc32(a)&0xffffffff)=='ACK'
# Simulate lost ACK: browser sends block 0 again. It must not duplicate bytes.
assert rx.block(0,a,zlib.crc32(a)&0xffffffff)=='ACK' and len(rx.out)==len(a)
# Corrupt/incorrect CRC cannot advance seq 1.
assert rx.block(1,b,(zlib.crc32(b)+1)&0xffffffff)=='NAK' and rx.next_seq==1
assert rx.block(1,b,zlib.crc32(b)&0xffffffff)=='ACK'
assert bytes(rx.out)==a+b

print(f'SD/WebSerial protocol regression OK: fw={fw_version} proto=4 framed exact-read + CRC + numbered retry ACK + atomic commit')
