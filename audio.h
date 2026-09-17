#pragma once
#include <stdint.h>

// Efectos de sonido del juego (cola, no bloqueante). El orden coincide con la
// tabla SFX de audio.cpp.
enum Sfx : uint8_t {
  SFX_TAP = 0,  // tocar / boton
  SFX_EAT,      // comer
  SFX_PLAY,     // punto del minijuego / golpe
  SFX_HEART,    // le gusta / mimo
  SFX_HATCH,    // eclosion
  SFX_EVOLVE,   // evolucion
  SFX_MEDAL,    // medalla / hito
  SFX_DENY,     // accion no permitida
  SFX_BYE,      // despedida
  SFX_LEVEL,    // sube de nivel
  // battle cues: a fight in silence is what made it feel flat
  SFX_HIT,      // a physical move landing
  SFX_BEAM,     // a special move
  SFX_STATUS,   // a status move or an ailment taking hold
  SFX_SUPER,    // super effective
  SFX_FAINT,    // something goes down
  SFX_VICTORY,  // the fight is won
  SFX_COUNT
};

// Background music. One square-wave voice and a single blocking audio task, so
// this is not mixed with the effects -- the task plays the tune a note at a
// time and hands the voice to any effect that arrives, resuming after. An
// effect therefore always cuts through, which is what you want anyway.
enum Music : uint8_t { MUS_NONE = 0, MUS_BATTLE, MUS_VICTORY };
void audioMusic(uint8_t id);

// 0..10. Stored, and applied as the square wave's amplitude; 0 is silence
// without disabling the system, which is what a mute expects to do.
void audioSetVolume(uint8_t v);
uint8_t audioVolume();

// Button click sound is independent from game SFX/music. This gives the device
// the short electronic feedback of a handheld virtual pet without forcing the
// rest of the game audio to the same volume. Settings persist in NVS.
void audioSetButtonEnabled(bool on);
bool audioButtonEnabled();
void audioSetButtonVolume(uint8_t v);
uint8_t audioButtonVolume();

void audioBegin();          // init ES8311 + I2S + amplificador + tarea de audio
bool audioReady();         // I2S + codec initialization succeeded
bool audioSelfTest();      // forced 3-tone speaker-path diagnostic
void audioUiGestureStart(); // marks a normal UI press so legacy per-button TAP calls are suppressed
void audioUiPress();       // v3.39 direct serialized I2S chirp after the resolved tap/action
void sfxPlay(uint8_t id);   // encola un efecto (no bloquea el loop)
// Transient display-power gate. When false, queued/automatic game SFX are
// suppressed and the PA is powered down without changing persistent sound prefs.
void audioSetScreenAwake(bool awake);
bool audioScreenAwake();
void audioSetEnabled(bool on);
bool audioEnabled();
