#include "audio.h"
#include "gbsynth.h"
#include "music.h"
#include "pin_config.h"
#include <Arduino.h>
#include <Wire.h>
#include <ESP_I2S.h>
#include <Preferences.h>

// ---------------------------------------------------------------------------
// Audio del TamaPoke: códec ES8311 (DAC -> amplificador PA -> altavoz) por I2S.
// Init del ES8311 portado del driver oficial de Espressif (esp-bsp), fijado a
// MCLK=4.096MHz (256*fs), 16kHz, 16-bit, esclavo I2S. Los efectos son tonos
// cuadrados (estilo Game Boy) sintetizados en una tarea aparte para no
// bloquear el loop de juego.
// ---------------------------------------------------------------------------

#define ES8311_ADDR 0x18
#define SAMPLE_RATE 16000

static I2SClass i2s;
static bool gReady = false;
static bool gOn = true;
static bool gBtnOn = true;
static QueueHandle_t gQ = nullptr;
static SemaphoreHandle_t gAudioMutex = nullptr;
static TaskHandle_t gAudioTaskHandle = nullptr;
static volatile bool gAmpOn = false;
static volatile bool gScreenAwake = true;  // transient power-state mute; never persisted
static volatile uint32_t gAmpHoldUntil = 0;
static const uint8_t AUDIO_SELFTEST_ID = 0xFE;

// ---- I2C del códec ----
static bool esW(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}
static uint8_t esR(uint8_t reg) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(ES8311_ADDR, 1);
  return Wire.available() ? Wire.read() : 0;
}

// Secuencia de init VERIFICADA EN ESTA PLACA (proyecto PlaneRadar2.0, misma
// Waveshare 1.75). Clave: reloj DERIVADO DEL BCLK (reg01=0xBF, sin MCLK externo)
// y referencia interna que alimenta el DAC (reg44=0x58); sin esos dos el codec
// respondia por I2C pero no salia audio. 16kHz, 16-bit, esclavo I2S.
static bool es8311Init() {
  Wire.beginTransmission(ES8311_ADDR);
  if (Wire.endTransmission() != 0) return false;

  // open()
  esW(0x0D, 0xFA); esW(0x44, 0x08); esW(0x44, 0x08);  // power up + quirk de 1a escritura
  esW(0x01, 0x30); esW(0x02, 0x00); esW(0x03, 0x10); esW(0x16, 0x24);
  esW(0x04, 0x10); esW(0x05, 0x00); esW(0x0B, 0x00); esW(0x0C, 0x00);
  esW(0x10, 0x1F); esW(0x11, 0x7F);
  esW(0x00, 0x80); esW(0x00, 0x80);                   // reset clock, esclavo
  esW(0x01, 0xBF);                                    // clk src = BCLK (sin MCLK externo)
  { uint8_t r = esR(0x06); r &= ~0x20; esW(0x06, r); }  // SCLK no invertido
  esW(0x13, 0x10); esW(0x1B, 0x0A); esW(0x1C, 0x6A);
  esW(0x44, 0x58);                                    // referencia interna -> alimenta el DAC

  // config_sample(): BCLK*8 = DIG_MCLK
  esW(0x02, 0x18); esW(0x05, 0x00); esW(0x03, 0x10); esW(0x04, 0x20);
  { uint8_t r = esR(0x07); r &= 0xC0; esW(0x07, r); }
  esW(0x08, 0xFF);
  { uint8_t r = esR(0x06); r &= 0xE0; r |= 0x03; esW(0x06, r); }  // bclk_div=4

  // formato I2S 16-bit
  esW(0x09, 0x0C); esW(0x0A, 0x0C);

  // start() DAC esclavo
  esW(0x00, 0x80); esW(0x01, 0xBF); esW(0x09, 0x0C); esW(0x0A, 0x0C);
  esW(0x17, 0xBF); esW(0x0E, 0x02); esW(0x12, 0x00); esW(0x14, 0x1A);
  esW(0x0D, 0x01); esW(0x15, 0x40); esW(0x37, 0x08); esW(0x45, 0x00);

  // volumen + unmute
  esW(0x32, 0xBF);                                    // volumen DAC ~0 dB
  { uint8_t r = esR(0x31); r &= 0x9F; esW(0x31, r); }  // unmute
  return true;
}

// ---- sintetizador de tono cuadrado ----
struct Note { uint16_t f, ms; };

static const Note N_TAP[]    = {{784, 58}, {0, 10}, {1047, 72}};  // v3.34: repeatable virtual-pet chirp on the original synth path
static const Note N_EAT[]    = {{660, 45}, {0, 12}, {660, 45}};
static const Note N_PLAY[]   = {{784, 45}, {988, 60}};
static const Note N_HEART[]  = {{1047, 55}, {1319, 90}};
static const Note N_HATCH[]  = {{523, 80}, {659, 80}, {784, 110}, {1047, 170}};
static const Note N_EVOLVE[] = {{523, 80}, {659, 80}, {784, 80}, {1047, 90}, {1319, 230}};
static const Note N_MEDAL[]  = {{784, 70}, {0, 25}, {784, 70}, {0, 25}, {1047, 200}};
static const Note N_DENY[]   = {{300, 110}, {200, 170}};
static const Note N_BYE[]    = {{784, 150}, {659, 150}, {523, 280}};
static const Note N_LEVEL[]  = {{784, 70}, {1047, 130}};

struct SfxDef { const Note *n; uint8_t len; };
// battle cues, deliberately short: they land between turns, not over them
static const Note N_HIT[]    = { {180, 40}, {120, 50} };
static const Note N_BEAM[]   = { {880, 30}, {1180, 30}, {1560, 60} };
static const Note N_STATUS[] = { {440, 60}, {370, 60}, {330, 90} };
static const Note N_SUPER[]  = { {1200, 40}, {1600, 40}, {2000, 80} };
static const Note N_FAINT[]  = { {520, 90}, {400, 110}, {300, 140}, {200, 200} };
static const Note N_VICTORY[] = { {784, 120}, {784, 120}, {784, 120}, {1047, 320},
                                  {880, 140}, {1047, 420} };

static const SfxDef SFX[SFX_COUNT] = {
  {N_TAP, 3}, {N_EAT, 3}, {N_PLAY, 2}, {N_HEART, 2}, {N_HATCH, 4},
  {N_EVOLVE, 5}, {N_MEDAL, 5}, {N_DENY, 2}, {N_BYE, 3}, {N_LEVEL, 2},
  {N_HIT, 2}, {N_BEAM, 3}, {N_STATUS, 3}, {N_SUPER, 3}, {N_FAINT, 4},
  {N_VICTORY, 6},
};

// A loop, not a one-shot: the task walks it and starts over.
static const Note M_BATTLE[] = {
  {392, 150}, {523, 150}, {659, 150}, {523, 150},
  {440, 150}, {587, 150}, {698, 150}, {587, 150},
  {392, 150}, {523, 150}, {659, 200}, {0, 100},
  {659, 120}, {587, 120}, {523, 120}, {440, 240}, {0, 160},
};
static const Note M_VICTORY[] = {
  {784, 140}, {880, 140}, {988, 140}, {1047, 420}, {0, 200},
};
struct TuneDef { const Note *n; uint8_t len; bool loop; };
static const TuneDef MUSIC[] = {
  { nullptr, 0, false },
  { M_BATTLE, 17, true },
  { M_VICTORY, 5, false },
};
static volatile uint8_t gMusic = MUS_NONE;
static volatile uint8_t gVol = 7;
static volatile uint8_t gBtnVol = 8;
static volatile bool gUiTapResolving = false;

static int16_t buf[256 * 2];  // estéreo intercalado (L=R)
static int16_t mono[256];
static GbSynth gSyn;

// The SFX tables are written in Hz; the synth speaks the Game Boy's frequency
// register. Converting here means the cues did not have to be rewritten.
static uint16_t hzToGb(uint16_t hz) {
  if (!hz) return 0;
  int32_t f = 2048 - (int32_t)(131072u / hz);
  return (f < 0 || f > 2047) ? 0 : (uint16_t)f;
}

// Raw square-wave output is kept only for the settings self-test. Normal UI
// feedback uses the original GbSynth effect path again in v3.34 because that
// path is already proven to retrigger reliably on this hardware.
static void playRawSquare(uint16_t f, uint16_t ms, uint8_t volume) {
  int total = SAMPLE_RATE * ms / 1000;
  if (total <= 0) return;
  int half = f ? (SAMPLE_RATE / (2 * (int)f)) : 0;
  if (f && half < 1) half = 1;
  const int16_t amp = (int16_t)(850 * (volume > 10 ? 10 : volume));  // max 8500
  int phase = 0, done = 0;
  bool high = true;
  while (done < total) {
    int n = total - done; if (n > 256) n = 256;
    for (int i = 0; i < n; i++) {
      int16_t v = 0;
      if (f && volume) {
        v = high ? amp : -amp;
        int idx = done + i;
        // Short ramps avoid a loud click when PA wakes up.
        if (idx < 24) v = (int16_t)(v * idx / 24);
        else if (idx > total - 40) v = (int16_t)(v * ((total - idx) > 0 ? (total - idx) : 0) / 40);
        if (++phase >= half) { phase = 0; high = !high; }
      }
      buf[i * 2] = v;
      buf[i * 2 + 1] = v;
    }
    i2s.write((uint8_t *)buf, n * 4);
    done += n;
  }
}

static void playUiChirpDirect(uint8_t volume) {
  // v3.38: use frequencies/durations close to the known-good speaker self-test.
  // The previous 52/60 ms chirp could finish while the tiny PA was still waking.
  playRawSquare(700, 82, volume);
  playRawSquare(0, 12, volume);
  playRawSquare(940, 92, volume);
  playRawSquare(0, 14, volume);
}

static void playAudioSelfTest() {
  // Forced diagnostic tone: ignores mute settings so the user can distinguish
  // an audio-path problem from a preference/volume problem.
  playRawSquare(659, 110, 8);
  playRawSquare(0, 35, 8);
  playRawSquare(880, 110, 8);
  playRawSquare(0, 35, 8);
  playRawSquare(1175, 160, 8);
}

// Renders `ms` of GbSynth game/music audio and pushes it to the codec. UI
// feedback and the explicit settings self-test use the direct raw I2S helper.
static void pump(uint32_t ms, uint8_t volume) {
  uint32_t left = (uint32_t)((uint64_t)ms * GB_RATE / 1000);
  while (left) {
    size_t n = left > 256 ? 256 : left;
    gSyn.render(mono, n, volume);
    for (size_t i = 0; i < n; i++) { buf[i * 2] = mono[i]; buf[i * 2 + 1] = mono[i]; }
    i2s.write((uint8_t *)buf, n * 4);
    left -= n;
  }
}

static bool audioLock(uint32_t waitMs = 250) {
  return gAudioMutex && xSemaphoreTake(gAudioMutex, pdMS_TO_TICKS(waitMs)) == pdTRUE;
}
static void audioUnlock() {
  if (gAudioMutex) xSemaphoreGive(gAudioMutex);
}

static void audioTask(void *) {
  uint8_t id;
  // where each music channel has got to, and when its current note ends
  uint16_t mi1 = 0, mi2 = 0;
  uint32_t at1 = 0, at2 = 0, clock = 0;
  uint8_t playing = MUS_NONE;

  for (;;) {
    // Game audio and the little handheld button beep are independent. Turning
    // off game sound must not silence button feedback, and turning button
    // sound off must not mute battle/music effects. If game sound is muted,
    // stop any melody envelope that was already active; a button beep can then
    // start cleanly from the queue below.
    if (!gReady || !gOn || !gScreenAwake) gSyn.allOff();
    uint8_t m = (gOn && gReady && gScreenAwake) ? gMusic : MUS_NONE;
    bool wantAudio = (m != MUS_NONE) || gSyn.busy();
    if (!gReady) { m = MUS_NONE; wantAudio = false; }

    // An effect always wins the melody voice. SFX_TAP is mixed at the separate
    // button volume; every other cue follows the game-sound setting/volume.
    if (xQueueReceive(gQ, &id, wantAudio ? 0 : pdMS_TO_TICKS(40)) == pdTRUE) {
      // Settings self-test is deliberately forced even when sound toggles are
      // off. It verifies I2S -> ES8311 -> PA -> speaker as one path.
      if (id == AUDIO_SELFTEST_ID) {
        if (gReady && audioLock(500)) {
          if (!gAmpOn) {
            digitalWrite(PA, HIGH);
            delay(85);
            gAmpOn = true;
            playRawSquare(0, 20, 8);
          }
          playAudioSelfTest();
          gAmpHoldUntil = millis() + 900;
          audioUnlock();
        }
        continue;
      }

      bool isButton = (id == SFX_TAP);
      bool allowed = gReady && gScreenAwake && id < SFX_COUNT && (isButton ? gBtnOn : gOn);
      uint8_t effectVol = isButton ? gBtnVol : gVol;
      if (allowed && effectVol && audioLock(500)) {
        if (!gAmpOn) {
          digitalWrite(PA, HIGH);
          // The board's PA needs real wake time. MISS was audible because DENY
          // is long, while short GOOD/PERFECT cues could end before the amp was
          // fully awake. Give game SFX a smaller but still safe warm-up too.
          delay(isButton ? 85 : 65);
          gAmpOn = true;
          playRawSquare(0, isButton ? 20 : 18, effectVol);
        }

        if (isButton) {
          gSyn.allOff();
          playUiChirpDirect(effectVol);
          gAmpHoldUntil = millis() + 1200;
        } else {
          const SfxDef &d = SFX[id];
          for (uint8_t i = 0; i < d.len; i++) {
            uint16_t f = hzToGb(d.n[i].f);
            if (id == SFX_HIT || id == SFX_FAINT)
              gSyn.noise(13, -1, 2, d.n[i].ms, (uint16_t)(4 + i * 4));
            else if (f)
              gSyn.note(0, f, 1, 14, -1, 4, d.n[i].ms);
            else
              gSyn.silence(0);
            pump(d.n[i].ms, effectVol);
          }
          // Keep the PA awake across the defence game's 520 ms feedback flash
          // so the next result tone does not pay the cold-start penalty again.
          gAmpHoldUntil = millis() + 900;
        }
        audioUnlock();
      }
      continue;
    }

    if (m == MUS_NONE) {
      // v3.62.9: when a battle/result screen hands control back to a hub, stop
      // any note envelope from the previous music immediately. Previously
      // gMusic changed to NONE but the synth voice could keep sounding until
      // its current note/envelope naturally expired, which made boss BGM appear
      // to leak into the boss menu.
      if (playing != MUS_NONE) gSyn.allOff();
      // Do not shut the PA down immediately after a UI click. Keeping it warm
      // briefly is what makes rapid consecutive presses audible on this board.
      if (gAmpOn && !gSyn.busy() && (int32_t)(millis() - gAmpHoldUntil) >= 0 && audioLock(20)) {
        if (gAmpOn && !gSyn.busy() && (int32_t)(millis() - gAmpHoldUntil) >= 0) {
          digitalWrite(PA, LOW);
          gAmpOn = false;
        }
        audioUnlock();
      }
      playing = MUS_NONE; mi1 = mi2 = 0; at1 = at2 = clock = 0;
      continue;
    }

    if (m != playing) { playing = m; mi1 = mi2 = 0; at1 = at2 = clock = 0; }

    // 0 = the gym leader battle, 3 = the victory fanfare. Winning used to play
    // index 2, which is the WILD BATTLE theme -- so a win sounded like the fight
    // had started again.
    const MusicTrack &t = MUSIC_TBL[(m == MUS_VICTORY) ? 3 : 0];
    // start whichever channel is due
    if (clock >= at1) {
      if (mi1 >= t.n1) {                       // loop, or stop a one-shot
        if (m == MUS_VICTORY) { gMusic = MUS_NONE; continue; }
        mi1 = 0; at1 = clock;
      }
      const MusicNote &n = t.ch1[mi1++];
      if (n.freq) gSyn.note(0, n.freq, n.duty, n.vol, n.envDir, n.envPeriod, n.ms);
      else gSyn.silence(0);
      at1 = clock + n.ms;
    }
    if (clock >= at2) {
      if (mi2 >= t.n2) { mi2 = 0; at2 = clock; }
      const MusicNote &n = t.ch2[mi2++];
      if (n.freq) gSyn.note(1, n.freq, n.duty, n.vol, n.envDir, n.envPeriod, n.ms);
      else gSyn.silence(1);
      at2 = clock + n.ms;
    }
    uint32_t next = (at1 < at2) ? at1 : at2;
    if (next <= clock) next = clock + 5;
    uint32_t step = next - clock;
    if (step > 60) step = 60;                  // stay responsive to an effect
    if (audioLock(120)) {
      if (!gAmpOn) { digitalWrite(PA, HIGH); delay(6); gAmpOn = true; }
      pump(step, gVol);
      audioUnlock();
      clock += step;
    } else {
      // A direct UI chirp owns the I2S bus briefly. Do not advance the music
      // clock until audio was actually rendered; this avoids skipped notes.
      delay(1);
    }
  }
}

void audioMusic(uint8_t id) { gMusic = (id < 3) ? id : MUS_NONE; }

void audioSetVolume(uint8_t v) {
  gVol = v > 10 ? 10 : v;
  Preferences p;
  p.begin("tamapoke", false);
  p.putUChar("vol", gVol);
  p.end();
}
uint8_t audioVolume() { return gVol; }

void audioBegin() {
  // I2S primero: arranca el MCLK que necesita el códec para engancharse
  pinMode(PA, OUTPUT);
  digitalWrite(PA, LOW);   // amp apagado; la tarea lo enciende al reproducir

  i2s.setPins(I2S_BCK_IO, I2S_WS_IO, I2S_DO_IO, I2S_DI_IO, I2S_MCK_IO);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("[audio] I2S begin failed");
    return;
  }
  Serial.println("[audio] I2S OK: 16kHz stereo, MCLK=GPIO42, PA=GPIO46");
  if (!es8311Init()) { Serial.println("[audio] ES8311 not responding"); return; }
  Serial.println("[audio] ES8311 OK");

  Preferences p;
  p.begin("tamapoke", true);
  gOn = p.getBool("snd", true);
  gVol = p.getUChar("vol", 7);
  if (gVol > 10) gVol = 7;
  gBtnOn = p.getBool("btnsnd", true);
  gBtnVol = p.getUChar("btnvol", 8);
  if (gBtnVol > 10) gBtnVol = 8;
  bool btnFix33Done = p.getBool("btnfix33", false);
  bool btnFix37Done = p.getBool("btnfix37", false);
  bool btnFix38Done = p.getBool("btnfix38", false);
  bool btnFix39Done = p.getBool("btnfix39", false);
  p.end();

  // Older button-sound builds could persist a silent/zero setting while the
  // actual game audio remained healthy. Repair it ONCE on first v3.33 boot;
  // after that the user's own setting is respected normally.
  if (!btnFix33Done) {
    gBtnOn = true;
    gBtnVol = 8;
    Preferences fix;
    fix.begin("tamapoke", false);
    fix.putBool("btnsnd", true);
    fix.putUChar("btnvol", 8);
    fix.putBool("btnfix33", true);
    fix.end();
  }

  if (!btnFix37Done) {
    // One-time recovery for devices upgraded through the experimental v3.33-
    // v3.36 button-audio builds. The user asked for click feedback on every UI
    // press, so ensure the new direct path is actually enabled once after the
    // update. From then on the normal settings are respected.
    gBtnOn = true;
    gBtnVol = 8;
    Preferences fix;
    fix.begin("tamapoke", false);
    fix.putBool("btnsnd", true);
    fix.putUChar("btnvol", 8);
    fix.putBool("btnfix37", true);
    fix.end();
  }

  if (!btnFix38Done) {
    // v3.38 re-enables button feedback once so an old silent preference cannot
    // hide the PA warm-up fix during the first verification boot.
    gBtnOn = true;
    gBtnVol = 9;
    Preferences fix;
    fix.begin("tamapoke", false);
    fix.putBool("btnsnd", true);
    fix.putUChar("btnvol", 9);
    fix.putBool("btnfix38", true);
    fix.end();
  }


  if (!btnFix39Done) {
    // v3.39 moves normal UI feedback out of the queue and into the touch path.
    // Enable it once so upgrades from any experimental button-audio build can
    // verify the new route immediately. Later boots respect the user's choice.
    gBtnOn = true;
    gBtnVol = 9;
    Preferences fix;
    fix.begin("tamapoke", false);
    fix.putBool("btnsnd", true);
    fix.putUChar("btnvol", 9);
    fix.putBool("btnfix39", true);
    fix.end();
  }

  gReady = true;
  Serial.printf("[audio] ready game=%d vol=%u button=%d btnvol=%u\n",
                gOn ? 1 : 0, gVol, gBtnOn ? 1 : 0, gBtnVol);
  gAudioMutex = xSemaphoreCreateMutex();
  gQ = xQueueCreate(24, sizeof(uint8_t));
  if (!gAudioMutex) Serial.println("[audio] mutex allocation failed; UI beep unavailable");
  if (!gQ) Serial.println("[audio] queue allocation failed; game SFX disabled, direct UI beep still available");
  if (gQ) {
    BaseType_t ok = xTaskCreatePinnedToCore(audioTask, "audio", 5120, nullptr, 1, &gAudioTaskHandle, 0);
    if (ok != pdPASS) {
      Serial.println("[audio] task creation failed; direct UI beep still available");
      vQueueDelete(gQ);
      gQ = nullptr;
    }
  }
  if (gQ) sfxPlay(SFX_HATCH);  // startup cue when background audio task is alive
}

void audioUiGestureStart() {
  // Called immediately before the normal UI tap handler. While it is true,
  // old per-screen sfxPlay(SFX_TAP) calls are muted so the common resolver can
  // emit exactly one tactile chirp after the action completes. A boolean is
  // safer than a time debounce: it cannot swallow the first hit of a minigame
  // that starts immediately after a menu tap.
  gUiTapResolving = true;
}

void audioUiPress() {
  // v3.39: NORMAL UI feedback bypasses the queue completely. The touch handler
  // owns one short, serialized I2S write so a delayed/full worker queue can no
  // longer make the button silent. A mutex keeps the game-audio task from
  // writing the codec at the same time.
  gUiTapResolving = false;
  if (!gReady || !gScreenAwake || !gBtnOn || !gBtnVol || !gAudioMutex) return;
  if (!audioLock(350)) {
    Serial.println("[audio] UI direct beep skipped: I2S busy");
    return;
  }

  gSyn.allOff();
  if (!gAmpOn) {
    digitalWrite(PA, HIGH);
    delay(90);                  // cold-start PA wake; known-good self-test margin
    gAmpOn = true;
    playRawSquare(0, 24, gBtnVol);
  }
  // Slightly shorter than the v3.38 worker chirp so direct feedback does not
  // make the interface feel sticky, but long enough to remain audible.
  playRawSquare(720, 58, gBtnVol);
  playRawSquare(0, 8, gBtnVol);
  playRawSquare(1020, 68, gBtnVol);
  gAmpHoldUntil = millis() + 1800;  // rapid taps reuse the already-awake PA
  audioUnlock();
}

void sfxPlay(uint8_t id) {
  if (!gReady || !gScreenAwake || !gQ || id >= SFX_COUNT) return;
  bool allowed = (id == SFX_TAP) ? gBtnOn : gOn;
  if (!allowed) return;

  if (id == SFX_TAP) {
    // During normal UI resolution the shared handler owns the tactile chirp.
    // Outside that scope (for example a minigame hit using SFX_TAP) the legacy
    // effect remains fully available and is never blocked by a time debounce.
    if (gUiTapResolving) return;
    if (xQueueSendToFront(gQ, &id, 0) != pdTRUE) {
      uint8_t stale;
      xQueueReceive(gQ, &stale, 0);
      xQueueSendToFront(gQ, &id, 0);
    }
    return;
  }
  xQueueSend(gQ, &id, 0);  // non-UI cues may be dropped only if the queue is full
}

bool audioReady() { return gReady; }

bool audioSelfTest() {
  if (!gReady) return false;
  if (!gQ) {
    if (!audioLock(500)) return false;
    if (!gAmpOn) { digitalWrite(PA, HIGH); delay(90); gAmpOn = true; playRawSquare(0, 24, 8); }
    playAudioSelfTest();
    gAmpHoldUntil = millis() + 1200;
    audioUnlock();
    return true;
  }
  uint8_t id = AUDIO_SELFTEST_ID;
  if (xQueueSendToFront(gQ, &id, 0) == pdTRUE) return true;
  uint8_t stale;
  xQueueReceive(gQ, &stale, 0);
  return xQueueSendToFront(gQ, &id, 0) == pdTRUE;
}

void audioSetButtonEnabled(bool on) {
  gBtnOn = on;
  Preferences p;
  p.begin("tamapoke", false);
  p.putBool("btnsnd", on);
  p.end();
}
bool audioButtonEnabled() { return gBtnOn; }

void audioSetButtonVolume(uint8_t v) {
  gBtnVol = v > 10 ? 10 : v;
  Preferences p;
  p.begin("tamapoke", false);
  p.putUChar("btnvol", gBtnVol);
  p.end();
}
uint8_t audioButtonVolume() { return gBtnVol; }

void audioSetScreenAwake(bool awake) {
  gScreenAwake = awake;
  if (awake) return;
  // Screen-off is an energy state, not a user mute preference. Drop queued
  // notifications/effects and power the PA down immediately; music state is
  // preserved and may resume after the physical-button wake.
  if (gQ) xQueueReset(gQ);
  if (gAudioMutex && audioLock(80)) {
    gSyn.allOff();
    if (gAmpOn) { digitalWrite(PA, LOW); gAmpOn = false; }
    gAmpHoldUntil = 0;
    audioUnlock();
  }
}
bool audioScreenAwake() { return gScreenAwake; }

void audioSetEnabled(bool on) {
  gOn = on;
  Preferences p;
  p.begin("tamapoke", false);
  p.putBool("snd", on);
  p.end();
}
bool audioEnabled() { return gOn; }
