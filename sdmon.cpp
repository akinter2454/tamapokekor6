#include "sdmon.h"
#include "pin_config.h"
#include "pet.h"   // gRegionArt, REGIONS -- the mask this narrows
#include <FS.h>
#include <SD_MMC.h>

bool sdReady = false;
bool sdDirty = false;
bool sdArtDirty = false;
SdThumbs thumbs;

// v3.62.8: last successfully committed framed transfer. Used only to recover
// from the rare case where DONE4 itself is lost after the atomic SD rename.
static String gLastPut4Path;
static uint32_t gLastPut4Size = 0;
static uint32_t gLastPut4Crc = 0;
void sdRememberPut4(const String &path, uint32_t size, uint32_t crc) {
  gLastPut4Path = path; gLastPut4Size = size; gLastPut4Crc = crc;
}

bool PmdMon::load(int16_t dexNum, bool shiny) {
  // int16_t, NOT uint8_t. The dex reached 386 and this did not follow, so
  // everything from 256 up wrapped into Kanto: MARSHTOMP (258) opened
  // p002.bin and drew an IVYSAUR. Same trap that caught DexEntry::evolvesTo
  // and TrainerMon::dex when the expansion landed.
  if (dexNum < 1 || dexNum > DEX_COUNT) return false;
  unload();
  if (!sdReady) return false;

  char path[28];
  snprintf(path, sizeof(path), "/mons/p%s%03u.bin", shiny ? "s" : "", (unsigned)dexNum);
  File f = SD_MMC.open(path, FILE_READ);
  if (!f && shiny) {  // sin shiny PMD: usa el normal
    snprintf(path, sizeof(path), "/mons/p%03u.bin", (unsigned)dexNum);
    f = SD_MMC.open(path, FILE_READ);
  }
  if (!f) return false;

  uint32_t size = f.size();
  if (size < 7 || size > 3UL * 1024 * 1024) { f.close(); return false; }
  blob = (uint8_t *)ps_malloc(size);
  if (!blob || f.read(blob, size) != size) {
    if (blob) { free(blob); blob = nullptr; }
    f.close();
    return false;
  }
  f.close();
  const bool tpk3 = memcmp(blob, "TPK3", 4) == 0;
  const bool tpk2 = memcmp(blob, "TPK2", 4) == 0;
  if (!tpk2 && !tpk3) { unload(); return false; }

  dex = dexNum;                  // what is actually in here, for the tests
  uint8_t nActs = blob[4];
  memcpy(&palCount, blob + 5, 2);
  if (palCount > 256 || (uint32_t)7 + palCount * 2 > size) { unload(); return false; }
  memcpy(pal, blob + 7, palCount * 2);

  const uint8_t *p = blob + 7 + palCount * 2;
  const uint8_t *end = blob + size;
  for (uint8_t i = 0; i < nActs && p + 4 <= end; i++) {
    uint8_t id = p[0], w = p[1], h = p[2], nf = p[3];
    p += 4;
    if (id >= PMD_NACTS || nf > 24) { unload(); return false; }
    // TPK3 adds 4 bytes of exact visible bounds per frame before the timing
    // table. This removes a potentially multi-million-pixel scan every time a
    // large added Pokemon is loaded. TPK2 remains fully backward compatible.
    uint32_t boundBytes = tpk3 ? (uint32_t)nf * 4u : 0u;
    uint32_t bytes = boundBytes + (uint32_t)nf * 2u + (uint32_t)w * h * nf;
    if (w == 0 || h == 0 || nf == 0 || p + bytes > end) { unload(); return false; }
    PmdAct &a = acts[id];
    a.w = w;
    a.h = h;
    a.frames = nf;
    a.totalMs = 0;

    uint8_t left = w, right = 0, top = h, bottom = 0;
    bool anyPixel = false;
    if (tpk3) {
      for (uint8_t k = 0; k < nf; ++k) {
        uint8_t l=p[0], t=p[1], r=p[2], b=p[3]; p += 4;
        if (l >= r || t >= b || r > w || b > h) { l=0; t=0; r=w; b=h; }
        a.frameL[k]=l; a.frameT[k]=t; a.frameR[k]=r; a.frameB[k]=b;
        anyPixel = true;
        if (l < left) left=l; if (r > right) right=r;
        if (t < top) top=t; if (b > bottom) bottom=b;
      }
    }

    for (uint8_t k = 0; k < nf; k++) {
      a.ms[k] = p[0] | (p[1] << 8);
      if (a.ms[k] == 0) a.ms[k] = 80;
      a.totalMs += a.ms[k];
      p += 2;
    }
    a.data = p;
    p += (uint32_t)w * h * nf;

    if (!tpk3) {
      // Legacy original-region TPK2: compute union + exact frame boxes once.
      // The current full-catalog TPK3 files skip this scan entirely.
      for (uint8_t fidx = 0; fidx < nf; fidx++) {
        const uint8_t *fr = a.data + (uint32_t)fidx * w * h;
        uint8_t fl=w, frt=0, ft=h, fb=0; bool frameAny=false;
        for (uint8_t r = 0; r < h; r++) {
          const uint8_t *row = fr + (uint32_t)r * w;
          for (uint8_t c = 0; c < w; c++) {
            if (row[c] == 0xFF) continue;
            frameAny = anyPixel = true;
            if (c < fl) fl=c; if ((uint8_t)(c+1) > frt) frt=c+1;
            if (r < ft) ft=r; if ((uint8_t)(r+1) > fb) fb=r+1;
          }
        }
        if (!frameAny) { fl=0; frt=w; ft=0; fb=h; }
        a.frameL[fidx]=fl; a.frameR[fidx]=frt; a.frameT[fidx]=ft; a.frameB[fidx]=fb;
        if (fl < left) left=fl; if (frt > right) right=frt;
        if (ft < top) top=ft; if (fb > bottom) bottom=fb;
      }
    }
    if (anyPixel) {
      a.visL = left; a.visR = right; a.visT = top; a.base = bottom;
    } else {
      a.visL = 0; a.visR = w; a.visT = 0; a.base = h;
      for (uint8_t k=0;k<nf;k++) { a.frameL[k]=0; a.frameR[k]=w; a.frameT[k]=0; a.frameB[k]=h; }
    }
  }
  loaded = true;
  Serial.printf("cargado %s (%u KB, %s)\n", path, size / 1024, tpk3 ? "TPK3" : "TPK2");
  return true;
}

void PmdMon::unload() {
  if (blob) {
    free(blob);
    blob = nullptr;
  }
  for (auto &a : acts) {
    a.w = a.h = a.frames = a.base = 0;
    a.visL = a.visR = a.visT = 0;
    memset(a.frameL, 0, sizeof(a.frameL));
    memset(a.frameR, 0, sizeof(a.frameR));
    memset(a.frameT, 0, sizeof(a.frameT));
    memset(a.frameB, 0, sizeof(a.frameB));
    a.totalMs = 0;
    a.data = nullptr;
  }
  loaded = false;
}

bool SdThumbs::load() {
  if (!sdReady) return false;
  File f = SD_MMC.open("/mons/thumbs.bin", FILE_READ);
  if (!f) {
    Serial.println("sin thumbs.bin (galeria sin miniaturas)");
    return false;
  }
  uint32_t size = f.size();
  data = (uint8_t *)ps_malloc(size);
  if (!data || f.read(data, size) != size || memcmp(data, "TPTH", 4) != 0) {
    Serial.println("thumbs.bin invalido");
    if (data) { free(data); data = nullptr; }
    f.close();
    return false;
  }
  f.close();
  memcpy(&count, data + 4, 2);
  loaded = true;
  Serial.printf("miniaturas cargadas: %u (%u KB)\n", count, size / 1024);
  return true;
}

const uint8_t *SdThumbs::get(int16_t dex) const {
  if (!loaded || dex < 1 || dex > count) return nullptr;
  uint32_t off;
  memcpy(&off, data + 6 + 4 * (dex - 1), 4);
  return data + off;
}

// Which regions actually have their sprite pack on the card.
//
// Three probes per region, not one: the realistic failure is a half-finished
// copy of a 100 MB pack, and a single probe would call that region present.
// Start, middle and end, so a copy that stopped partway fails the check.
//
// This NARROWS gRegionArt, which starts as everything. A board with no SD is
// therefore untouched and keeps today's behaviour, which is the documented
// requirement -- an empty game would be a worse answer than a graceful one.
void sdScanRegionArt(bool verbose) {
  if (!sdReady) return;                 // no card: leave every region enabled
  uint16_t mask = 0;
  for (uint8_t r = 0; r < REGION_COUNT; r++) {
    if (r == REGION_ALL) continue;      // derived from the others, never probed
    const RegionInfo &rg = REGIONS[r];
    int16_t probe[3] = { rg.lo, (int16_t)((rg.lo + rg.hi) / 2), rg.hi };
    // Kanto..Alola keep their proven original-pack probes. New regions use
    // three enabled CURRENT base entries because appended/form IDs are not
    // contiguous and an unavailable PMDCollab slot must not become a probe.
    if (r >= 7) {
      int16_t picks[256]; uint16_t pn = 0;
      for (int16_t d = 1; d <= DEX_COUNT && pn < 256; d++) {
        if (DEX_REGION[d] == r && DEX_ENABLED[d] && d < 1200) picks[pn++] = d;
      }
      if (pn) { probe[0]=picks[0]; probe[1]=picks[pn/2]; probe[2]=picks[pn-1]; }
    }
    bool all = true;
    for (int i = 0; i < 3 && all; i++) {
      char path[28];
      snprintf(path, sizeof(path), "/mons/p%03u.bin", (unsigned)probe[i]);
      File f = SD_MMC.open(path, FILE_READ);
      if (!f) all = false; else f.close();
    }
    if (all) mask |= (uint16_t)(1u << r);
    if (verbose)
      Serial.printf("art: %-6s %s\n", rg.name, all ? "si" : "NO (falta el pack)");
  }
  gRegionArt = mask;
}

bool sdBegin() {
  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
  // Never auto-format on a transient mount/contact failure. The card may hold
  // both the existing Pokemon /mons pack and the independent /digimon pack.
  sdReady = SD_MMC.begin("/sdcard", true /* 1-bit mode */, false /* no auto-format */);
  if (sdReady) {
    Serial.printf("SD montada: %llu MB\n", SD_MMC.cardSize() / (1024ULL * 1024ULL));
    SD_MMC.mkdir("/mons");
    sdScanRegionArt();
  } else {
    Serial.println("SD no detectada (el juego usa los sprites de flash)");
  }
  return sdReady;
}

bool SdMon::load(int16_t dexNum, bool shiny) {
  if (dexNum < 1 || dexNum > DEX_COUNT) return false;
  unload();
  if (!sdReady) return false;

  char path[24];
  snprintf(path, sizeof(path), "/mons/%s%03u.bin", shiny ? "s" : "", (unsigned)dexNum);
  File f = SD_MMC.open(path, FILE_READ);
  if (!f && shiny) {  // sin variante shiny: usa la normal
    snprintf(path, sizeof(path), "/mons/%03u.bin", (unsigned)dexNum);
    f = SD_MMC.open(path, FILE_READ);
  }
  if (!f) {
    Serial.printf("no existe %s\n", path);
    return false;
  }

  char magic[4];
  uint16_t header[4];
  if (f.read((uint8_t *)magic, 4) != 4 || memcmp(magic, "TPK1", 4) != 0 ||
      f.read((uint8_t *)header, 8) != 8) {
    f.close();
    return false;
  }
  w = header[0];
  h = header[1];
  frames = header[2];
  frameMs = header[3];
  // acota dimensiones: evita size desbordado o absurdo con archivo corrupto
  if (f.read((uint8_t *)&palCount, 2) != 2 || palCount > 256 ||
      w == 0 || w > 256 || h == 0 || h > 256 || frames == 0 || frames > 64) {
    f.close();
    return false;
  }
  if (f.read((uint8_t *)pal, palCount * 2) != palCount * 2) {
    f.close();
    return false;
  }

  uint32_t size = (uint32_t)w * h * frames;
  data = (uint8_t *)ps_malloc(size);
  if (!data) {
    Serial.println("sin PSRAM para el sprite");
    f.close();
    return false;
  }
  uint32_t got = f.read(data, size);
  f.close();
  if (got != size) {
    Serial.printf("%s truncado (%u de %u)\n", path, got, size);
    unload();
    return false;
  }

  // zoom entero para que el bicho mida ~200 px de alto en pantalla
  scale = 200 / h;
  if (scale < 2) scale = 2;
  if (scale > 5) scale = 5;

  Serial.printf("cargado %s: %ux%u x%u frames @%ums, escala %u\n",
                path, w, h, frames, frameMs, scale);
  loaded = true;
  return true;
}

void SdMon::unload() {
  if (data) {
    free(data);
    data = nullptr;
  }
  loaded = false;
}

// ---------------------------------------------------------------------------
// Protocolo de carga por USB (para llenar la SD sin sacarla de la placa):
//   PUT4 <ruta> <bytes> <block> <crc32>\n + framed blocks -> numbered ACK/NAK + DONE4
//   PUT <ruta> <bytes>\n  + datos crudos   -> legacy "OK" ... "DONE"
//   SDINFO\n                               -> SDINFO OK proto=4 ...\n//   DEL mons/pNNN.bin\n                    -> DONE (safe /mons sprite cleanup)\n//   LS\n                                   -> listado de /mons
// Usar con tools/send_sd.py
// ---------------------------------------------------------------------------

bool sdSerialCommand(const String &line) {
  // v3.62.3: the game normally keeps USB TX non-blocking so debug prints can
  // never stall rendering when no serial monitor is attached.  PUT is the one
  // exception: the browser uses OK/#/DONE as flow-control.  If those tiny ACKs
  // are allowed to be dropped, a perfectly good SD transfer can randomly stop
  // on p001.bin (or hundreds of files later).  While an explicit host transfer
  // is active, make TX reliable, flush each ACK, then restore non-blocking mode.
  if (line == "SDINFO") {
    Serial.setTxTimeoutMs(1000);
    if (sdReady) {
      Serial.printf("SDINFO OK proto=4 cardMB=%llu\n",
                    (unsigned long long)(SD_MMC.cardSize() / (1024ULL * 1024ULL)));
    } else {
      Serial.println("SDINFO ERR no-card");
    }
    Serial.flush();
    Serial.setTxTimeoutMs(0);
    return true;
  }

  if (line.startsWith("DEL ")) {
    // v3.62.5: remove only retired sprite binaries. Never expose a generic
    // filesystem delete primitive over Web Serial.
    String path = line.substring(4);
    path.trim();
    if (!path.startsWith("/")) path = "/" + path;
    bool safe = path.startsWith("/mons/p") && path.endsWith(".bin") && path.indexOf("..") < 0;
    Serial.setTxTimeoutMs(1000);
    bool ok = false;
    if (sdReady && safe) {
      if (!SD_MMC.exists(path.c_str())) ok = true;  // idempotent cleanup
      else ok = SD_MMC.remove(path.c_str());
    }
    if (ok) { sdDirty = true; sdArtDirty = true; }
    Serial.println(ok ? "DONE" : "ERR");
    Serial.flush();
    Serial.setTxTimeoutMs(0);
    return true;
  }


  if (line.startsWith("PUTSTAT ")) {
    String path = line.substring(8); path.trim();
    if (!path.startsWith("/")) path = "/" + path;
    Serial.setTxTimeoutMs(1000);
    if (path == gLastPut4Path && gLastPut4Size > 0) {
      Serial.printf("PUTSTAT OK %u %08lX\n", (unsigned)gLastPut4Size, (unsigned long)gLastPut4Crc);
    } else {
      Serial.println("PUTSTAT MISS");
    }
    Serial.flush();
    Serial.setTxTimeoutMs(0);
    return true;
  }

  // v3.62.8 reliable framed transfer.  The original PUT stream depended on a
  // single readBytes() call returning the complete 2 KiB block.  USB CDC may
  // legally split one browser write into several reads, which occasionally
  // shifted block boundaries and left the browser waiting forever for '#'.
  //
  // PUT4 keeps the same Web-Serial installation workflow, but frames every
  // block as:
  //   "BLK4" + u16 seq + u16 len + u32 CRC32(IEEE) + raw payload
  // The receiver accumulates EXACTLY len bytes, validates CRC, acknowledges the
  // block number, and accepts a duplicate block if an ACK was lost.  The file is
  // written to *.part and atomically renamed only after an END4 commit frame, so
  // an interrupted transfer never destroys the previously valid sprite.
  if (line.startsWith("PUT4 ")) {
    String args = line.substring(5);
    int a = args.indexOf(' ');
    int b = a >= 0 ? args.indexOf(' ', a + 1) : -1;
    int c = b >= 0 ? args.indexOf(' ', b + 1) : -1;
    String path = a >= 0 ? args.substring(0, a) : String();
    uint32_t size = (a >= 0 && b > a) ? (uint32_t)args.substring(a + 1, b).toInt() : 0;
    uint32_t blockSize = (b >= 0 && c > b) ? (uint32_t)args.substring(b + 1, c).toInt() : 0;
    uint32_t expectedFileCrc = c >= 0 ? (uint32_t)strtoul(args.substring(c + 1).c_str(), nullptr, 16) : 0;

    auto txLine = [](const String &msg) {
      Serial.println(msg);
      Serial.flush();
    };
    auto crcUpdate = [](uint32_t crc, const uint8_t *data, size_t len) {
      for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit)
          crc = (crc >> 1) ^ (0xEDB88320UL & (uint32_t)-(int32_t)(crc & 1U));
      }
      return crc;
    };
    auto readExact = [](uint8_t *dst, size_t len, uint32_t idleTimeoutMs) {
      size_t got = 0;
      uint32_t lastProgress = millis();
      while (got < len) {
        int avail = Serial.available();
        if (avail > 0) {
          size_t want = len - got;
          if ((size_t)avail < want) want = (size_t)avail;
          size_t n = Serial.readBytes(dst + got, want);
          if (n > 0) {
            got += n;
            lastProgress = millis();
            continue;
          }
        }
        if ((uint32_t)(millis() - lastProgress) >= idleTimeoutMs) return false;
        delay(1);
        yield();
      }
      return true;
    };

    if (!path.startsWith("/")) path = "/" + path;
    const bool safePath = path.length() > 1 && path.indexOf("..") < 0;
    const bool valid = sdReady && safePath && size > 0 && size <= 4UL * 1024 * 1024 &&
                       blockSize >= 512 && blockSize <= 4096;
    Serial.setTxTimeoutMs(1500);
    Serial.setTimeout(250);
    if (!valid) {
      txLine("ERR4 PARAM");
      Serial.setTimeout(1000);
      Serial.setTxTimeoutMs(0);
      return true;
    }

    String tmpPath = path + ".part";
    if (SD_MMC.exists(tmpPath.c_str())) SD_MMC.remove(tmpPath.c_str());
    File f = SD_MMC.open(tmpPath.c_str(), FILE_WRITE);
    if (!f) {
      txLine("ERR4 OPEN");
      Serial.setTimeout(1000);
      Serial.setTxTimeoutMs(0);
      return true;
    }

    const uint32_t blocks = (size + blockSize - 1) / blockSize;
    txLine(String("OK4 ") + blocks);
    static uint8_t frameBuf[4096];
    uint8_t hdr[12];
    uint32_t nextSeq = 0;
    uint32_t remaining = size;
    uint32_t fileCrcState = 0xFFFFFFFFUL;
    bool transferOk = true;
    bool committed = false;

    // Once all data blocks have arrived we deliberately stay in this loop until
    // END4.  Therefore even the FINAL data ACK can be lost: the browser may
    // resend that same BLK4 and will receive the numbered ACK again safely.
    while (transferOk && !committed) {
      if (!readExact(hdr, sizeof(hdr), 30000)) {
        txLine(String("ERR4 TIMEOUT ") + nextSeq);
        transferOk = false;
        break;
      }
      const bool isBlock = memcmp(hdr, "BLK4", 4) == 0;
      const bool isEnd = memcmp(hdr, "END4", 4) == 0;
      const bool isAbort = memcmp(hdr, "ABT4", 4) == 0;
      const uint16_t seq = (uint16_t)hdr[4] | ((uint16_t)hdr[5] << 8);
      const uint16_t len = (uint16_t)hdr[6] | ((uint16_t)hdr[7] << 8);
      const uint32_t frameCrc = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) |
                                ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);

      if (isAbort) {
        txLine("ABORT4");
        transferOk = false;
        break;
      }
      if (isEnd) {
        if (nextSeq != blocks || remaining != 0 || len != 0 || seq != (uint16_t)blocks) {
          txLine(String("NAK END ORDER ") + nextSeq);
          continue;
        }
        const uint32_t actualFileCrc = fileCrcState ^ 0xFFFFFFFFUL;
        if (frameCrc != expectedFileCrc || actualFileCrc != expectedFileCrc) {
          txLine(String("ERR4 FILECRC ") + String(actualFileCrc, HEX));
          transferOk = false;
          break;
        }
        f.flush();
        f.close();
        // Two-phase replace: keep the old valid sprite as .bak4 until the new
        // .part file has been renamed successfully.  A transfer/power failure
        // before commit therefore cannot turn a good sprite into a partial one.
        String bakPath = path + ".bak4";
        if (SD_MMC.exists(bakPath.c_str())) SD_MMC.remove(bakPath.c_str());
        const bool hadOld = SD_MMC.exists(path.c_str());
        if (hadOld && !SD_MMC.rename(path.c_str(), bakPath.c_str())) {
          txLine("ERR4 BACKUP");
          transferOk = false;
          break;
        }
        if (!SD_MMC.rename(tmpPath.c_str(), path.c_str())) {
          if (hadOld) SD_MMC.rename(bakPath.c_str(), path.c_str());
          txLine("ERR4 RENAME");
          transferOk = false;
          break;
        }
        if (hadOld && SD_MMC.exists(bakPath.c_str())) SD_MMC.remove(bakPath.c_str());
        sdDirty = true;
        sdArtDirty = true;
        // Save the last committed transfer so the browser can recover if only
        // the final DONE4 line was lost after the SD rename succeeded.
        sdRememberPut4(path, size, actualFileCrc);
        txLine(String("DONE4 ") + String(actualFileCrc, HEX));
        committed = true;
        break;
      }
      if (!isBlock || len == 0 || len > blockSize) {
        txLine(String("ERR4 HEADER ") + nextSeq);
        transferOk = false;
        break;
      }
      if (!readExact(frameBuf, len, 30000)) {
        txLine(String("ERR4 DATA_TIMEOUT ") + seq);
        transferOk = false;
        break;
      }
      uint32_t blockCrc = crcUpdate(0xFFFFFFFFUL, frameBuf, len) ^ 0xFFFFFFFFUL;
      if (blockCrc != frameCrc) {
        txLine(String("NAK ") + seq + " CRC");
        continue;
      }

      // ACK-loss recovery: consume and validate a repeated already-committed
      // block, but never write it twice.
      if ((uint32_t)seq < nextSeq) {
        txLine(String("ACK ") + seq);
        continue;
      }
      if ((uint32_t)seq > nextSeq) {
        txLine(String("NAK ") + seq + " ORDER " + nextSeq);
        continue;
      }
      const uint32_t expectedLen = remaining > blockSize ? blockSize : remaining;
      if (len != expectedLen) {
        txLine(String("NAK ") + seq + " LEN " + expectedLen);
        continue;
      }
      size_t wr = f.write(frameBuf, len);
      if (wr != len) {
        txLine(String("ERR4 WRITE ") + seq);
        transferOk = false;
        break;
      }
      fileCrcState = crcUpdate(fileCrcState, frameBuf, len);
      remaining -= len;
      nextSeq++;
      txLine(String("ACK ") + seq);
      yield();
    }

    if (f) f.close();
    if (!committed && SD_MMC.exists(tmpPath.c_str())) SD_MMC.remove(tmpPath.c_str());
    Serial.setTimeout(1000);
    Serial.setTxTimeoutMs(0);
    return true;
  }

  if (line.startsWith("PUT ")) {
    int sp = line.lastIndexOf(' ');
    String path = line.substring(4, sp);
    uint32_t size = line.substring(sp + 1).toInt();
    Serial.setTxTimeoutMs(1000);
    if (!sdReady || size == 0 || size > 4 * 1024 * 1024) {
      Serial.println("ERR");
      Serial.flush();
      Serial.setTxTimeoutMs(0);
      return true;
    }
    if (!path.startsWith("/")) path = "/" + path;
    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) {
      Serial.println("ERR");
      Serial.flush();
      Serial.setTxTimeoutMs(0);
      return true;
    }
    Serial.println("OK");
    Serial.flush();
    static uint8_t buf[2048];
    uint32_t remaining = size;
    bool writeOk = true;
    Serial.setTimeout(8000);
    while (remaining > 0) {
      size_t want = remaining > sizeof(buf) ? sizeof(buf) : remaining;
      size_t n = Serial.readBytes(buf, want);
      if (n == 0) { writeOk = false; break; }  // host/data timeout
      size_t wr = f.write(buf, n);
      if (wr != n) { writeOk = false; break; }
      remaining -= n;
      Serial.println("#");  // host may send the next 2 KiB only after this ACK
      Serial.flush();
      yield();
    }
    f.close();
    Serial.setTimeout(1000);
    const bool done = writeOk && remaining == 0;
    sdDirty = done;
    // A pack file just landed, so which regions are playable may have changed.
    // Flag it rather than rescanning here: this runs between the last data block
    // and the DONE the host is waiting on, and 15 file opens belong nowhere near
    // that. loop() picks it up.
    if (done) sdArtDirty = true;
    Serial.println(done ? "DONE" : "ERR");
    Serial.flush();
    Serial.setTxTimeoutMs(0);
    return true;
  } else if (line == "LS") {
    File dir = SD_MMC.open("/mons");
    if (dir) {
      File e;
      while ((e = dir.openNextFile())) {
        Serial.printf("%s %u\n", e.name(), (uint32_t)e.size());
        e.close();
      }
      dir.close();
    }
    Serial.println("DONE");
    return true;
  }
  return false;
}
