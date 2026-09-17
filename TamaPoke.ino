// TamaPoke - tamagotchi pixel art inspirado en la gen 1
// para Waveshare ESP32-S3-Touch-AMOLED-1.75
//
// Librerias (Library Manager o repo de Waveshare):
//   - "GFX Library for Arduino" (moononournation), con soporte CO5300 QSPI
//   - "SensorLib" (Lewis He), driver tactil CST9217
//
// Placa: ESP32S3 Dev Module | Flash 16MB | PSRAM: OPI PSRAM | USB CDC On Boot: Enabled
//
// Los sprites y la tabla de especies se generan con tools/sprites.py (emit).

#include <Arduino.h>
#include <Wire.h>
#include <SD_MMC.h>
#ifndef TAMAPOKE_EMU
#include <U8g2lib.h>
#endif
#include "Arduino_GFX_Library.h"
#include "TouchDrvCSTXXX.hpp"
#include "pin_config.h"
#include "species.h"
#include "dex.h"
#include "types.h"
#include "moves.h"
#include "battle.h"
#include "trainers.h"
#include "link.h"
#include "linknow.h"
#include "backs.h"
#include "badges.h"
#include "avatars.h"
#include "noart.h"
#include <stdarg.h>
#include <string.h>
#include "party.h"
#include "save.h"
#include "pet.h"
#include "digimon.h"
#include "care_slots.h"
#include "sdmon.h"
#include "rtcbat.h"
#include "i18n.h"
#include "audio.h"
#include "game_extras.h"
#include "personality.h"

// Version del firmware. Subir este numero en cada release (y manifest.json para
// el instalador web). Se muestra en la pantalla de ajustes y por serie al arrancar.
#define FW_VERSION "3.89.0"
// Set to 1 only for a connected USB soak test. Serial printf can itself cause
// a visible hitch, so normal builds keep frame diagnostics completely off.
#define TAMAPOKE_FRAME_DIAG 0

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *panel = new Arduino_CO5300(
  bus, LCD_RESET, 0 /*rotation*/, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);
// Framebuffer completo en PSRAM: dibujamos todo y hacemos flush() (sin parpadeo)

Arduino_Canvas *gfx = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, panel);

// v3.61.3 render path ---------------------------------------------------------
// Arduino_Canvas already owns a complete RGB565 framebuffer in PSRAM. PMD pixel
// art is by far the hottest drawing path after the full catalog expansion, so
// write solid zoomed runs straight into that framebuffer instead of calling
// fillRect() thousands of times per sprite frame. This does NOT change the
// visual output or animation cadence; it only removes virtual-call/clipping
// overhead from each source pixel run.
static inline void canvasFillRectFast(int x, int y, int w, int h, uint16_t col) {
  if (w <= 0 || h <= 0) return;
  int x0 = x < 0 ? 0 : x;
  int y0 = y < 0 ? 0 : y;
  int x1 = x + w; if (x1 > LCD_WIDTH) x1 = LCD_WIDTH;
  int y1 = y + h; if (y1 > LCD_HEIGHT) y1 = LCD_HEIGHT;
  if (x0 >= x1 || y0 >= y1) return;
  uint16_t *fb = gfx->getFramebuffer();
  if (!fb) { gfx->fillRect(x0, y0, x1 - x0, y1 - y0, col); return; }
  const int ww = x1 - x0;
  uint16_t *row0 = fb + (uint32_t)y0 * LCD_WIDTH + x0;
  // Fill one row, then duplicate it for the zoomed vertical run. Most PMD
  // rectangles are s pixels tall, so this turns repeated PSRAM scalar stores
  // into optimized memcpy() calls without changing a single output pixel.
  for (int xx = 0; xx < ww; ++xx) row0[xx] = col;
  const size_t rowBytes = (size_t)ww * sizeof(uint16_t);
  for (int yy = y0 + 1; yy < y1; ++yy) {
    uint16_t *dst = fb + (uint32_t)yy * LCD_WIDTH + x0;
    memcpy(dst, row0, rowBytes);
  }
}

// Present a full-width horizontal band from the completed Canvas. A full Canvas
// flush transfers 466x466 every frame even when the lower care controls have not
// changed. On the animated home/gallery paths we can safely send only the moving
// band while still drawing the whole frame off-screen first, so no half-drawn
// image is ever visible on the AMOLED.
static inline void canvasPresentBand(int y, int h) {
  if (y < 0) { h += y; y = 0; }
  if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
  if (h <= 0) return;
  if (y == 0 && h == LCD_HEIGHT) { gfx->flush(); return; }
  uint16_t *fb = gfx->getFramebuffer();
  if (!fb) { gfx->flush(); return; }
  panel->draw16bitRGBBitmap(0, y, fb + (uint32_t)y * LCD_WIDTH, LCD_WIDTH, h);
}

// Korean UI uses Arduino_GFX's bundled QuanPixel CJK font. It is an 8x8 pixel
// font, so it preserves the TamaPoke pixel-art look while covering Hangul.
static void applyLanguageFont() {
#ifndef TAMAPOKE_EMU
  if (gLang == LANG_KO) {
    gfx->setUTF8Print(true);
    gfx->setFont(u8g2_font_quan7_h_cjk);
  } else {
    gfx->setUTF8Print(false);
    gfx->setFont((const GFXfont *)NULL);
  }
#endif
}

// Arduino_GFX's built-in 5x7 font treats cursor Y as the top of the text cell,
// while U8g2 fonts use cursor Y as the glyph baseline. TamaPoke's UI coordinates
// were authored for the built-in font, so Hangul otherwise appears one font
// height too high. QuanPixel also has a small left-side bearing relative to the
// legacy cell. Track the active scale and compensate both axes only in Korean:
//   Y: +7 px per text-size step (baseline -> top-left coordinates)
//   X: +1 px per text-size step (visual left-edge alignment)
// Other languages keep their original coordinates byte-for-byte.
#define KO_FONT_X_BIAS 1
#define KO_FONT_BASELINE_Y 7
static uint8_t gUiTextScale = 1;
static void uiSetTextSize(uint8_t s) {
  if (s == 0) s = 1;
  gUiTextScale = s;
  gfx->setTextSize(s);
}
static void uiSetCursor(int16_t x, int16_t y) {
#ifndef TAMAPOKE_EMU
  if (gLang == LANG_KO) {
    x += (int16_t)(KO_FONT_X_BIAS * gUiTextScale);
    y += (int16_t)(KO_FONT_BASELINE_Y * gUiTextScale);
  }
#endif
  gfx->setCursor(x, y);
}

static uint16_t utf8GlyphCount(const char *s) {
  uint16_t n = 0;
  if (!s) return 0;
  for (const uint8_t *p = (const uint8_t *)s; *p; ++p) {
    if ((*p & 0xC0) != 0x80) ++n;
  }
  return n;
}

// Centered Korean labels used to be measured as 8 px for *every* UTF-8 glyph.
// That overestimates ASCII embedded in Korean UI ("KO >", "Lv.1", times,
// numbers), pulling mixed strings visibly to the left. QuanPixel's Hangul cell is
// 8 px wide while the legacy-style ASCII advance is approximately 6 px, so
// measure each UTF-8 codepoint by class instead of counting all glyphs equally.
static int uiTextWidth(const char *s, uint8_t size) {
  if (!s) return 0;
  int units = 0;
  const uint8_t *p = (const uint8_t *)s;
  while (*p) {
    if (*p < 0x80) {
      units += 6;
      ++p;
    } else {
      units += (gLang == LANG_KO ? 8 : 6);
      ++p;
      while ((*p & 0xC0) == 0x80) ++p;
    }
  }
  return units * size;
}
static int uiTextHalfWidth(const char *s, uint8_t size) { return uiTextWidth(s, size) / 2; }

// Prefer a more readable label size, but never let a single-line UI string run
// outside its box.  The display font only supports integer scaling, so fitting
// means stepping down one size at a time.  Callers provide the real usable
// width (after padding/icons/counters), which keeps Korean names and translated
// strings from being clipped when the accessibility-sized text is enabled.
static uint8_t uiFitTextSize(const char *s, uint8_t preferred,
                             uint8_t minimum, int16_t maxWidth) {
  if (preferred < minimum) preferred = minimum;
  while (preferred > minimum && uiTextWidth(s, preferred) > maxWidth) --preferred;
  return preferred;
}

static uint8_t uiDrawCenteredFit(const char *s, int16_t centerX, int16_t y,
                                 int16_t maxWidth, uint8_t preferred,
                                 uint8_t minimum = 1) {
  uint8_t size = uiFitTextSize(s, preferred, minimum, maxWidth);
  uiSetTextSize(size);
  uiSetCursor(centerX - uiTextHalfWidth(s, size), y);
  gfx->print(s);
  return size;
}

static uint8_t uiDrawLeftFit(const char *s, int16_t x, int16_t y,
                             int16_t maxWidth, uint8_t preferred,
                             uint8_t minimum = 1) {
  uint8_t size = uiFitTextSize(s, preferred, minimum, maxWidth);
  uiSetTextSize(size);
  uiSetCursor(x, y);
  gfx->print(s);
  return size;
}

TouchDrvCST92xx touch;
Pet pet;

// sprite animado de la SD para la especie actual (si existe el archivo)
SdMon mon;          // sprite B/N (respaldo y minijuego si no hay PMD)
PmdMon pmd;         // sprite PMD multi-accion (pantalla principal)
PmdMon evoPmd;      // forma anterior, solo durante el parpadeo de evolucion
int16_t monFor = -2;
bool monShinyFor = false;
static bool drawDigiFrameCentered(uint16_t spriteId,uint8_t frame,int centerX,
                                  int groundY,int scale,bool flip,bool silhouette);

// comportamiento del bicho en pantalla
struct {
  uint8_t mode = 0;     // 0 idle, 1 paseo, 2 gesto one-shot
  uint8_t act = PMD_IDLE;
  uint32_t t0 = 0;      // inicio de la animacion en curso
  uint32_t until = 0;   // fin del estado actual
  float x = 233, targetX = 233;
} beh;
#define PET_GROUND 304  // linea de suelo de la mascota
PmdMon galleryPmd;  // sprite grande de la vista detalle de la galeria (PMD/TPK2+TPK3, legal)

// galeria pokedex
bool galleryOpen = false;
bool galleryDirty = false;
// 16 to a page, and the Pokedex is browsed ONE REGION AT A TIME. Three
// generations flat is 25 pages of swiping to reach Hoenn, which is not a
// Pokedex, it is a scroll. A vertical swipe changes region and a horizontal one
// pages within it, so nothing is ever more than ten pages from the front.
// ALL is deliberately not offered here -- it is the thing being replaced.
#define GAL_PER_PAGE 16
#define GAL_REGIONS (REGION_COUNT - 1)          // the real regions, not ALL
#define GAL_SPAN (regionDexCount(galleryRegion % GAL_REGIONS))
#define GAL_PAGES ((GAL_SPAN + GAL_PER_PAGE - 1) / GAL_PER_PAGE)
uint8_t galleryRegion = 0;
static int16_t gGalleryPanelDex = -1;  // v3.61.3 partial-present validity
// Both the Pokedex and the gym ladder now open on a REGION CHOOSER rather than
// dropping you into whichever region was last viewed. The vertical swipe that
// changes region still works, but it is invisible, so on its own it meant the
// Johto and Hoenn content looked absent.
bool galleryPick = false;
bool gymPick = false;
uint8_t rpickPage = 0;      // the region chooser is paged; shared by all 3 modes
int galleryPage = 0;        // GAL_PAGES paginas de GAL_PER_PAGE
int16_t galleryDetail = 0;  // dex en vista detalle, 0 = rejilla

bool screenOff = false;       // pulsacion corta del boton PWR
bool cardOpen = false;        // ficha del bicho (deslizar vertical)
bool kbOpen = false;
enum : uint8_t { KB_PET = 0, KB_TRAINER };
uint8_t kbTarget = KB_PET;          // teclado para renombrar al bicho
char nameBuf[12] = "";
uint8_t nameLen = 0;
#define CARD_PAGES 4   // profile, stats, moves, progress -- medals moved to
                       // the player card, where the totals already live
uint8_t cardPage = 0;         // 0 perfil, 1 stats+medallas
// Menu overlay: opened by tapping the pet's name on the main screen. The
// horizontal swipe is already taken by the Pokedex (it pages through 10 pages
// internally), so a hub on that axis would be ambiguous; the header was inert
// and is the only free surface left.
bool menuOpen = false;
bool digiOpen = false;
bool digiDexOpen = false;
uint8_t digiDexPage = 0;
int16_t digiDexDetail = -1;
void renderDigi();
void digiTap(int16_t x, int16_t y);
void renderDigiDex();
void digiDexTap(int16_t x, int16_t y);
// v3.17 expands the home menu with daily missions and Battle Tower. Seven
// compact rows still fit safely inside the 466px round bezel.
#define MENU_X 78
#define MENU_Y 42
#define MENU_W 310
#define MENU_H 382
#define MENU_ROW_H 39
#define MENU_ROW_GAP 3
#define MENU_ROWS 8
#define MENU_ROW_Y(i) (MENU_Y + 12 + (i) * (MENU_ROW_H + MENU_ROW_GAP))

// Korean-only v3.18 inventory flow. The FOOD icon first opens the classic
// six-icon quick selector: four original foods, earned growth items, and TMs.
// Only the last two enter a full-screen list, so one tap of actual food always
// returns to the pet and makes the eating animation visible.
bool bagOpen = false;
uint8_t bagTab = 0;       // 0 earned growth items, 1 type TMs
uint8_t bagPage = 0;
uint8_t tmPendingType = T_NONE;
uint8_t tmPendingMove = 0;
bool missionOpen = false;
bool adventureOpen = false;
bool exploreOpen = false;
uint8_t explorePage = 0;
bool bossOpen = false;
bool towerOpen = false;
bool treasureOpen = false;
bool rivalOpen = false;

// Party screen. partyPick != 0 means the newcomer needs a slot: the player
// either taps someone to replace or lets it go.
bool partyOpen = false;
bool partyPick = false;
uint8_t partyDetail = 0;   // 0 = the grid, else slot + 1
// The box, reached from the party screen. `boxSwapFrom` is the party slot
// waiting for something to trade with, 0 when nothing is pending.
bool boxOpen = false;
uint8_t boxPage = 0;
uint8_t boxSwapFrom = 0;   // party slot + 1, armed from the party side
uint8_t boxSel = 0;        // box slot + 1, armed from the box side
// The box gets the same detail sheet the party has. Tapping a box slot used to
// yank the creature into the party immediately, which is a surprising amount to
// happen from one tap -- and left nowhere to put a RELEASE button.
uint8_t boxDetail = 0;        // box slot + 1 whose sheet is open
// The RELEASE confirm, on whichever sheet is up. One flag rather than two,
// because only one sheet can be open at a time and partyDetail/boxDetail
// already say which.
bool releaseConfirm = false;
#define BOX_PER_PAGE 6
uint32_t partyBannerUntil = 0;   // "<name> joined the party!"
char partyBannerName[32] = "";
#define PARTY_CELL_W 150
#define PARTY_CELL_H 70
#define PARTY_GRID_X 78
#define PARTY_GRID_Y 88

bool clockOpen = false;       // pantalla de ajuste de hora (deslizar abajo)
int clockH = 12, clockM = 0;  // hora en edicion

// escena de bano: espuma sobre el bicho y limpieza al reventar
uint32_t bathUntil = 0;
bool bathPending = false;
struct { int16_t x, y; uint8_t r, ph; } bubbles[14];
uint32_t feedMenuUntil = 0;   // selector de comida abierto hasta este millis

// 방어 훈련: 게이지 타이밍 게임. 바늘이 좌우로 왕복하고 중앙의 방어
// 구간에 들어왔을 때 화면을 한 번 터치한다. 12라운드 고정이라 규칙이
// 단순하고, SD 스프라이트/물리/충돌 계산을 전혀 사용하지 않아 프레임
// 드랍이나 멈춤 가능성을 최소화한다. PERFECT=3, GOOD=2, BLOCK=1점.
bool gameOpen = false;
uint32_t gameOverUntil = 0;           // non-zero = result screen
uint16_t gameScore = 0;
uint8_t gameGain = 0;
bool gameNewHi = false;
uint8_t gameIvReward = XITEM_COUNT;
uint8_t gameIvRewardCount = 0;
bool gameShinyBerryReward = false;
uint8_t defRound = 0;                 // completed attempts, 0..12
uint16_t defBlocks = 0, defGoods = 0, defPerfects = 0;
uint32_t defRoundStarted = 0;
uint32_t defFeedbackUntil = 0;
uint8_t defFeedback = 0;              // 1 BLOCK, 2 GOOD, 3 PERFECT, 4 MISS
int16_t defStoppedX = 233;             // screen center; CX is defined later
#define DEF_GAUGE_X 66
#define DEF_GAUGE_W 334
#define DEF_GAUGE_Y 246
#define DEF_GAUGE_H 38
#define DEF_ROUNDS 12
#define DEF_FEEDBACK_MS 520

// saco de entrenamiento (entrena la fuerza)
bool sackOpen = false;
uint32_t sackUntil = 0, sackOverUntil = 0;
enum DigiAnimKind : uint8_t { DIGI_ANIM_NONE, DIGI_ANIM_TOUCH, DIGI_ANIM_TRAIN };
static uint8_t digiActionKind = DIGI_ANIM_NONE;
static uint32_t digiActionUntil = 0;
static void digiReact(uint8_t kind,uint32_t duration=1800){if(pet.currentIsDigimon()){digiActionKind=kind;digiActionUntil=millis()+duration;}}
uint16_t sackHits = 0;
float sackShake = 0;
uint8_t sackGain = 0;
bool sackNewHi = false;
uint8_t sackIvReward = XITEM_COUNT;
uint8_t sackIvRewardCount = 0;
bool sackShinyBerryReward = false;

// training submenu (the 5th icon): routes to the trainer for each stat.
// ATK = punching bag, SPEED = lightning targets, DEF = timing gauge.
bool trainOpen = false;

// v3.62.9 training persistence guard. Training results can touch the pet save,
// two daily missions, a guaranteed IV berry and a rare Shiny Berry at once. Writing all of those NVS blobs
// back-to-back inside the minigame render/touch path can stall flash long enough
// to look like (or become) a reset. Keep gameplay state in RAM, then commit the
// pet and extras on separate loop passes after the minigame has closed.
uint8_t trainingPersistPhase = 0;
uint32_t trainingPersistAfter = 0;

static void queueTrainingPersist() {
  trainingPersistPhase = 1;
  trainingPersistAfter = millis() + 250UL;
}

// move picker, opened from the MOVES card page. Most learnsets are level 0, so
// a level-up "you learned a move" prompt would almost never fire -- the moveset
// is edited on demand instead.
bool movePickOpen = false;
uint8_t movePickSlot = 0;   // which of the 4 slots is being replaced
uint8_t movePickParty = 0;  // 0 = the live pet, else the party slot + 1
uint8_t movePickPage = 0;
#define MOVE_ROW_Y(i) (96 + (i) * 58)
#define MOVE_PICK_PER_PAGE 5
#define MOVE_PICK_Y(i) (76 + (i) * 58)
// level-up learn prompt: modal, and deliberately without a timeout -- it
// decides what the creature is for the rest of its life, and once banked into
// the party, forever.
#define LEARN_ROW_Y(i) (104 + (i) * 56)
#define LEARN_SKIP_Y 334

// ---------- battle ----------
// The move menu is a 2x2 grid rather than four stacked rows: the round panel
// has to fit both creatures, both HP bars and the menu, and four full-width
// rows do not leave room for the sprites.
bool battleOpen = false;
bool btlTower = false;       // battle belongs to the endless Battle Tower
bool btlBoss = false;        // three-live-care-slot type boss
bool btlRival = false;       // persistent Korean rival trainer
uint8_t btlBossType = T_NORMAL;
bool btlBossPhase2 = false;  // v3.25 boss enrages below half HP
uint8_t btlWeather = WEATHER_CLEAR;
uint8_t btlRivalSpecial = RIVSPEC_NONE;

enum : uint8_t {
  SCR_STARTER = 0, SCR_REGION, SCR_GALLERY, SCR_DEXPICK, SCR_MOVEPICK, SCR_BOX,
  SCR_PARTY, SCR_KEYBOARD, SCR_CARD, SCR_PLAYER, SCR_CLOCK, SCR_GYM, SCR_GYMPICK,
  SCR_LAN, SCR_PICK, SCR_BATTLE, SCR_WIN, SCR_LEARN, SCR_TRAIN, SCR_MENU,
  SCR_GAME, SCR_MAIN, SCR_COUNT
};
extern const char *const SCREEN_NAME[SCR_COUNT];   // const is internal linkage in C++

// Declared HERE, above every use. tools/emu/build.sh generates a proto.h with
// every prototype at the top, so the emulator compiled these happily while
// arduino-cli -- which relies on the IDE's auto-prototyping and does not always
// produce one -- did not. A green emulator build is not proof the firmware
// builds; only arduino-cli is.
void bootReport();
uint8_t uiCurrentScreen();
void invalidateHomePresent();
// Declared here because renderMonSheet() and the sheet's tap handlers use both
// ~4000 lines above where they are defined. The emulator generates a proto.h and
// would compile it either way; arduino-cli would not, and that has shipped once.
void uiConfirmRects(int *b1Top, int *b1Bot, int *b2Top, int *b2Bot);
void renderParty();
void renderBox();
void renderBag();
void bagTap(int16_t x, int16_t y);
void drawCareTabs();
bool careTabTap(int16_t x, int16_t y);
void renderMissions();
void missionTap(int16_t x, int16_t y);
void renderAdventure();
void adventureTap(int16_t x, int16_t y);
void renderExplore();
void exploreTap(int16_t x, int16_t y);
void renderBoss();
void bossTap(int16_t x, int16_t y);
void renderTower();
void towerTap(int16_t x, int16_t y);
void renderTreasure();
void treasureTap(int16_t x, int16_t y);
void renderRival();
void rivalTap(int16_t x, int16_t y);
void startRivalBattle();
void startVitalityGame();
void renderVitality();
void vitalityTap(int16_t x, int16_t y);
void leaveVitality();
void renderRandomEvent();
static void extraRewardLabel(char *out, size_t n, uint8_t kind, uint8_t id, uint8_t count);
void startTowerBattle();
void startBossBattle();
static void stepDefenseGauge(uint32_t now);
static uint8_t squadCapForRegion(uint8_t region, uint8_t idx, bool hard);
void drawConfirmPanel(const char *q, const char *sub1, const char *sub2,
                      uint16_t subCol, const char *o1, uint16_t c1, uint16_t t1,
                      const char *o2, uint16_t c2, uint16_t t2);
const char *const SCREEN_NAME[SCR_COUNT] = {
  "starter", "region", "gallery", "dexpick", "movepick", "box",
  "party", "keyboard", "card", "player", "clock", "gym", "gympick",
  "lan", "pick", "battle", "win", "learn", "train", "menu",
  "minigame", "main"
};

// Badge art for a region, or nullptr when that region has none yet.
//
// NEVER `BADGES_ART[region % BADGE_REGIONS]`. The moment GYM_REGIONS outgrew
// BADGE_REGIONS -- which happened when Kalos landed and the upstream badge set
// stops at Unova -- 5 % 5 = 0 silently dressed Kalos in KANTO's badges, on both
// the win screen and the player card. A wrong badge is worse than an honest
// blank: it claims you won something you did not. The callers draw a
// placeholder instead, and static_assert below stops the array being indexed
// out of range whatever the two counts are.
static_assert(BADGE_REGIONS <= GYM_REGIONS,
              "more badge sheets than ladders -- BADGES_ART would be indexed by a region that does not exist");
static const BadgeArt *badgeArtFor(uint8_t region, uint8_t i) {
  if (region >= BADGE_REGIONS || i >= TRAINER_GYMS) return nullptr;
  return &BADGES_ART[region][i];
}

// Does this region have its own badge art? Split out with a primitive return
// type so the tests can ask: the emulator's genproto.py emits every prototype
// ABOVE the includes, where the BadgeArt struct does not exist yet.
bool badgeArtExists(uint8_t region, uint8_t i) { return badgeArtFor(region, i) != nullptr; }


// v3.61.4: Galar and Paldea use lightweight original pixel emblems rather than
// borrowing older-region art. They are deliberately simple, type-coloured
// device badges (not ripped game assets) and remain distinct for all 8 leaders.
static bool modernBadgeRegion(uint8_t region) { return region == 7 || region == 8; }
static void drawModernBadge(uint8_t region, uint8_t i, int cx, int cy, int scale, bool earned) {
  const Trainer &tr = TRAINER_SETS[region % GYM_REGIONS].list[i % TRAINER_GYMS];
  uint16_t col = earned ? typeColor(tr.type) : UI_TRACK;
  uint16_t edge = earned ? UI_INK : UI_TRACK;
  int r = 8 * scale;
  if (region == 7) { // Galar: league medallion + one of eight puzzle tabs
    gfx->fillCircle(cx, cy, r, col);
    gfx->drawCircle(cx, cy, r, edge);
    gfx->drawCircle(cx, cy, r - 2 * scale, UI_WHITE);
    static const int8_t TX[8] = {0,5,7,5,0,-5,-7,-5};
    static const int8_t TY[8] = {-7,-5,0,5,7,5,0,-5};
    int tx = cx + TX[i & 7] * scale, ty = cy + TY[i & 7] * scale;
    gfx->fillCircle(tx, ty, 2 * scale, col);
    gfx->drawCircle(tx, ty, 2 * scale, edge);
  } else { // Paldea: compact academy shield/diamond
    gfx->fillTriangle(cx, cy - r, cx - r, cy, cx, cy + r, col);
    gfx->fillTriangle(cx, cy - r, cx + r, cy, cx, cy + r, col);
    gfx->drawLine(cx, cy - r, cx - r, cy, edge);
    gfx->drawLine(cx - r, cy, cx, cy + r, edge);
    gfx->drawLine(cx, cy + r, cx + r, cy, edge);
    gfx->drawLine(cx + r, cy, cx, cy - r, edge);
    int notch = ((i & 3) - 1) * 2 * scale;
    gfx->fillCircle(cx + notch, cy, 2 * scale, UI_WHITE);
    gfx->drawCircle(cx + notch, cy, 2 * scale, edge);
  }
}

Combatant btlYou, btlFoe;
bool btlOver = false;
bool btlWon = false;
bool btlNewBadge = false;
uint32_t btlWinUntil = 0;   // the win screen is up
// A trainer fight is a run of 1v1s: both sides queue their squad and the next
// one steps up when the current one faints. This is the whole difficulty curve
// -- no gating, just attrition, so one strong creature sweeps Brock and dies
// four deep into Lance.
// The ladder is now sequential: a leader opens once the previous one is beaten,
// tracked per difficulty so hard mode is its own run. This replaces the earlier
// "no gating, attrition is the gate" rule -- with both ladders level-capped,
// nothing stopped you opening with Lance and simply losing, which read as a
// dead end rather than a challenge.
// reaction test (trains SPEED)
bool spdOpen = false;
uint32_t spdUntil = 0, spdOverUntil = 0, spdBorn = 0;
int16_t spdX = 0, spdY = 0;
uint16_t spdHits = 0, spdMisses = 0;   // spdHits is the score (gold targets give +2)
uint8_t spdIvReward = XITEM_COUNT;
uint8_t spdIvRewardCount = 0;
bool spdShinyBerryReward = false;
uint16_t spdCombo = 0, spdBestCombo = 0;
uint8_t spdGain = 0;
bool spdNewHi = false;
bool spdGold = false;                  // rare bonus lightning target

bool hpOpen = false;
uint32_t hpRoundStarted = 0, hpFeedbackUntil = 0, hpOverUntil = 0;
uint8_t hpRound = 0, hpFeedback = 0, hpGain = 0;
uint16_t hpScore = 0;
uint8_t hpIvReward = XITEM_COUNT, hpIvRewardCount = 0;
bool hpShinyBerryReward = false;
#define HP_ROUNDS 12

bool gymOpen = false;
bool gymHard = false;   // which ladder the list is showing

// LAN battle. `lanOpen` is the pairing screen; once both squads are known the
// normal battle screen takes over with btlLink set.
bool lanOpen = false;
Link lan;
// What the shared region chooser is being used FOR. It only changes the
// subtitle and whether there is a way back: at first boot every count would
// read zero, which tells the player nothing, and there is nowhere to go back to.
#define RPICK_FOR_GYMS  0
#define RPICK_FOR_DEX   1
#define RPICK_FOR_START 2
// Three rows is what the round panel fits above the BACK label. The dex now
// lists four regions and will list more, so the chooser PAGES rather than
// growing -- and every paged screen in this sketch has to be driven by
// swipe_test, which is why that test exists at all.
#define RPICK_PER_PAGE  3
extern uint8_t rpickPage;
static void renderRegionPick(uint8_t mode);   // the region chooser, defined below
// Not static: swipe_test drives these so it asks the FIRMWARE for the page
// count instead of recomputing it from its own copy of RPICK_PER_PAGE, which
// would prove the transcription rather than the screen.
uint8_t rpickRegions(uint8_t mode);           // rows this mode lists (gyms: 3)
uint8_t rpickPageCount(uint8_t mode);
uint8_t rpickModeNow();                       // which chooser is up, or 0xFF
static bool rpickSwipe(int dir);              // true if it handled the gesture
static int regionPickTap(int16_t x, int16_t y, uint8_t mode);
static void drawEggRegion();          // defined with the egg screen helpers
static int eggRegionTap(int16_t x, int16_t y);
static void drawBtlBack();
static void btlLinkPoll();   // defined with the battle code, called from render()
static void btlSwitchTo(uint8_t i);
static void btlResolve(uint8_t yourMove);
// The peer's whole team, kept live. A trainer's replacements are built fresh
// from TRAINERS[] because they only ever arrive once; a linked opponent can
// switch OUT and back IN, so its creatures have to remember how battered they
// are. Host side only -- the guest takes absolute health off the wire.
Combatant btlFoeSquad[TRAINER_TEAM_MAX];
uint8_t btlFoeSquadN = 0;
uint8_t btlMyAct = 0;        // host: our own action, latched until theirs lands
// Which ladder the gym screen and the current fight belong to. The battle keeps
// its own copy so that leaving the gym list mid-fight cannot retarget the badge.
// The BOX button on the party screen. It was 150x32 with a pixel-exact hit
// test, which is a small target on a round panel -- reported as hard to press,
// same complaint as the battle grid. Now bigger AND padded: the drawn size grew
// too, so the button looks like the size it actually is rather than hiding a
// generous hit area behind a small graphic.
// The smallest a button may be. Three separate "hard to hit" reports -- the
// battle grid's bottom row, the party screen's BOX, and the LAN button -- were
// all the same mistake: a control sized to fit its label rather than a finger.
// 44 px is the usual guidance and roughly a fingertip on this 466 px panel.
#define UI_TAP_MIN 44

// The LAN battle button on the gym region chooser.
#define LANBTN_W 190
#define LANBTN_H UI_TAP_MIN
#define LANBTN_X (233 - LANBTN_W / 2)
#define LANBTN_Y 336

#define BOXBTN_X 146
#define BOXBTN_Y 320
#define BOXBTN_W 174
#define BOXBTN_H UI_TAP_MIN
#define BOXBTN_PAD 8
// CLOSE sits below BOX with a real gap between them. They used to touch at
// y=372, and BOX's padding then reached to 380 -- so the top of CLOSE was
// inside BOX's hit area and taps there opened the box instead of closing the
// screen. Padding one button into its neighbour just moves the problem along.
#define PARTYCLOSE_Y 376
#define PARTYCLOSE_H UI_TAP_MIN
#define PARTYCLOSE_X 133
#define PARTYCLOSE_W 200

// The confirm panel, in ONE place. Three callers draw it -- the evolve/farewell/
// retire dialog on the main screen, and letting a banked creature go from the
// party or box sheet -- and hit_test asserts against the same numbers through
// uiConfirmRects(). Copies of this geometry in the tap handler are exactly how
// a YES button ends up somewhere the drawing is not.
#define CONFIRM_X 73
#define CONFIRM_Y 156
#define CONFIRM_W 320
#define CONFIRM_H 188
#define CONFIRM_BTN_X 93
#define CONFIRM_BTN_W 280
#define CONFIRM_BTN_H 52
#define CONFIRM_B1_Y 206
#define CONFIRM_B2_Y 268

// The two buttons on the party/box detail sheet. A third full-width row would
// not fit above BACK, and BRING BACK was h=38 -- under UI_TAP_MIN, the shape
// section 4 of CLAUDE.md keeps warning about.
//
// THEY ARE NOT EQUAL HALVES, and that is the whole point. The first version
// centred the pair on 233 with a dead gap between them -- which put the gap at
// the CENTRE OF THE PANEL, the one place a thumb naturally lands, so BRING BACK
// "did not work" and was reported as broken. Worse, every build up to v3.5 drew
// BRING BACK as a FULL-WIDTH button centred exactly there, so muscle memory
// aimed straight at the dead zone.
//
// The primary action now owns the centre. RELEASE is narrower, offset right,
// and still over UI_TAP_MIN -- it is irreversible, so it should be reachable
// but never the thing you hit by aiming at the middle. The gap between them is
// wider than before, not narrower.
#define PDET_BTN_Y 336
#define PDET_BTN_H 48
#define PDET_L_X 70
#define PDET_L_W 180
#define PDET_R_X 266
#define PDET_R_W 120
static_assert(PDET_L_X < 233 && 233 < PDET_L_X + PDET_L_W,
              "the panel centre must land on the sheet's PRIMARY button, not between the two");
static_assert(PDET_R_X > PDET_L_X + PDET_L_W + 8,
              "the destructive button needs a real dead gap before it");

uint8_t gymRegion = 0;
uint8_t btlRegion = 0;
#define TRAINERS (TRAINER_SETS[gymRegion % GYM_REGIONS].list)
#define BTL_TRAINERS (TRAINER_SETS[btlRegion % GYM_REGIONS].list)
bool gShowAllAvatars = false;  // emulator screenshot aid, never set on hardware
bool btlPetIn = false;       // was the live pet in the squad?
uint8_t btlTrainGain = 0;    // what the win trained, for the win screen
uint8_t btlTrainWhich = 0;
bool btlLink = false;      // this fight is against another device
bool btlLinkHost = false;
static bool gymUnlocked(uint8_t idx, bool hard) {
  return idx == 0 || pet.hasBadge(gymRegion, idx - 1, hard);
}

// Team select. Candidate 0 is the live pet, 1..PARTY_SLOTS are the banked
// party members, and the remaining candidates are box members. A 32-bit mask
// covers the full 1 + 6 + 18 roster without moving any saved creature.
bool pickOpen = false;
// The team picker serves the gym ladder and the LAN screen both. PICK_LAN is
// not a trainer index: squadCap() already returns an uncapped six for anything
// past the roster, which is what a LAN battle wants -- two players who know
// each other can bring what they like.
#define PICK_LAN 0xFF
#define PICK_BOSS 0xFE
static void lanOffer(bool host);
uint8_t pickTrainer = 0;
uint8_t pickRegion = 0;      // region latched when a gym leader opens the team picker
bool pickHard = false;
bool lanWantHost = true;   // which button opened the picker
uint32_t squadMask = 0xFFFFFFFFUL;   // everything, until the player says otherwise
uint8_t pickPage = 0;
uint8_t pickSourceTab = 0;  // 0=current+party, 1=box; selection persists across tabs
#define PICK_PER_PAGE 6
#define PICK_CELL_W 150
#define PICK_CELL_H 68
#define PICK_X(i) (78 + ((i) % 2) * (PICK_CELL_W + 10))
#define PICK_Y(i) (108 + ((i) / 2) * (PICK_CELL_H + 6))
#define PICK_TAB_Y 70
#define PICK_TAB_H 30
#define PICK_TAB_W 150
#define PICK_TAB_X(i) (78 + (i) * (PICK_TAB_W + 10))
#define PICK_GO_Y 350
// BACK beside FIGHT on the team-select screen. There was no way out of it but a
// swipe, which is invisible -- the same complaint as everywhere else.
#define PICK_BTN_W 155
#define PICK_BTN_H UI_TAP_MIN
#define PICK_BACK_X (233 - PICK_BTN_W - 7)
#define PICK_GO_X (233 + 7)
bool playerOpen = false;
// One badge page per gym region, then the medals. Three ladders will not fit on
// one page, and the page you are on IS the region -- no extra control needed,
// and horizontal paging already works everywhere else.
uint8_t playerPage = 0;
#define PLAYER_PAGES (GYM_REGIONS + 1)
#define playerBadgeRegion (playerPage % GYM_REGIONS)
uint8_t gymPage = 0;
#define GYM_ROWS 5
#define GYM_ROW_Y(i) (110 + (i) * 50)
int8_t btlTrainer = -1;      // index into TRAINERS, -1 = a one-off fight
bool btlHard = false;
Combatant btlSquad[TRAINER_TEAM_MAX + 1];
uint8_t btlSquadN = 0, btlSquadAt = 0;
uint8_t btlFoeAt = 0;

// Animation. Deliberately built on the thumbnails the screen already draws
// rather than on PmdMon: three PmdMon blobs are live already, and the battle
// has to stay graceful on a board with no SD at all (S_NO_SPRITES). Index 0 is
// you, 1 is the foe.
uint32_t btlLungeUntil[2] = { 0, 0 };   // acted: leans toward the opponent
uint32_t btlHitUntil[2] = { 0, 0 };     // was hit: jitters and flashes
uint16_t btlHpShown[2] = { 0, 0 };      // bars ease toward the real value
// Two streamed sprites, so the creatures can actually swing and flinch. They
// cost ~135 KB of PSRAM each on average and are freed when the fight ends. The
// player's side is NOT the global `pmd`: the active creature may be a banked
// party member rather than the live pet.
PmdMon btlPmd[2];
int16_t btlPmdDex[2] = { 0, 0 };
static uint16_t btlDigiPixels[2][48*48];
static int16_t btlDigiFor[2]={-1,-1};
static uint16_t btlDigiTransparent[2]={0,0};
static bool loadBattleDigi(uint8_t who,uint16_t id);
// A faint used to swap the next creature in instantly, inside the same call
// that resolved the turn -- which is why it felt like a jump cut. The swap is
// now deferred: the fainted one drops out of frame, and the replacement slides
// in only once the player dismisses that message.
uint32_t btlFaintUntil[2] = { 0, 0 };
uint32_t btlEnterUntil[2] = { 0, 0 };
int8_t btlSwapWho = -1;        // 0 = your side, 1 = the foe's, -1 = nothing due
#define BTL_FAINT_MS 700
#define BTL_ENTER_MS 420
// battle menu: 0 = FIGHT/POKEMON, 1 = the moves, 2 = the switch list
uint8_t btlMenu = 0;
#define BTL_LUNGE_MS 260
#define BTL_HIT_MS 420
char btlMsg[6][96];
uint8_t btlMsgCount = 0;   // queued lines; a tap shows the next
#define BTL_CELL_W 160
#define BTL_CELL_H 44
#define BTL_GRID_X 69
#define BTL_GRID_Y 274
#define BTL_CELL_X(i) (BTL_GRID_X + ((i) % 2) * (BTL_CELL_W + 8))
#define BTL_CELL_Y(i) (BTL_GRID_Y + ((i) / 2) * (BTL_CELL_H + 8))

// A cell's HIT area is bigger than the cell that is drawn. On the board the two
// bottom buttons were much harder to hit than the top two: the drawn cells are
// only 44 px tall, there was an 8 px dead gap between the rows, and everything
// below the bottom row was dead too -- so a finger landing low, or a touch panel
// reading a few pixels high, missed entirely. Nothing else is tappable in this
// area while the grid is open, so the slop costs nothing.
//
// The gap between the two rows and the two columns is split down the middle, and
// the bottom row additionally claims the empty space beneath it.
#define BTL_HIT_PAD 4
// The bottom row keeps extra room downward, but not so much that it reaches the
// BACK bar below it -- the mistake made once already with BOX and CLOSE.
#define BTL_HIT_BOTTOM 6
// BACK, under the grid: the move and switch screens had no way out except
// choosing something.
// The gym list's EASY/HARD pill. It was 24 px tall, which is half a fingertip.
// Sits between the title and the first leader row. At 72 with a 44 px height it
// ran to 116 and overlapped the first row, which begins at 110 -- introduced
// when the pill was enlarged to a real tap target.
#define GYMDIF_Y 60
#define GYMDIF_H UI_TAP_MIN
#define BTL_BACK_W 190
#define BTL_BACK_H UI_TAP_MIN
#define BTL_BACK_X (233 - BTL_BACK_W / 2)
#define BTL_BACK_Y 384
#define BTL_HIT_X0(i) (BTL_CELL_X(i) - BTL_HIT_PAD)
// The far edges stop one pixel short so the four boxes TILE: the gap between
// two cells is split down the middle with no pixel left over and none shared.
#define BTL_HIT_X1(i) (BTL_CELL_X(i) + BTL_CELL_W + BTL_HIT_PAD - 1)
#define BTL_HIT_Y0(i) (BTL_CELL_Y(i) - BTL_HIT_PAD)
#define BTL_HIT_Y1(i) (BTL_CELL_Y(i) + BTL_CELL_H - 1 + \
                       ((i) / 2 ? BTL_HIT_BOTTOM : BTL_HIT_PAD))

// Which cell a point falls in, or -1. Exposed (not static) so a test can sweep
// the panel and prove there are no dead pixels between the cells -- the bug that
// made the bottom row hard to press was a gap, not a wrong rectangle.
int btlCellIndexAt(int16_t x, int16_t y);

// Used by BOTH the move grid and the switch grid. They had a copy each of the
// same rectangle test, which is exactly how two halves of one control drift.
static inline bool btlCellHit(int i, int16_t x, int16_t y) {
  return x >= BTL_HIT_X0(i) && x <= BTL_HIT_X1(i) &&
         y >= BTL_HIT_Y0(i) && y <= BTL_HIT_Y1(i);
}
#define TRAIN_X 73
#define TRAIN_Y 52
#define TRAIN_W 320
#define TRAIN_H 370
#define TRAIN_ROW_H 56
#define TRAIN_ROW_GAP 8
#define TRAIN_ROW_Y(i) (TRAIN_Y + 54 + (i) * (TRAIN_ROW_H + TRAIN_ROW_GAP))

// las 9 especies con sprite propio en flash (respaldo sin SD): dex -> indice
int flashIdxForDex(int16_t dex) {
  static const int8_t IDX[10] = { -1, 3, 4, 5, 0, 1, 2, 6, 7, 8 };
  return (dex >= 1 && dex <= 9) ? IDX[dex] : -1;
}

#define CX 233  // centro de la pantalla redonda
#define CY 233
#define PET_CY 202  // centro vertical del sprite

// v3.20: three live raising tabs. They sit between the status line and the
// sprite; small enough not to cover the pet, large enough for the round touch UI.
#define CARE_TAB_Y 112
#define CARE_TAB_W 54
#define CARE_TAB_H 24
#define CARE_TAB_GAP 8
#define CARE_TAB_X0 (CX - (CARE_SLOT_COUNT * CARE_TAB_W + (CARE_SLOT_COUNT - 1) * CARE_TAB_GAP) / 2)

static const uint16_t INK_K = 0x18C4;  // spriteColor('k')

// botones de icono siguiendo el arco inferior de la pantalla redonda
// (los exteriores van mas altos para no salirse del circulo)
struct Btn {
  int16_t cx, cy;
  const char *const *icon;
};
// Five across the arc: spacing tightened 62 -> 54 so the outer pair stays far
// enough in to keep its old y. Lifting them instead would have run the row into
// the ENE/HYG bars, which end at y=361 -- the buttons are 52 tall, so any centre
// above 387 overlaps them.
// Four, not five. The ball had its own icon here until it became DEFENCE's
// trainer and moved into the training menu -- at which point tapping it just
// opened the same menu the dumbbell does, two icons for one destination.
// They sit on the panel's curve: y = 406 - dx^2/729.
#define BTN_COUNT 4
// Referred to by NAME, never by literal index. Removing the ball icon shifted
// every index by one and drawButtons() still had `i != 2` meaning LIGHT -- which
// silently made the BATH button the one that wakes the pet.
#define BTN_FOOD  0
#define BTN_LIGHT 1
#define BTN_BATH  2
#define BTN_TRAIN 3
Btn buttons[BTN_COUNT] = {
  { 134, 393, SPR_ICON_FOOD },   // comer
  { 200, 405, SPR_ICON_LIGHT },  // luz
  { 266, 405, SPR_ICON_CLEAN },  // bano
  { 332, 393, SPR_ICON_TRAIN },  // entrenar
};
#define BTN_HALF 30  // boton de 60x60 -- mas grande y mas separado que antes
#define BTN_HIT 40   // radio tactil (un poco mas generoso)

// grietas del huevo (pixeles 'k' sobre el sprite)
static const uint8_t CRACK1[][2] = { {15,8},{16,9},{15,10} };
static const uint8_t CRACK2[][2] = { {11,13},{12,14},{11,15},{20,12},{19,13},{20,14} };
// estrellas del modo noche
static const uint16_t STARS[][2] = { {120,140},{330,120},{370,210},{95,230},{280,90},{160,95} };

bool wasPressed = false;
// eleccion de inicial (primera partida): Bulbasaur / Charmander / Squirtle, 3 filas
// The first-boot starter list is the FRONT of each region's starter array in
// dex.h -- not a copy of it. That array is also the pool a region's first egg
// is drawn from (pet.cpp rollInRegion), where Kanto's five deliberately include
// Pikachu and Eevee; the choice screen shows the canonical three and leaves the
// rest to the egg. starter_test pins the first three of every region, so
// reordering that array cannot silently change the first screen anyone sees.
#define STARTER_SHOWN 3
int16_t starterOf(uint8_t region, uint8_t i) {
  const RegionInfo &rg = REGIONS[region % REGION_COUNT];
  if (i >= rg.starterCount) i = 0;
  return rg.starters[i];
}
uint8_t starterCountShown(uint8_t region) {
  uint8_t n = REGIONS[region % REGION_COUNT].starterCount;
  return n < STARTER_SHOWN ? n : STARTER_SHOWN;
}

// First boot runs region -> starter. This is NOT persisted: a reset between the
// two lands back on the region, which is the harmless direction to fail in --
// nothing has been chosen yet, and pet.setRegion() is idempotent.
static bool starterRegionDone = false;
#define STARTER_ROW_Y 110
#define STARTER_ROW_H 70
#define STARTER_ROW_GAP 8
// boton-CTA de evolucion (centrado, mitad de pantalla)
#define EVO_BTN_W 256
#define EVO_BTN_H 64
#define EVO_BTN_X (CX - EVO_BTN_W / 2)
#define EVO_BTN_Y 172
// Evolution now lives on the stats card instead of covering the home Pokemon.
#define CARD_EVO_X 116
#define CARD_EVO_Y 312
#define CARD_EVO_W 234
#define CARD_EVO_H 46
// boton-CTA de despedida (mas ancho: lleva el nombre + frase)
#define FAR_BTN_W 408
#define FAR_BTN_H 58
#define FAR_BTN_X (CX - FAR_BTN_W / 2)
#define FAR_BTN_Y 176
// el CST9217 avisa por el pin INT cuando hay datos tactiles; lo usamos para no
// leer el bus I2C mientras el chip esta dormido (esa lectura se colgaba ~1s)
volatile bool gTouchIrq = false;
void IRAM_ATTR touchIsr() { gTouchIrq = true; }
uint32_t lastRender = 0;
// proteccion del AMOLED: atenuado por inactividad
uint32_t lastInteract = 0;
uint8_t dimStage = 0;        // 0 awake, 1 dimmed (90s); 5min enters true screenOff
bool swallowGesture = false; // el toque que despierta no acciona nada
uint32_t holdStart = 0;     // pulsacion larga sobre el bicho
uint32_t confirmUntil = 0;  // dialogo "soltar?" activo hasta este millis
uint8_t choiceKind = 0;     // dialogo de decision: 0 ninguno, 1 evolucion, 2 despedida
uint32_t choiceUntil = 0;   // se cierra solo a este millis
int16_t tX0, tY0, tXl, tYl; // gesto en curso (inicio y ultima posicion)
uint32_t tStart = 0;
bool holdFired = false;

// v3.61.5: loop() snapshots millis() before touch handling, but a touch can
// update lastInteract a few milliseconds later. Unsigned subtraction of that
// "future" timestamp used to look like ~49 days of idle time and immediately
// forced screenOff. Use a signed delta (still safe across the normal millis()
// wrap for short intervals) and centralize screen power transitions.
static inline uint32_t safeElapsedMs(uint32_t now, uint32_t then) {
  int32_t d = (int32_t)(now - then);
  return d < 0 ? 0UL : (uint32_t)d;
}

static void setScreenOffState(bool off, uint32_t now) {
  if (screenOff == off) return;
  screenOff = off;
  pet.setScreenOff(off);
  audioSetScreenAwake(!off);

  // Never let an old CST9217 press leak across an off/on transition.
  gTouchIrq = false;
  wasPressed = false;
  swallowGesture = false;

  if (!off) {
    lastInteract = now;
    invalidateHomePresent();
  }
}

void setup() {
  Serial.setRxBufferSize(8192);  // la transferencia a SD llega en bloques de 2 KB
  Serial.begin(115200);
  // CRITICO: sin esto, Serial.print BLOQUEA el juego cuando no hay un
  // monitor serie abierto en el host (el bufer TX del USB CDC se llena
  // y nadie lo vacia) -> con timeout 0 los mensajes se descartan
  Serial.setTxTimeoutMs(0);
  Serial.printf("TamaPoke fw v%s\n", FW_VERSION);
  bootReport();   // why the last run ended, and what it was doing
  loadLang();  // saved language; KO is the default on a fresh install
  Wire.begin(IIC_SDA, IIC_SCL);
  // CST9217 (tactil), AXP2101 (PMU) y PCF85063 (RTC) comparten este bus I2C.
  // Red de seguridad para PMU/RTC (SensorLib NO respeta este timeout en el
  // tactil; el cuelgue del tactil dormido se resuelve gateando por INT, ver
  // handleTouch).
  Wire.setTimeOut(50);

  // CRITICO: encender la alimentacion del panel (BLDO1=OLED VDD 3.3V) ANTES de
  // inicializar el display. Si el PMU se reseteo (drenaje total), este rail
  // queda OFF y la pantalla se ve negra aunque el resto de la placa funcione.
  pmuEnablePanel();

  // QSPI a 80MHz (por defecto 40): el flush del framebuffer es el cuello de
  // botella del fps (~56ms a 40MHz). Si el panel mostrara basura, bajar a 40M.
  if (!gfx->begin(80000000)) Serial.println("gfx->begin() fallo");
  applyLanguageFont();
  panel->setBrightness(180);

  touch.setPins(TP_RESET, TP_INT);
  bool touchOk = false;
  for (int i = 0; i < 3 && !touchOk; i++) {  // a veces falla al primer intento
    touchOk = touch.begin(Wire, 0x5A, IIC_SDA, IIC_SCL);
    if (!touchOk) delay(150);
  }
  if (!touchOk) Serial.println("CST9217 no detectado");
  // begin() deja el chip en modo comando (lee la identidad y no sale);
  // hace falta un reset por hardware para que vuelva a reportar toques
  touch.reset();
  touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
  touch.setMirrorXY(true, true);  // el panel esta montado girado 180 grados
  // INT activo-bajo: salta cuando hay datos. Gatea las lecturas I2C (ver loop)
  pinMode(TP_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TP_INT), touchIsr, FALLING);

  party.begin();
  extras.begin();   // Pet::newEgg() can consume an armed Shiny Charm safely
  pet.begin();
  sdBegin();
  thumbs.load();

  // reloj real: aplica el tiempo que estuvo apagado
  rtcBegin();
  batBegin();
  pwrSetup();
  uint32_t e = rtcEpoch();
  if (e == 0) {
    rtcSetEpoch(1767225600UL);  // RTC virgen: semilla (la hora absoluta da igual,
    e = rtcEpoch();             // solo importan las diferencias)
    Serial.println("RTC sin hora: sembrado, sin progresion offline esta vez");
  }
  pet.syncClock(e);
  careSlots.begin(pet, e);
  extras.syncObservedPet(pet);

  audioBegin();  // ES8311 + I2S + amplificador (suena un jingle de arranque)

  lastInteract = millis();
}

// carga/descarga el sprite de SD cuando cambia la especie
void ensureMon() {
  if (pet.speciesId == monFor && monShinyFor == pet.shiny && !sdDirty) return;
  sdDirty = false;
  monFor = pet.speciesId;
  monShinyFor = pet.shiny;
  mon.unload();
  pmd.unload();
  beh.x = beh.targetX = 233;
  beh.mode = 0;
  beh.until = 0;
  // DEX_ENABLED is authoritative. Even if an older microSD still contains a
  // retired/temporarily-disabled p<ID>.bin, never render it from stale media.
  if (pet.speciesId >= 1 && pet.speciesId <= DEX_COUNT && speciesHasArt(pet.speciesId)) {
    pmd.load(pet.speciesId, pet.shiny);          // principal: PMD
    if (!pmd.loaded) mon.load(pet.speciesId, pet.shiny);  // respaldo: B/N
  }
}

static bool creatureHasArt(int16_t id) {
  return isDigimonId(id) || (id>=1 && id<=DEX_COUNT && speciesHasArt(id));
}

void loop() {
  uint32_t now = millis();
  pet.update(now);

  // The link is pumped here rather than from the LAN screen, because it has to
  // keep running through the battle too: linkNowPoll() drains what the radio
  // parked on the WiFi task, and tick() is what resends a lost packet and gives
  // up on a peer that has gone quiet.
  if (lan.live()) {
    linkNowPoll();
    lan.tick(now);
  }

  // avisa con un sonido cuando el bicho pasa a estar listo para evolucionar
  // (incluye el caso de cumplir al despertar). canEvolveNow es false durmiendo.
  static bool wasEvoReady = false;
  bool evoReady = pet.wantEvolveButton();
  if (evoReady && !wasEvoReady && !screenOff) sfxPlay(SFX_MEDAL);
  wasEvoReady = evoReady;
  // aviso sombrio cuando el bicho esta a punto de escaparse por abandono
  static bool wasRunReady = false;
  bool runReady = pet.canRunawayNow();
  if (runReady && !wasRunReady && !screenOff) sfxPlay(SFX_DENY);
  wasRunReady = runReady;

  handleTouch();
  handleSerial();
  ensureMon();

  // Input handlers may call millis() after the loop's original timestamp was
  // captured. Refresh now so every later idle/power decision sees a time that
  // is not older than lastInteract. safeElapsedMs() below is the second guard.
  now = millis();

  // A sprite pack can arrive long after the card was mounted: the web installer
  // streams it over PUT into the FIRMWARE THAT IS ALREADY RUNNING. gRegionArt was
  // computed once in sdBegin(), so without this the region a player just spent ten
  // minutes downloading stayed greyed out reading NEEDS PACK until they rebooted --
  // indistinguishable, from the player's side, from the download having failed.
  // Quietly, because the host is still parsing this serial stream.
  if (sdArtDirty) {
    sdArtDirty = false;
    sdScanRegionArt(false);
  }

  // A farewell or release just finished: the creature is waiting for a slot.
  // With room it simply joins; with a full party the player is taken straight
  // to the party screen to choose who it replaces, or to let it go.
  if (pet.endedKind != CER_NONE && !partyPick) {
    // party first, then the box; only a full box makes it your choice
    if (party.add(pet.endedMon) || party.boxAdd(pet.endedMon)) {
      snprintf(partyBannerName, sizeof(partyBannerName), "%s",
               pet.endedMon.nick[0] ? pet.endedMon.nick : creatureName(pet.endedMon.dex));
      partyBannerUntil = now + 3500;
      pet.endedKind = CER_NONE;
      if (!screenOff) sfxPlay(SFX_MEDAL);
    } else {
      partyPick = true;
      partyOpen = true;
      menuOpen = false;
    }
  }

  // pulsacion corta del PWR: pantalla on/off
  static uint32_t lastPwr = 0;
  if (now - lastPwr > 250) {
    lastPwr = now;
    if (pwrShortPressed()) {
      // The side button is the only allowed wake source. While awake it still
      // acts as a convenient manual screen-off toggle.
      setScreenOffState(!screenOff, now);
    }
  }

  updateBrightness(now);

  // vuelca el autoguardado periodico SOLO con la pantalla atenuada/apagada o
  // durmiendo: la escritura a NVS congela ~1s ambos cores (caché de flash off),
  // y aqui no hay animacion que se corte ni dedo esperando respuesta. Con 90s
  // de inactividad la pantalla ya atenua, asi que se vuelca enseguida; el uso
  // activo persiste igual por los guardados de cada accion (comer/jugar/...).
  // A queued training result owns its own staggered persistence path below.
  // Do not let the generic idle/sleep saver flush pet + extras back-to-back and
  // accidentally recreate the flash-write burst this guard was added to avoid.
  if (!trainingPersistPhase && pet.savePending() && (screenOff || dimStage >= 1 || pet.sleeping)) {
    pet.flushSave();
  }
  if (!trainingPersistPhase && extras.savePending() && (screenOff || dimStage >= 1 || pet.sleeping)) {
    extras.flushPendingSave();
  }

  // v3.63.2: persist at most ONE NVS domain per loop pass after training. A
  // static result screen is also safe, so the earned berry/mission progress is
  // committed before the player dismisses the result instead of living only in
  // RAM for several seconds. Active gameplay still never writes flash here.
  bool trainingPersistSafe = (!gameOpen && !sackOpen && !spdOpen && !hpOpen) ||
                             (gameOpen && gameOverUntil) ||
                             (sackOpen && sackOverUntil) ||
                             (spdOpen && spdOverUntil) ||
                             (hpOpen && hpOverUntil);
  if (trainingPersistSafe && trainingPersistPhase &&
      (int32_t)(now - trainingPersistAfter) >= 0) {
    if (trainingPersistPhase == 1) {
      if (pet.savePending()) pet.flushSave();
      trainingPersistPhase = 2;
      trainingPersistAfter = millis() + 160UL;
    } else {
      if (extras.savePending()) extras.flushPendingSave();
      trainingPersistPhase = 0;
      trainingPersistAfter = 0;
    }
    now = millis();
  }

  // anota la hora real cada 30 s (se persiste en cada save del juego)
  static uint32_t lastClock = 0;
  if (now - lastClock > 30000) {
    lastClock = now;
    uint32_t e = rtcEpoch();
    if (e) pet.lastSeenEpoch = e;
  }

  // v3.17 meta loop: observe hatch/evolution for Pokedex research, refresh the
  // daily missions, and only roll a random event while the player is not in a
  // battle or another modal screen. Pending events wait safely for the home.
  extras.observePet(pet);
  bool extrasCanEvent = !battleOpen && !pickOpen && !lanOpen && !gymOpen &&
                        !partyOpen && !galleryOpen && !cardOpen && !playerOpen &&
                        !clockOpen && !kbOpen && !trainOpen && !gameOpen &&
                        !sackOpen && !spdOpen && !hpOpen && !bagOpen && !missionOpen &&
                        !adventureOpen && !exploreOpen && !bossOpen &&
                        !towerOpen && !treasureOpen && !rivalOpen && !menuOpen;
  extras.update(pet, extrasCanEvent);

  // v3.46: periodically verify the compact critical-progress guard even while
  // the device stays powered on. This catches an unexpected in-RAM rollback or
  // partial primary-save loss without waiting for the next reboot.
  static uint32_t lastProgressGuard = 0;
  if (now - lastProgressGuard > 600000UL) {
    lastProgressGuard = now;
    pet.verifyCriticalProgress();
  }

  // latido de salud cada 5 min (para el soak test; se descarta si no hay monitor)
  static uint32_t lastHealth = 0;
  if (now - lastHealth > 300000) {
    lastHealth = now;
    Serial.printf("HEALTH up=%lus heap=%u min=%u\n", (unsigned long)(now / 1000),
                  ESP.getFreeHeap(), ESP.getMinFreeHeap());
  }

  // Defence timing is updated independently from screen redraws. This keeps
  // attack arrival timing stable even if an AMOLED/SD frame takes longer.
  if (gameOpen && !gameOverUntil) stepDefenseGauge(now);

  // v3.61.3 fixed-cadence scheduler -------------------------------------------
  // Do not "stabilize" by lowering FPS. Active games/battles retain the proven
  // 85 ms cadence (~11.8 fps) and normal animated UI retains 100 ms (10 fps).
  // The renderer optimizations below are responsible for meeting that budget.
  // If one exceptional frame overruns (e.g. an SD species load), schedule the
  // next frame immediately after it finishes -- no extra 20/28/36 ms penalty and
  // no skipped deadline. This avoids the deliberate frame drops introduced by
  // the v3.61.2 heavy-scene pacing while preserving touch responsiveness.
  bool activeAnimated = gameOpen || sackOpen || spdOpen || hpOpen || battleOpen;
  const uint32_t targetFrameMs = activeAnimated ? 85UL : 100UL;
  static uint32_t nextFrameAt = 0;

  uint32_t sinceInput = safeElapsedMs(now, lastInteract);
  bool inputNeedsFrame = (lastInteract != 0 && lastInteract > lastRender &&
                          sinceInput < 120UL);
  if (inputNeedsFrame && (int32_t)(nextFrameAt - now) > 8) nextFrameAt = now;

  // When the AMOLED is explicitly off there is nothing to present. Keep game
  // time/events/saves running, but do zero Canvas->panel transfers until the
  // physical side button wakes it. This saves power without lowering a single
  // visible-frame cadence: 85/100 ms remain unchanged whenever the screen is on.
  if (screenOff) {
    nextFrameAt = 0;
  } else if (nextFrameAt == 0 || (int32_t)(now - nextFrameAt) >= 0) {
    uint32_t frameStart = millis();
    lastRender = frameStart;
    render();
    uint32_t frameDone = millis();
#if TAMAPOKE_FRAME_DIAG
    uint32_t frameMs = frameDone - frameStart;
#endif

    // Start-to-start cadence. Never add a recovery delay after an overrun; the
    // completed Canvas is already tear-safe. If rendering took longer than the
    // budget, the next loop is immediately eligible rather than dropping a frame.
    uint32_t deadline = frameStart + targetFrameMs;
    nextFrameAt = ((int32_t)(deadline - frameDone) > 0) ? deadline : frameDone;

#if TAMAPOKE_FRAME_DIAG
    // Optional soak diagnostics. Disabled in release builds because a USB CDC
    // printf at the wrong moment can itself become the slow frame being measured.
    static uint32_t lastSlowFrameLog = 0;
    static uint32_t frames10s = 0, overruns10s = 0, sumFrame10s = 0, maxFrame10s = 0;
    frames10s++; sumFrame10s += frameMs; if (frameMs > maxFrame10s) maxFrame10s = frameMs;
    if (frameMs > targetFrameMs) overruns10s++;
    if (frameMs > 120UL && frameDone - lastSlowFrameLog > 5000UL) {
      lastSlowFrameLog = frameDone;
      Serial.printf("RENDER slow=%lums target=%lums heap=%u psram=%u\n",
                    (unsigned long)frameMs, (unsigned long)targetFrameMs,
                    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
    }
    static uint32_t lastFrameStats = 0;
    if (frameDone - lastFrameStats >= 10000UL) {
      uint32_t avg = frames10s ? sumFrame10s / frames10s : 0;
      uint8_t fscr = uiCurrentScreen();
      Serial.printf("FRAME10S screen=%s n=%lu avg=%lums max=%lums over=%lu target=%lums\n",
                    fscr < SCR_COUNT ? SCREEN_NAME[fscr] : "?",
                    (unsigned long)frames10s, (unsigned long)avg,
                    (unsigned long)maxFrame10s, (unsigned long)overruns10s,
                    (unsigned long)targetFrameMs);
      frames10s = overruns10s = sumFrame10s = maxFrame10s = 0;
      lastFrameStats = frameDone;
    }
#endif
  }

}

// brillo segun sueno + inactividad (proteccion del AMOLED)
void updateBrightness(uint32_t now) {
  // Visible pet animations keep an already-awake panel from timing out, but
  // v3.61.5 never lets them wake a screenOff panel by themselves.
  if (!screenOff && (pet.evolving() || pet.ceremony || pet.eating() || pet.showHeart())) {
    lastInteract = now;
  }
  // Never interpret a touch timestamp written a few milliseconds after the
  // caller's old loop timestamp as billions of milliseconds of inactivity.
  uint32_t idle = safeElapsedMs(now, lastInteract);

  // v3.61.5: after five minutes without interaction, enter the same true
  // screen-off state as the side button. The display is not merely dimmed: it
  // stops receiving frame transfers, the PA is muted, and touch cannot wake it.
  // Only the physical AXP2101 side button returns from this state.
  if (!screenOff && idle > 300000UL) {
    setScreenOffState(true, now);
  }

  // Player-requested rest used to force brightness down to 25 immediately,
  // which made the wake/light control and the rest of the interface difficult
  // to see. Rest now changes only the pet state; it does not darken the panel.
  // While the pet is visibly resting we also suppress idle dimming so the
  // interface remains readable. Explicit screen-off still turns the panel off.
  dimStage = pet.sleeping ? 0 : ((idle > 300000) ? 2 : (idle > 90000) ? 1 : 0);
  uint8_t target = usbPresent() ? 180 : 145;
  if (dimStage == 1) target = 60;
  else if (dimStage == 2) target = 8;
  if (screenOff) target = 0;
  static uint8_t current = 255;
  if (target != current) {
    current = target;
    panel->setBrightness(target);
  }
}

// ---------- consola serie (provision de SD + depuracion) ----------

void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;
  if (sdSerialCommand(line)) return;

  if (line.startsWith("DIGI START ")) {
    digiPet.start((uint8_t)line.substring(11).toInt());
    Serial.printf("DIGI %s Ver.%u Lv.%u\n", digiPet.species().name, digiPet.version, digiPet.level());
    Serial.println("DONE");
  } else if (line.startsWith("DIGI TRAIN ")) {
    String a=line.substring(11); a.toUpperCase();
    DigiTrain k=a=="DEF"?DIGI_DEF:a=="SPE"?DIGI_SPE:a=="HP"?DIGI_HP:DIGI_ATK;
    digiPet.train(k);
    Serial.printf("DIGI TRAIN %u/%u/%u/%u\n",digiPet.training[0],digiPet.training[1],digiPet.training[2],digiPet.training[3]);
    Serial.println("DONE");
  } else if (line == "DIGI EVOLVE") {
    bool ok=digiPet.evolve();
    Serial.printf("DIGI %s %s Lv.%u\n",ok?"EVOLVED":"WAIT",digiPet.species().name,digiPet.level());
    Serial.println("DONE");
  } else if (line == "DIGI STATS") {
    Serial.printf("DIGI Ver.%u %s stage=%u Lv.%u canEvo=%d\n",digiPet.version,digiPet.species().name,digiPet.species().stage,digiPet.level(),digiPet.canEvolve());
    Serial.printf("type=%s/%s moves=%s,%s,%s,%s\n",typeName(digiPet.type1()),digiPet.type2()==T_NONE?"-":typeName(digiPet.type2()),MOVE_TBL[digiPet.moves[0]].name,MOVE_TBL[digiPet.moves[1]].name,MOVE_TBL[digiPet.moves[2]].name,MOVE_TBL[digiPet.moves[3]].name);
    Serial.printf("stat=%u/%u/%u/%u iv=%u/%u/%u/%u train=%u/%u/%u/%u shinyCandy=BLOCKED\n",
      digiPet.stat(DIGI_ATK),digiPet.stat(DIGI_DEF),digiPet.stat(DIGI_SPE),digiPet.stat(DIGI_HP),
      digiPet.iv[0],digiPet.iv[1],digiPet.iv[2],digiPet.iv[3],digiPet.training[0],digiPet.training[1],digiPet.training[2],digiPet.training[3]);
    Serial.println("DONE");
  } else if (line == "HATCH") {
    pet.eggTap(); pet.eggTap(); pet.eggTap();
    Serial.println("DONE");
  } else if (line.startsWith("SPEC ")) {
    int n = line.substring(5).toInt();
    if (n >= 1 && n <= DEX_COUNT) {
      pet.prevSpeciesId = pet.speciesId;
      pet.speciesId = n;
      Serial.printf("especie #%d %s\n", n, DEX_TBL[n].name);
    }
    Serial.println("DONE");
  } else if (line.startsWith("LVL ")) {
    // level() is 1 + age/rate, so the age for level N is (N-1) rates -- this
    // used to set N and hand back N+1, which is a poor thing for a command
    // named LVL to do when it is what every balance check is anchored on.
    long want = line.substring(4).toInt();
    if (want < 1) want = 1;
    if (want > MAX_LEVEL) want = MAX_LEVEL;
    pet.ageMinutes = (uint32_t)(want - 1) * MINUTES_PER_LEVEL;
    pet.levelMinutes = pet.ageMinutes;
    pet.sleepLevelRemainder = 0;
    pet.saveNow();
    Serial.printf("lvl=%u\n", pet.level());
  } else if (line.startsWith("MISS ")) {
    // MISS <n>: sets the care mistakes -- "descuidos", the desc= on STATS.
    // Each one pushes every evolution threshold up a level, so a creature that
    // was neglected early evolves late; MISS 0 forgives that. Its sibling
    // commands are IV, TR and LVL.
    long m = line.substring(5).toInt();
    if (m < 0) m = 0;
    if (m > MAX_LEVEL) m = MAX_LEVEL;
    pet.careMistakes = (uint8_t)m;
    pet.saveNow();
    Serial.printf("desc=%u\n", pet.careMistakes);
  } else if (line.startsWith("TR ")) {
    // TR <atk> <def> <spe>: sets the TRAINING (this game's EVs), for testing a
    // fully-raised creature without playing the minigames for an hour. Each is
    // clamped to trMaxFor(iv), the same IV-bound ceiling the games enforce, so
    // this cannot produce a creature the player could not have raised.
    int v[3] = { 0, 0, 0 };
    int n = sscanf(line.c_str() + 3, "%d %d %d", &v[0], &v[1], &v[2]);
    if (n >= 1) {
      int a = v[0], d = (n >= 2) ? v[1] : v[0], e = (n >= 3) ? v[2] : v[0];
      pet.trAtk = (uint8_t)(a < 0 ? 0 : (a > pet.trMaxAtk() ? pet.trMaxAtk() : a));
      pet.trDef = (uint8_t)(d < 0 ? 0 : (d > pet.trMaxDef() ? pet.trMaxDef() : d));
      pet.trSpe = (uint8_t)(e < 0 ? 0 : (e > pet.trMaxSpe() ? pet.trMaxSpe() : e));
      pet.saveNow();
    }
    Serial.printf("tr=%u/%u/%u topes=%u/%u/%u\n", pet.trAtk, pet.trDef, pet.trSpe,
                  pet.trMaxAtk(), pet.trMaxDef(), pet.trMaxSpe());
  } else if (line.startsWith("IV ")) {
    // IV <fue> <def> <vel> <vit>: fija los valores individuales (pruebas).
    // Con "IV 31 31 31 31" se ve el techo; con "IV 8 8 8 8" el suelo.
    int v[4] = { 16, 16, 16, 16 };
    int n = sscanf(line.c_str() + 3, "%d %d %d %d", &v[0], &v[1], &v[2], &v[3]);
    if (n >= 1) {
      for (int i = 0; i < 4; i++) v[i] = v[i] < 0 ? 0 : (v[i] > 31 ? 31 : v[i]);
      pet.ivAtk = v[0];
      pet.ivDef = (n >= 2) ? v[1] : v[0];
      pet.ivSpe = (n >= 3) ? v[2] : v[0];
      pet.ivHp = (n >= 4) ? v[3] : v[0];
      if (pet.trAtk > pet.trMaxAtk()) pet.trAtk = pet.trMaxAtk();
      if (pet.trDef > pet.trMaxDef()) pet.trDef = pet.trMaxDef();
      if (pet.trSpe > pet.trMaxSpe()) pet.trSpe = pet.trMaxSpe();
    }
    pet.saveNow();   // IV used to change RAM only and never write
    Serial.printf("iv=%u/%u/%u/%u topes=%u/%u/%u\n", pet.ivAtk, pet.ivDef,
                  pet.ivSpe, pet.ivHp, pet.trMaxAtk(), pet.trMaxDef(), pet.trMaxSpe());
    Serial.println("DONE");
  } else if (line.startsWith("TIME ")) {
    uint32_t e = (uint32_t)line.substring(5).toInt();
    rtcSetEpoch(e);
    pet.setClock(e);
    Serial.printf("rtc=%u\n", rtcEpoch());
    Serial.println("DONE");
  } else if (line.startsWith("RTCSET ")) {  // solo RTC (simular apagados en pruebas)
    rtcSetEpoch((uint32_t)line.substring(7).toInt());
    Serial.printf("rtc=%u\n", rtcEpoch());
    Serial.println("DONE");
  } else if (line == "TIME") {
    Serial.printf("rtc=%u\n", rtcEpoch());
    Serial.println("DONE");
  } else if (line == "SAVECHK") {
    Serial.printf("save tr=%u/%u/%u badges=%u hard=%u region=%u\n",
                  pet.trAtk, pet.trDef, pet.trSpe,
                  pet.badgeCount(false), pet.badgeCount(true), pet.region);
    Serial.println("DONE");
  } else if (line == "GAL") {
    galleryOpen = !galleryOpen;
    galleryDetail = 0;
    galleryDirty = true;
    if (!galleryOpen) galleryPmd.unload();
    Serial.println("DONE");
  } else if (line == "EGGS") {
    // simula 20 tiradas de huevo (no cambia el estado del juego)
    for (int i = 0; i < 20; i++) {
      int16_t d = pet.pickEggSpecies(false);
      Serial.printf("%d:%s(r%u) ", d, DEX_TBL[d].name, DEX_TBL[d].rarity);
    }
    Serial.println();
    Serial.println("DONE");
  } else if (line.startsWith("EGG ")) {
    // EGG <dex> [shiny]: hatch a chosen species right now. The legendary and
    // shiny IV guarantees only apply at hatch time, so this is the only way to
    // exercise them on hardware (SHINY below just toggles the flag afterwards).
    int dex = 0, sh = 0;
    int n = sscanf(line.c_str() + 4, "%d %d", &dex, &sh);
    if (n >= 1 && dex >= 1 && dex <= DEX_COUNT) {
      pet.dbgHatchAs(dex, sh != 0);
      Serial.printf("%s%s iv=%u/%u/%u/%u\n", DEX_TBL[dex].name,
                    pet.shiny ? " *SHINY*" : "", pet.ivAtk, pet.ivDef,
                    pet.ivSpe, pet.ivHp);
    }
    Serial.println("DONE");
  } else if (line.startsWith("BATTLE")) {
    // BATTLE <dex> [level] -- the only way in until the trainer roster exists
    int dex = 0, lvl = 0;
    int n = sscanf(line.c_str() + 6, "%d %d", &dex, &lvl);
    if (n >= 1 && dex >= 1 && dex <= DEX_COUNT) {
      startBattle(dex, lvl > 0 ? (uint8_t)lvl : pet.level());
      Serial.printf("battle vs %s Lv.%u\n", DEX_TBL[dex].name, btlFoe.level);
    } else {
      Serial.println("uso: BATTLE <dex> [nivel]");
    }
    Serial.println("DONE");
  } else if (line == "SHINY") {  // alterna shiny del actual (pruebas)
    pet.shiny = !pet.shiny;
    Serial.printf("shiny=%d\n", pet.shiny);
    Serial.println("DONE");
  } else if (line.startsWith("NICK ")) {
    pet.rename(line.substring(5).c_str());
    Serial.printf("nick=%s\n", pet.nick);
    Serial.println("DONE");
  } else if (line == "CAREDAY") {  // simula un dia nuevo cuidado (pruebas)
    pet.setClock(pet.lastSeenEpoch + 86400);
    pet.caress();
    Serial.printf("streak=%u bond=%u medals=0x%X\n", pet.streak, pet.bond, pet.medals);
    Serial.println("DONE");
  } else if (line == "BYE") {
    pet.startFarewell();
    Serial.println("DONE");
  } else if (line == "RUN") {
    pet.startRunaway();
    Serial.println("DONE");
  } else if (line == "BEEP") {
    sfxPlay(SFX_HATCH);  // prueba de audio
    Serial.println("DONE");
  } else if (line == "ABANDON") {
    pet.dbgRunawayReady();  // fuerza el estado "lista para escaparse" (test del boton)
    Serial.println("DONE");
  } else if (line == "EXPORT") {
    // Prints the whole save as a block of IMPORT commands. Pasting that block
    // back is the restore -- there is no separate format to get wrong, and no
    // single 2000-character line for a terminal to mangle.
    static uint8_t buf[4096];
    size_t n = saveExport(buf, sizeof(buf));
    if (!n) { Serial.println("EXPORT FAIL"); return; }
    Serial.printf("# TamaPoke save, %u bytes. Paste this whole block back.\n",
                  (unsigned)n);
    for (size_t i = 0; i < n; i += 48) {
      Serial.print("IMPORT ");
      for (size_t j = i; j < i + 48 && j < n; j++) Serial.printf("%02X", buf[j]);
      Serial.println();
    }
    Serial.println("IMPORT");        // the empty one commits
  } else if (line.startsWith("IMPORT")) {
    // IMPORT <hex>   append a chunk
    // IMPORT         commit what has been appended
    static uint8_t in[4096];
    static size_t inN = 0;
    String hex = line.substring(6);
    hex.trim();
    if (hex.length()) {
      if (hex.length() & 1) { Serial.println("IMPORT ODD"); inN = 0; return; }
      for (size_t i = 0; i + 1 < (size_t)hex.length(); i += 2) {
        if (inN >= sizeof(in)) { Serial.println("IMPORT FULL"); inN = 0; return; }
        auto nyb = [](char c) -> int {
          if (c >= '0' && c <= '9') return c - '0';
          if (c >= 'A' && c <= 'F') return c - 'A' + 10;
          if (c >= 'a' && c <= 'f') return c - 'a' + 10;
          return -1;
        };
        const char *hs = hex.c_str();
        int hi = nyb(hs[i]), lo = nyb(hs[i + 1]);
        if (hi < 0 || lo < 0) { Serial.println("IMPORT BAD"); inN = 0; return; }
        in[inN++] = (uint8_t)((hi << 4) | lo);
      }
      return;                        // silent while collecting
    }
    if (!inN) { Serial.println("IMPORT EMPTY"); return; }
    bool ok = saveImport(in, inN);
    Serial.println(ok ? "IMPORT OK" : "IMPORT REJECTED");
    inN = 0;
    if (ok) { Serial.println("DONE"); delay(100); ESP.restart(); }
  } else if (line == "WIPE") {
    pet.factoryReset();     // borra NVS y reinicia -> partida nueva (eleccion de inicial)
    Serial.println("DONE");
    delay(100);
    ESP.restart();
  } else if (line.startsWith("PARTY")) {
    // PARTY          list the party
    // PARTY <dex>    bank a level-50 specimen (fills slots up for testing)
    // PARTY CLEAR    empty it
    String arg = line.substring(5);
    arg.trim();
    if (arg == "CLEAR") {
      for (int i = 0; i < PARTY_SLOTS; i++) party.releaseAt(i);
    } else if (arg.length()) {
      int d = arg.toInt();
      if (d >= 1 && d <= DEX_COUNT) {
        PartyMon m;
        m.dex = d;
        m.level = 50;
        m.ivAtk = m.ivDef = m.ivSpe = m.ivHp = 20;
        m.trAtk = m.trDef = m.trSpe = 50;
        Serial.println(party.add(m) ? "added" : "party full");
      }
    }
    Serial.printf("party %u/%u:", party.count(), PARTY_SLOTS);
    for (int i = 0; i < PARTY_SLOTS; i++) {
      const PartyMon &m = party.slots[i];
      if (m.empty()) Serial.print(" -");
      else Serial.printf(" %s%s(nv%u)", creatureName(m.dex), m.shiny ? "*" : "", m.level);
    }
    Serial.println();
    Serial.println("DONE");
  } else if (line == "REG") {
    Serial.printf("pokedex %u/%u:", pet.registeredCount(), regionDexCount(REGION_ALL));
    for (int i = 1; i <= DEX_COUNT; i++)
      if (pet.isRegistered(i)) Serial.printf(" %d", i);
    Serial.println();
    Serial.println("DONE");
  } else if (line == "HEALTH") {
    Serial.printf("up=%lus heap=%u min=%u sd=%d mon=%d\n",
                  (unsigned long)(millis() / 1000), ESP.getFreeHeap(),
                  ESP.getMinFreeHeap(), sdReady, pmd.loaded || mon.loaded);
    // PSRAM is where the sprites and the framebuffer live, so a memory problem
    // shows up here long before it shows up in the heap figure above.
    Serial.printf("psram=%u screen=%s btlspr=%d/%d\n", (unsigned)ESP.getFreePsram(),
                  SCREEN_NAME[uiCurrentScreen() % SCR_COUNT],
                  btlPmd[0].loaded ? 1 : 0, btlPmd[1].loaded ? 1 : 0);
    Serial.println("DONE");
  } else if (line == "STATS") {
    Serial.printf("spec=%d nv=%u com=%u fel=%u ene=%u lim=%u desc=%u sd=%d mon=%d bat=%d mv=%u usb=%d rtc=%u\n",
                  pet.speciesId, pet.level(), pet.fullness, pet.joy, pet.energy,
                  pet.hygiene, pet.careMistakes, sdReady, mon.loaded,
                  batPercent(), batVoltageMv(), usbPresent(), rtcEpoch());
    Serial.printf("peso=%u fue=%u def=%u vel=%u vit=%u baya=%d\n",
                  pet.weight, pet.atkStat(), pet.defStat(), pet.speStat(),
                  pet.vitStat(), pet.berryKnown);
    Serial.printf("iv=%u/%u/%u/%u tr=%u/%u/%u topes=%u/%u/%u\n",
                  pet.ivAtk, pet.ivDef, pet.ivSpe, pet.ivHp,
                  pet.trAtk, pet.trDef, pet.trSpe,
                  pet.trMaxAtk(), pet.trMaxDef(), pet.trMaxSpe());
    Serial.printf("shiny=%d streak=%u/%u bond=%u medals=0x%X(%u) nick=%s\n",
                  pet.shiny, pet.streak, pet.bestStreak, pet.bond, pet.medals,
                  pet.totalMedals, pet.nick);
    Serial.println("DONE");
  }
}

// ---------- entrada tactil ----------

bool inPetZone(int16_t x, int16_t y) {
  return x > 110 && x < 356 && y > 95 && y < 310;
}

// el toque se resuelve al LEVANTAR el dedo para distinguir tap de deslizar
void handleTouch() {
  static uint32_t lastPoll = 0;
  if (millis() - lastPoll < 20) return;  // 50 Hz le sobra a un dedo
  lastPoll = millis();

  // v3.61.5 power-save rule: a dark screen can ONLY be woken by the physical
  // AXP2101 side button. Touch IRQs are drained so the controller does not leave
  // INT latched low, but they never change screenOff/lastInteract and can never
  // trigger a UI action. This also prevents a stale touch from firing just after
  // the physical button wakes the panel.
  if (screenOff) {
    if (gTouchIrq) {
      gTouchIrq = false;
      int16_t dx, dy;
      (void)touch.getPoint(&dx, &dy, 1);
    }
    wasPressed = false;
    swallowGesture = false;
    return;
  }

  // solo tocamos el bus si el chip aviso por INT o si el dedo sigue abajo (hay
  // que detectar el levantamiento). Leer el CST9217 dormido se colgaba ~1s y
  // congelaba el loop entero; SensorLib no respeta el timeout de Wire.
  if (!gTouchIrq && !wasPressed) return;
  gTouchIrq = false;
  int16_t x, y;
  bool pressed = touch.getPoint(&x, &y, 1) > 0;

  // Training games use press-down immediately instead of waiting for finger-up
  // gesture recognition. This removes the extra I2C polling/gesture delay that
  // could feel like a short freeze during fast play. In defence every tap is a
  // gauge stop, so HUD touches cannot accidentally exit the game.
  if (gameOpen) {
    if (pressed && !wasPressed) {
      lastInteract = millis();
      gameTap(x, y);
    }
    wasPressed = gameOpen ? pressed : false;
    return;
  }
  if (spdOpen) {
    if (pressed && !wasPressed) {
      lastInteract = millis();
      spdTap(x, y);
    }
    wasPressed = spdOpen ? pressed : false;
    return;
  }
  if (hpOpen) {
    if (pressed && !wasPressed) {
      lastInteract = millis();
      vitalityTap(x, y);
    }
    wasPressed = hpOpen ? pressed : false;
    return;
  }

  // saco de entrenamiento: cada toque cuenta al instante (aporrear rapido)
  if (sackOpen) {
    if (pressed && !wasPressed) {
      lastInteract = millis();
      if (y < 72) leaveSack();       // tocar arriba = salir, conservando lo ganado
      else sackTap();
    }
    wasPressed = pressed;
    return;
  }

  if (pressed && !wasPressed) {  // empieza el gesto
    tX0 = tXl = x;
    tY0 = tYl = y;
    tStart = millis();
    holdFired = false;
    swallowGesture = (dimStage > 0);  // dimmed panel: first touch only restores brightness
    // UI chirps are intentionally deferred until finger-up/action completion.
    // Some actions save to NVS; queuing a short beep here could lose it while
    // flash/cache work temporarily stalls the audio task.
    lastInteract = millis();
  } else if (pressed) {  // sigue apoyado
    tXl = x;
    tYl = y;
    // pulsacion larga sin moverse sobre el bicho -> dialogo de soltar
    //
    // Gated on the MAIN screen, not on a hand-maintained list of screens to
    // exclude. That list had gallery/card/keyboard/clock on it and nothing
    // else, so the hold still fired on the party, box, gym, battle, player and
    // menu screens -- every one of which draws something inside inPetZone.
    // On the party screen it was genuinely dangerous: the grid overlaps the
    // zone, so holding a party slot opened "release the live pet?", and that
    // dialog's YES box sits on top of party slot 4. Hold a slot, tap where you
    // think a creature is, lose the creature you are actually raising.
    // One question with one answer, so a new screen cannot be forgotten.
    if (!holdFired && !swallowGesture && uiCurrentScreen() == SCR_MAIN && millis() - tStart > 3000 &&
        abs(tXl - tX0) < 30 && abs(tYl - tY0) < 30 && inPetZone(tX0, tY0) &&
        !pet.isEgg() && !confirmUntil && !pet.ceremony) {
      confirmUntil = millis() + 10000;
      holdFired = true;
    }
  } else if (wasPressed) {  // levanta el dedo: resolver gesto
    lastInteract = millis();
    int dx = tXl - tX0, dy = tYl - tY0;
    uint32_t dt = millis() - tStart;
    if (!holdFired && !swallowGesture) {
      if (abs(dx) > 80 && abs(dy) < 70 && dt < 800) onSwipe(dx > 0 ? 1 : -1);
      else if (abs(dy) > 80 && abs(dx) < 70 && dt < 800) onSwipeV(dy > 0 ? 1 : -1);
      else if (dt < 1500 && abs(dx) < 40 && abs(dy) < 40) {
        // One common tactile feedback path for every normal UI tap. Suppress
        // legacy per-screen TAP requests during the action, then chirp exactly
        // once after it has completed (including NVS-writing rest/wake actions).
        audioUiGestureStart();
        onTap(tX0, tY0);
        audioUiPress();
      }
    }
  }
  wasPressed = pressed;
}

// deslizar vertical: abre/cierra la ficha del bicho

void openClock();  // prototipo

void onSwipeV(int dir) {
  if (pet.awaitingStarter()) return;  // bloqueado durante la eleccion de inicial
  if (uiCurrentScreen() == SCR_DEXPICK || uiCurrentScreen() == SCR_GYMPICK)
    return;                 // on the chooser, vertical does nothing: pick a row
  if (menuOpen) { menuOpen = false; return; }   // any swipe closes the menu
  if (digiDexOpen) { digiDexOpen = false; digiOpen = false; return; }
  if (digiOpen) { digiOpen = false; return; }
  if (bagOpen) { bagOpen = false; tmPendingType = T_NONE; tmPendingMove = 0; return; }
  if (missionOpen) { missionOpen = false; return; }
  if (exploreOpen) { exploreOpen = false; adventureOpen = true; return; }
  if (bossOpen) { bossOpen = false; adventureOpen = true; return; }
  if (towerOpen) { towerOpen = false; adventureOpen = true; return; }
  if (treasureOpen) { treasureOpen = false; adventureOpen = true; return; }
  if (rivalOpen) { rivalOpen = false; adventureOpen = true; return; }
  if (adventureOpen) { adventureOpen = false; return; }
  if (battleOpen) return;   // no swiping out of a fight
  if (pickOpen) {
    pickOpen = false;
    if (pickTrainer == PICK_BOSS) bossOpen = true;
    else if (pickTrainer == PICK_LAN) lanOpen = true;
    else { gymRegion = pickRegion; gymOpen = true; }
    return;
  }
  if (lanOpen) { lanLeave(); lanOpen = false; return; }
  if (gymOpen) {
    // Same gesture as the Pokedex: vertical changes region, horizontal pages.
    gymRegion = (uint8_t)((gymRegion + (dir > 0 ? 1 : GYM_REGIONS - 1)) % GYM_REGIONS);
    gymPage = 0;
    sfxPlay(SFX_TAP);
    return;
  }
  if (playerOpen) { playerOpen = false; return; }
  if (trainOpen) { trainOpen = false; return; }
  if (movePickOpen) { movePickOpen = false; return; }
  if (boxOpen) { boxOpen = false; boxSel = 0; return; }   // vertical backs out
  if (partyOpen) {
    if (partyDetail) { partyDetail = 0; return; }
    if (partyPick) { partyPick = false; pet.endedKind = CER_NONE; }
    partyOpen = false;
    return;
  }
  // Either minigame exits on a swipe. A swipe cannot be confused with a ball
  // hit -- the gesture resolver separates them -- which the header tap no
  // longer can now that the ball is hittable up there.
  if (gameOpen) { leaveGame(); return; }
  if (sackOpen) { leaveSack(); return; }
  if (spdOpen) { leaveSpeed(); return; }
  if (hpOpen) { leaveVitality(); return; }
  if (galleryOpen) {
    if (galleryDetail) { galleryDetail = 0; galleryPmd.unload(); galleryDirty = true; return; }
    galleryRegion = (uint8_t)((galleryRegion + (dir > 0 ? 1 : GAL_REGIONS - 1)) % GAL_REGIONS);
    galleryPage = 0;
    galleryDirty = true;
    sfxPlay(SFX_TAP);
    return;
  }
  if (kbOpen || pet.ceremony) return;
  if (clockOpen) { clockOpen = false; return; }
  if (cardOpen) {
    if (dir < 0) cardOpen = false;  // arriba cierra la ficha
    return;
  }
  // Swipe down is the PLAYER card, up is the creature's. The clock lost this
  // gesture on purpose -- the menu's SETTINGS row already opens it, and the
  // player card is the thing you reach for far more often.
  if (dir > 0) {
    if (!confirmUntil && !feedMenuUntil) playerOpen = true;
  } else if (!pet.isEgg() && !confirmUntil && !feedMenuUntil) {
    cardOpen = true;                // deslizar arriba: ficha
    cardPage = 0;
  }
}

// party screen: pick a slot (when a newcomer is waiting) or just leave
// A banked creature's sheet: its moves above all, since typing alone does not
// tell you whether that Lapras still has ICE BEAM -- and in hard mode that is
// what decides the fight.
// The detail sheet, shared by the party and the box so the two cannot drift.
// `fromBox` picks which action the LEFT button offers; the right one is always
// RELEASE, which is irreversible and therefore always asks first.
void renderMonSheet(const PartyMon &m, bool fromBox) {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  char head[48];
  snprintf(head, sizeof(head), "%s%s Lv.%u", m.shiny ? "*" : "",
           m.nick[0] ? m.nick : creatureName(m.dex), (unsigned)m.level);
  gfx->setTextColor(RGB565_BLACK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(head, 2), 40);
  gfx->print(head);
  char ty[24];
  uint8_t t1=creatureType1(m.dex),t2=creatureType2(m.dex);
  if (t2 == T_NONE) snprintf(ty, sizeof(ty), "%s", localizedTypeName(t1));
  else snprintf(ty, sizeof(ty), "%s/%s", localizedTypeName(t1), localizedTypeName(t2));
  gfx->setTextColor(UI_TRACK);
  uiSetTextSize(1);
  uiSetCursor(CX - uiTextHalfWidth(ty, 1), 64);
  gfx->print(ty);

  for (int i = 0; i < MOVE_SLOTS; i++)
    drawMoveRow(78 + i * 52, m.moves[i], false, m.dex);

  char st[40];
  snprintf(st, sizeof(st), "ATK %u  DEF %u  SPD %u  HP %u",
           party.atkOf(m), party.defOf(m), party.speOf(m), party.vitOf(m));
  gfx->setTextColor(UI_INK);
  uiSetTextSize(1);
  uiSetCursor(CX - uiTextHalfWidth(st, 1), 300);
  gfx->print(st);

  uint8_t pmPers = personalityIdFor(m.dex, m.ivAtk, m.ivDef, m.ivSpe, m.ivHp);
  char pmPs[64];
  snprintf(pmPs, sizeof(pmPs), "성격: %s · %s", personalityNameKo(pmPers), personalityEffectKo(pmPers));
  gfx->setTextColor(UI_TRACK);
  uiSetTextSize(1);
  uiSetCursor(CX - uiTextHalfWidth(pmPs, 1), 318);
  gfx->print(pmPs);

  // Bringing one back is only offered while an egg is waiting. Otherwise it
  // would silently destroy whatever creature is currently alive, and a rule the
  // player cannot see is worse than a button they cannot press. A box creature
  // goes to the party instead, which is always allowed if there is room.
  bool leftOk = fromBox ? (party.firstFree() >= 0)
                        : (pet.isEgg() && !pet.awaitingStarter());
  const char *leftLbl = fromBox ? T(S_BOX_TAKE) : T(S_REVIVE);
  gfx->fillRoundRect(PDET_L_X, PDET_BTN_Y, PDET_L_W, PDET_BTN_H, 10,
                     leftOk ? UI_BAR_OK : UI_TRACK);
  gfx->drawRoundRect(PDET_L_X, PDET_BTN_Y, PDET_L_W, PDET_BTN_H, 10, UI_INK);
  gfx->setTextColor(leftOk ? UI_BG_DAY : 0x8410);
  uiSetTextSize(2);
  uiSetCursor(PDET_L_X + PDET_L_W / 2 - uiTextHalfWidth(leftLbl, 2),
                 PDET_BTN_Y + PDET_BTN_H / 2 - 8);
  gfx->print(leftLbl);

  gfx->fillRoundRect(PDET_R_X, PDET_BTN_Y, PDET_R_W, PDET_BTN_H, 10, UI_BAR_BAD);
  gfx->drawRoundRect(PDET_R_X, PDET_BTN_Y, PDET_R_W, PDET_BTN_H, 10, UI_INK);
  gfx->setTextColor(UI_WHITE);
  uiSetTextSize(1);
  uiSetCursor(PDET_R_X + PDET_R_W / 2 - uiTextHalfWidth(T(S_RELEASE_BTN), 1),
                 PDET_BTN_Y + PDET_BTN_H / 2 - 4);
  gfx->print(T(S_RELEASE_BTN));

  if (!leftOk && !fromBox) {
    gfx->setTextColor(UI_TRACK);
    uiSetTextSize(1);
    uiSetCursor(CX - uiTextHalfWidth(T(S_REVIVE_EGG), 1), PDET_BTN_Y + PDET_BTN_H + 4);
    gfx->print(T(S_REVIVE_EGG));
  }
  gfx->setTextColor(UI_TRACK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(T(S_BACK), 2), 404);
  gfx->print(T(S_BACK));

  // Asked before it happens, because nothing gets this creature back: it is not
  // a farewell, it does not join anything, and there is no undo.
  if (releaseConfirm) {
    char q[96];
    snprintf(q, sizeof(q), T(S_RELEASE_FMT), m.nick[0] ? m.nick : creatureName(m.dex));
    drawConfirmPanel(q, T(S_RELEASE_GONE), nullptr, UI_BAR_BAD,
                     T(S_YES), UI_BAR_BAD, UI_WHITE, T(S_NO), UI_TRACK, UI_INK);
  }
  gfx->flush();
}

void renderPartyDetail() {
  // An empty slot means the creature was just let go. Fall through to the grid
  // rather than returning: a bare return draws nothing AND never flushes, which
  // leaves the previous frame frozen on the panel -- the exact failure
  // flush_test exists to catch, and invisible to any screenshot.
  if (party.slots[partyDetail - 1].empty()) {
    partyDetail = 0;
    releaseConfirm = false;
    renderParty();
    return;
  }
  renderMonSheet(party.slots[partyDetail - 1], false);
}

void renderBoxDetail() {
  if (party.box[boxDetail - 1].empty()) {
    boxDetail = 0;
    releaseConfirm = false;
    renderBox();
    return;
  }
  renderMonSheet(party.box[boxDetail - 1], true);
}

// The two buttons' hit areas, so a test can prove they do not overlap without
// copying the geometry -- a test that restates the numbers drifts from them.
// Every primary button's height, so a test can hold them all to UI_TAP_MIN
// instead of waiting for somebody to report the next one by hand.
void uiButtonHeights(int *out, int max, int *n) {
  const int h[] = { BOXBTN_H, PARTYCLOSE_H, LANBTN_H, BTL_CELL_H + BTL_HIT_PAD * 2 };
  int c = (int)(sizeof(h) / sizeof(h[0]));
  if (c > max) c = max;
  for (int i = 0; i < c; i++) out[i] = h[i];
  if (n) *n = c;
}

// The gym list's difficulty pill against its first leader row. Enlarging the
// pill to a real tap target once pushed it straight over that row -- the third
// overlap of this kind, after BOX/CLOSE and the battle grid against BACK.
// Which home icon is the only one live while the pet sleeps, and where it is.
// Exposed so a test can prove it is the LIGHT: removing an icon once shifted
// every index and quietly made the BATH button the wake-up button.
// Asleep, the LIGHT is the only live icon -- it is what wakes the pet. Both the
// draw path and the tap path ask THIS, so a greyed button can never still be
// tappable and the greying can never point at the wrong icon.
bool uiButtonDisabled(int i) { return pet.sleeping && i != BTN_LIGHT; }

int uiSleepButton(int *cx, int *cy) {
  if (cx) *cx = buttons[BTN_LIGHT].cx;
  if (cy) *cy = buttons[BTN_LIGHT].cy;
  return BTN_LIGHT;
}

void uiButtonAt(int i, int *cx, int *cy, int *half) {
  if (i < 0 || i >= BTN_COUNT) return;
  if (cx) *cx = buttons[i].cx;
  if (cy) *cy = buttons[i].cy;
  if (half) *half = BTN_HALF;
}

void gymHeaderRects(int *pillTop, int *pillBot, int *rowTop) {
  if (pillTop) *pillTop = GYMDIF_Y;
  if (pillBot) *pillBot = GYMDIF_Y + GYMDIF_H;
  if (rowTop) *rowTop = GYM_ROW_Y(0);
}

void partyButtonRects(int *boxTop, int *boxBot, int *closeTop, int *closeBot) {
  if (boxTop) *boxTop = BOXBTN_Y - BOXBTN_PAD;
  if (boxBot) *boxBot = BOXBTN_Y + BOXBTN_H + BOXBTN_PAD;
  if (closeTop) *closeTop = PARTYCLOSE_Y;
  if (closeBot) *closeBot = PARTYCLOSE_Y + PARTYCLOSE_H;
}

// Which of the detail sheet's two buttons a tap landed on. One answer for the
// party and the box, off the same PDET_* constants the sheet is DRAWN with, so
// the graphic and the hit area cannot drift apart.
bool monSheetBtn(int16_t x, int16_t y, bool left) {
  if (y < PDET_BTN_Y || y > PDET_BTN_Y + PDET_BTN_H) return false;
  int x0 = left ? PDET_L_X : PDET_R_X;
  int w = left ? PDET_L_W : PDET_R_W;
  return x >= x0 && x <= x0 + w;
}

// YES / NO on the release confirm. Returns true when the tap was consumed. The
// caller swallows everything else while it is up: a modal that leaks a miss
// through to what is drawn underneath is precisely how an irreversible action
// gets triggered by a fumbled tap, which is the shape CLAUDE.md section 4 keeps
// warning about.
bool monSheetConfirmTap(int16_t x, int16_t y, bool fromBox) {
  int c1t, c1b, c2t, c2b;
  uiConfirmRects(&c1t, &c1b, &c2t, &c2b);
  if (x < CONFIRM_BTN_X || x > CONFIRM_BTN_X + CONFIRM_BTN_W) return false;
  if (y >= c1t && y <= c1b) {            // YES -- and it does not come back
    if (fromBox) { party.boxReleaseAt(boxDetail - 1); boxDetail = 0; }
    else { party.releaseAt(partyDetail - 1); partyDetail = 0; }
    // A half-finished swap named a slot that may now be empty, so disarm both
    // sides rather than leaving one pointing at a creature that is gone.
    boxSwapFrom = 0;
    boxSel = 0;
    releaseConfirm = false;
    sfxPlay(SFX_BYE);
    return true;
  }
  if (y >= c2t && y <= c2b) {            // NO
    releaseConfirm = false;
    sfxPlay(SFX_TAP);
    return true;
  }
  return false;
}

void partyTap(int16_t x, int16_t y) {
  if (boxOpen) { boxTap(x, y); return; }
  // The SHEET IS CHECKED FIRST, and that is not cosmetic ordering. It covers the
  // whole screen, and its RELEASE button (x240..370, y336..384) lands inside the
  // BOX button's hit area (x138..328, y312..372) below -- so with BOX tested
  // first, tapping RELEASE opened the box instead. Nothing behind a full-screen
  // sheet may answer a tap.
  if (partyDetail) {
    // The confirm is modal: while it is up nothing else on the sheet responds,
    // or a miss on YES would fall through to the move rows underneath it.
    if (releaseConfirm) { monSheetConfirmTap(x, y, false); return; }
    if (monSheetBtn(x, y, true)) {           // BRING BACK
      if (!pet.isEgg() || pet.awaitingStarter()) { sfxPlay(SFX_DENY); return; }
      pet.reviveFrom(party.slots[partyDetail - 1]);
      party.releaseAt(partyDetail - 1);      // it is alive now, not banked
      partyDetail = 0;
      boxSwapFrom = 0;
      partyOpen = false;
      sfxPlay(SFX_HATCH);
      return;
    }
    if (monSheetBtn(x, y, false)) {          // RELEASE -- ask first, always
      releaseConfirm = true;
      sfxPlay(SFX_TAP);
      return;
    }
    for (int i = 0; i < MOVE_SLOTS; i++) {   // tap a move to change it
      int ry = 78 + i * 52;
      if (x < 70 || x > 396 || y < ry || y > ry + 50) continue;
      movePickParty = partyDetail;
      movePickSlot = i;
      movePickPage = 0;
      movePickOpen = true;
      sfxPlay(SFX_TAP);
      return;
    }
    partyDetail = 0;
    releaseConfirm = false;
    sfxPlay(SFX_TAP);
    return;
  }
  if (!partyPick && y >= BOXBTN_Y - BOXBTN_PAD && y <= BOXBTN_Y + BOXBTN_H + BOXBTN_PAD &&
      x >= BOXBTN_X - BOXBTN_PAD && x <= BOXBTN_X + BOXBTN_W + BOXBTN_PAD) {
    boxOpen = true;                  // open the box, nothing picked yet
    boxPage = 0;
    boxSwapFrom = 0;
    sfxPlay(SFX_TAP);
    return;
  }
  // exit button, and the top band, both always work
  if ((y >= 372 && y <= 416 && x >= 133 && x <= 333) || y < 34) {
    if (partyPick) {                 // declined the swap: the pet is let go
      partyPick = false;
      pet.endedKind = CER_NONE;
    }
    partyOpen = false;
    sfxPlay(SFX_TAP);
    return;
  }
  for (int i = 0; i < PARTY_SLOTS; i++) {
    int cx0 = PARTY_GRID_X + (i % 2) * (PARTY_CELL_W + 10);
    int cy0 = PARTY_GRID_Y + (i / 2) * (PARTY_CELL_H + 8);
    if (x < cx0 || x > cx0 + PARTY_CELL_W || y < cy0 || y > cy0 + PARTY_CELL_H) continue;
    if (boxSel) {                    // a box creature is waiting for a slot
      party.swapPartyBox(i, boxSel - 1);
      boxSel = 0;
      sfxPlay(SFX_MEDAL);
      return;
    }
    if (!partyPick) {
      if (boxSwapFrom == i + 1) {    // tapped again: take it to the box
        boxOpen = true;
        boxPage = 0;
        sfxPlay(SFX_TAP);
        return;
      }
      if (party.slots[i].empty()) { boxSwapFrom = i + 1; sfxPlay(SFX_TAP); return; }
      partyDetail = i + 1;
      boxSwapFrom = i + 1;           // armed, in case the box is opened next
      sfxPlay(SFX_TAP);
      return;
    }
    party.replaceAt(i, pet.endedMon);
    snprintf(partyBannerName, sizeof(partyBannerName), "%s",
             pet.endedMon.nick[0] ? pet.endedMon.nick : creatureName(pet.endedMon.dex));
    partyBannerUntil = millis() + 3500;
    pet.endedKind = CER_NONE;
    partyPick = false;
    partyOpen = false;
    sfxPlay(SFX_MEDAL);
    return;
  }
}

// deslizar: dir +1 = hacia la derecha
void onSwipe(int dir) {
  // The region chooser pages, and it is checked before everything else because
  // it sits on TOP of the starter/gallery/gym screens -- each of which has its
  // own horizontal handler that would otherwise swallow the gesture. Paging a
  // screen by closing it is the bug this project shipped four times.
  if (rpickSwipe(dir)) return;
  if (pet.awaitingStarter()) return;  // bloqueado durante la eleccion de inicial
  if (menuOpen) { menuOpen = false; return; }   // any swipe closes the menu
  if (digiDexOpen) {
    int p = (int)digiDexPage + (dir > 0 ? -1 : 1);
    uint8_t pages = (uint8_t)((DIGI_SPECIES_COUNT + 7) / 8);
    if (p < 0 || p >= pages) { digiDexOpen = false; digiOpen = false; }
    else digiDexPage = (uint8_t)p;
    return;
  }
  if (digiOpen) { digiOpen = false; return; }
  if (bagOpen) {
    if (tmPendingMove && tmPendingMove < MOVE_COUNT) { tmPendingMove = 0; tmPendingType = T_NONE; return; }
    uint8_t pages = bagTab == 0 ? 3 : 5;
    int p = (int)bagPage + (dir > 0 ? -1 : 1);
    if (p < 0 || p >= pages) bagOpen = false; else bagPage = (uint8_t)p;
    return;
  }
  if (missionOpen) { missionOpen = false; return; }
  if (exploreOpen) {
    if (!extras.explorationActive() && !extras.explorationReady()) {
      int p = (int)explorePage + (dir > 0 ? -1 : 1);
      if (p < 0 || p > 2) { exploreOpen = false; adventureOpen = true; }
      else explorePage = (uint8_t)p;
    } else { exploreOpen = false; adventureOpen = true; }
    return;
  }
  if (bossOpen) { bossOpen = false; adventureOpen = true; return; }
  if (towerOpen) { towerOpen = false; adventureOpen = true; return; }
  if (treasureOpen) { treasureOpen = false; adventureOpen = true; return; }
  if (rivalOpen) { rivalOpen = false; adventureOpen = true; return; }
  if (adventureOpen) { adventureOpen = false; return; }
  if (battleOpen) return;   // no swiping out of a fight
  if (pickOpen) {   // horizontal pages the candidates, as everywhere else
    uint8_t pages = (pickCandidates() + PICK_PER_PAGE - 1) / PICK_PER_PAGE;
    if (!pages) pages = 1;
    int p = (int)pickPage + (dir > 0 ? -1 : 1);
    if (p < 0 || p >= pages) {
      pickOpen = false;
      if (pickTrainer == PICK_BOSS) bossOpen = true;
      else if (pickTrainer == PICK_LAN) lanOpen = true;
      else { gymRegion = pickRegion; gymOpen = true; }
    }
    else pickPage = (uint8_t)p;
    return;
  }
  if (gymOpen) {   // horizontal pages the ladder; vertical backs out
    uint8_t pages = (TRAINER_COUNT + GYM_ROWS - 1) / GYM_ROWS;
    int p = (int)gymPage + (dir > 0 ? -1 : 1);
    if (p < 0 || p >= pages) { gymPick = true; rpickPage = 0; }  // back to the chooser
    else gymPage = (uint8_t)p;
    return;
  }
  if (playerOpen) {   // horizontal pages it, like the card and the gallery
    int p = (int)playerPage + (dir > 0 ? -1 : 1);
    if (p < 0 || p >= PLAYER_PAGES) playerOpen = false;
    else playerPage = (uint8_t)p;
    return;
  }
  if (trainOpen) { trainOpen = false; return; }
  if (movePickOpen) {   // the picker is paged; without this its later pages
    uint8_t all[64];    // were simply unreachable
    uint8_t n = learnableList(all, sizeof(all));
    uint8_t pages = n ? (n + MOVE_PICK_PER_PAGE - 1) / MOVE_PICK_PER_PAGE : 1;
    int p = (int)movePickPage + (dir > 0 ? -1 : 1);
    if (p < 0 || p >= pages) movePickOpen = false;
    else movePickPage = (uint8_t)p;
    return;
  }
  if (boxOpen) {   // horizontal pages the box, as every other paged screen
    uint8_t pages = BOX_SLOTS / BOX_PER_PAGE;
    int p = (int)boxPage + (dir > 0 ? -1 : 1);
    if (p < 0 || p >= pages) { boxOpen = false; boxSel = 0; }
    else boxPage = (uint8_t)p;
    return;
  }
  if (partyOpen) {
    if (partyDetail) { partyDetail = 0; return; }
    if (partyPick) { partyPick = false; pet.endedKind = CER_NONE; }
    partyOpen = false;
    return;
  }
  if (gameOpen) { leaveGame(); return; }   // swipe out, keeping what you earned
  if (spdOpen) { leaveSpeed(); return; }
  if (hpOpen) { leaveVitality(); return; }
  if (kbOpen || clockOpen) return;
  if (cardOpen) {  // dentro de la ficha: cambiar entre las 4 paginas
    int p = (int)cardPage + (dir > 0 ? -1 : 1);  // izquierda avanza
    cardPage = p < 0 ? 0 : (p > CARD_PAGES - 1 ? CARD_PAGES - 1 : p);
    return;
  }
  if (!galleryOpen) {
    // Swipe LEFT is the gym ladder, RIGHT is the party. The Pokedex lost this
    // gesture: it has a menu row, and gestures are worth more spent on screens
    // without one.
    if (!pet.ceremony && !confirmUntil) {
      if (dir < 0) { gymOpen = true; gymPick = true; gymPage = 0; rpickPage = 0; }
      else partyOpen = true;
    }
    return;
  }
  if (galleryDetail) {  // en detalle: volver a la rejilla
    galleryDetail = 0;
    gGalleryPanelDex = -1;
    galleryPmd.unload();
    galleryDirty = true;
    return;
  }
  int np = galleryPage - dir;  // deslizar a la izquierda avanza pagina
  if (np < 0) {                // back past the first page = the region chooser
    galleryPick = true;
    rpickPage = 0;
    galleryPmd.unload();
    return;
  }
  if (np > GAL_PAGES - 1) np = GAL_PAGES - 1;
  if (np != galleryPage) {
    galleryPage = np;
    galleryDirty = true;
  }
}

void onTap(int16_t x, int16_t y) {
  if (pet.awaitingStarter()) {  // primera partida: region y luego inicial
    if (!starterRegionDone) {
      int r = regionPickTap(x, y, RPICK_FOR_START);
      if (r >= 0) {
        if (r >= GAL_REGIONS) {
          uint8_t slot=(uint8_t)(r-GAL_REGIONS);
          pet.setDigimonVersion(slot<5?slot+1:(slot==5?10:slot+5));
          starterRegionDone = false;
        } else {
          pet.setRegion((uint8_t)r);
          starterRegionDone = true;
        }
        sfxPlay(SFX_TAP);
      }
      return;
    }
    for (int i = 0; i < starterCountShown(pet.region); i++) {
      int ry = STARTER_ROW_Y + i * (STARTER_ROW_H + STARTER_ROW_GAP);
      if (x >= 70 && x <= 396 && y >= ry && y <= ry + STARTER_ROW_H) {
        pet.chooseStarter(starterOf(pet.region, (uint8_t)i));
        sfxPlay(SFX_TAP);
        break;
      }
    }
    return;
  }
  if (battleOpen) {
    battleTap(x, y);
    return;
  }
  // Like render(), touch gives the modal picker priority over every source
  // screen so an old box/gym/boss flag cannot steal the selection tap.
  if (pickOpen) {
    pickTap(x, y);
    return;
  }
  if (bagOpen) { bagTap(x, y); return; }
  if (missionOpen) { missionTap(x, y); return; }
  if (exploreOpen) { exploreTap(x, y); return; }
  if (bossOpen) { bossTap(x, y); return; }
  if (towerOpen) { towerTap(x, y); return; }
  if (treasureOpen) { treasureTap(x, y); return; }
  if (rivalOpen) { rivalTap(x, y); return; }
  if (adventureOpen) { adventureTap(x, y); return; }
  if (playerOpen) {
    if (playerPage == 0 && y >= 32 && y < 68) {   // the name: rename yourself
      openKeyboardFor(KB_TRAINER);
      sfxPlay(SFX_TAP);
      return;
    }
    // the avatar is the only other live target; everything else backs out
    if (playerPage == 0 && x > CX - 40 && x < CX + 40 && y > 70 && y < 146) {
      pet.avatar = (uint8_t)((pet.avatar + 1) % AVATAR_COUNT);
      pet.flushSave();
      sfxPlay(SFX_TAP);
      return;
    }
    playerOpen = false;
    return;
  }
  if (lanOpen) {
    lanTap(x, y);
    return;
  }
  if (gymOpen && gymPick) {
    int r = regionPickTap(x, y, RPICK_FOR_GYMS);
    if (r >= 0) {
      gymRegion = (uint8_t)r;
      gymPage = 0;
      gymPick = false;
      sfxPlay(SFX_TAP);
      return;
    }
    if (y >= LANBTN_Y && y <= LANBTN_Y + LANBTN_H &&
        x >= LANBTN_X && x <= LANBTN_X + LANBTN_W) {   // LAN battle
      gymOpen = false; gymPick = false;
      lan.state = LINK_OFF;
      lanOpen = true;
      sfxPlay(SFX_TAP);
      return;
    }
    if (y > 380) { gymOpen = false; gymPick = false; }
    return;
  }
  if (gymOpen) {
    if (y >= GYMDIF_Y && y <= GYMDIF_Y + GYMDIF_H) {   // the difficulty pill
      gymHard = !gymHard;
      sfxPlay(SFX_TAP);
      return;
    }
    if (y >= 380 && y <= 412 && x >= 148 && x <= 318) {   // LAN battle
      gymOpen = false;
      lan.state = LINK_OFF;
      lanOpen = true;
      sfxPlay(SFX_TAP);
      return;
    }
    for (int i = 0; i < GYM_ROWS; i++) {
      uint8_t idx = gymPage * GYM_ROWS + i;
      if (idx >= TRAINER_COUNT) break;
      int ry = GYM_ROW_Y(i);
      if (x < 70 || x > 396 || y < ry || y > ry + 44) continue;
      if (!gymUnlocked(idx, gymHard)) { sfxPlay(SFX_DENY); return; }
      sfxPlay(SFX_TAP);
      gymOpen = false;
      pickTrainer = idx;
      pickRegion = gymRegion;   // freeze the ladder region before leaving the gym list
      pickHard = gymHard;
      pickPage = 0;
      pickSourceTab = 0;
      pickDefault(squadCapForRegion(pickRegion, idx, gymHard));
      partyOpen = false; boxOpen = false; bossOpen = false; adventureOpen = false;
      pickOpen = true;
      return;
    }
    gymOpen = false;
    return;
  }
  // Training UI has visual priority over a move-learning offer. Keep touch
  // priority identical so a level-up during training cannot make an invisible
  // learn dialog steal the next training tap.
  if (trainOpen) {
    bool inPanel = (x >= TRAIN_X && x <= TRAIN_X + TRAIN_W &&
                    y >= TRAIN_Y && y <= TRAIN_Y + TRAIN_H);
    if (!inPanel) { trainOpen = false; return; }   // tap outside = back to the pet
    for (int i = 0; i < 4; i++) {
      int ry = TRAIN_ROW_Y(i);
      if (x < TRAIN_X + 18 || x > TRAIN_X + TRAIN_W - 18) continue;
      if (y < ry || y > ry + TRAIN_ROW_H) continue;
      sfxPlay(SFX_TAP);
      trainOpen = false;
      if (i == 0) startSack();
      else if (i == 1) startSpeedGame();
      else if (i == 2) startGame();
      else startVitalityGame();
      return;
    }
    return;
  }
  if (pet.hasLearnOffer()) {
    for (int i = 0; i < MOVE_SLOTS; i++) {
      int ry = LEARN_ROW_Y(i);
      if (x < 70 || x > 396 || y < ry || y > ry + 50) continue;
      sfxPlay(SFX_TAP);
      pet.acceptLearn(i);
      return;
    }
    if (x >= 70 && x <= 396 && y >= LEARN_SKIP_Y && y <= LEARN_SKIP_Y + 44) {
      sfxPlay(SFX_TAP);
      pet.declineLearn();
    }
    return;   // modal: nothing else on screen responds until it is answered
  }
  // The menu is modal and has three independent ways out: the CLOSE row, a tap
  // anywhere on the dimmed area outside the panel, and any swipe (see onSwipe).
  // Deliberately no timeout: a menu that vanishes while you read it is worse
  // than one that lingers.
  if (menuOpen) {
    bool inPanel = (x >= MENU_X && x <= MENU_X + MENU_W &&
                    y >= MENU_Y && y <= MENU_Y + MENU_H);
    if (!inPanel) { menuOpen = false; return; }   // tap outside = back to the pet
    for (int i = 0; i < MENU_ROWS; i++) {
      int ry = MENU_ROW_Y(i);
      if (x < MENU_X + 18 || x > MENU_X + MENU_W - 18) continue;
      if (y < ry || y > ry + MENU_ROW_H) continue;
      sfxPlay(SFX_TAP);
      menuOpen = false;
      if (i == 0) { cardOpen = true; cardPage = 1; }
      else if (i == 1) { galleryOpen = true; galleryPick = true; galleryPage = 0; rpickPage = 0; galleryDetail = 0; galleryDirty = true; }
      else if (i == 2) { missionOpen = true; extras.ensureDaily(pet); }
      else if (i == 3) { adventureOpen = true; }
      else if (i == 4) { openClock(); }
      else if (i == 5) { digiDexOpen = true; digiDexPage = 0; digiDexDetail = -1; }
      else if (i == 6) {
        if (!pet.canRetireNow()) { sfxPlay(SFX_DENY); return; }
        choiceKind = 3; choiceUntil = millis() + 12000;
      }
      return;                                     // i == 7 is CLOSE
    }
    return;
  }
  if (digiDexOpen) { digiDexTap(x, y); return; }
  if (digiOpen) { digiOpen = false; return; } // legacy screen is no longer a second raising mode
  if (movePickOpen) {
    uint8_t all[64];
    uint8_t n = learnableList(all, sizeof(all));
    for (uint8_t i = 0; i < MOVE_PICK_PER_PAGE; i++) {
      uint8_t idx = movePickPage * MOVE_PICK_PER_PAGE + i;
      if (idx >= n) break;
      int ry = MOVE_PICK_Y(i);
      if (x < 70 || x > 396 || y < ry || y > ry + 50) continue;
      sfxPlay(SFX_TAP);
      // Swapping for a move already in another slot would silently duplicate
      // it, so trade the two slots instead of overwriting.
      uint8_t *tgt = pickTargetMoves();
      for (int s = 0; s < MOVE_SLOTS; s++)
        if (tgt[s] == all[idx] && s != movePickSlot) tgt[s] = tgt[movePickSlot];
      tgt[movePickSlot] = all[idx];
      if (movePickParty) party.save(); else pet.flushSave();
      movePickOpen = false;
      return;
    }
    movePickOpen = false;   // tap anywhere else = back to the moves page
    return;
  }
  if (partyOpen) {
    partyTap(x, y);
    return;
  }
  if (galleryOpen) {
    if (galleryPick) {
      int r = regionPickTap(x, y, RPICK_FOR_DEX);
      if (r >= 0) {
        galleryRegion = (uint8_t)r;
        galleryPage = 0;
        galleryDetail = 0;
        galleryDirty = true;
        galleryPick = false;
        sfxPlay(SFX_TAP);
      } else if (y > 380) {
        galleryOpen = false;
      }
      return;
    }
    galleryTap(x, y);
    return;
  }
  if (kbOpen) {
    keyboardTap(x, y);
    return;
  }
  if (clockOpen) {
    clockTap(x, y);
    return;
  }
  if (pet.ceremony) return;  // durante la despedida no hay botones
  if (cardOpen) {
    if (choiceKind == 1) {
      int c1t, c1b, c2t, c2b;
      uiConfirmRects(&c1t, &c1b, &c2t, &c2b);
      const bool inX = x >= CONFIRM_BTN_X && x <= CONFIRM_BTN_X + CONFIRM_BTN_W;
      bool evolveYes = inX && y >= c1t && y <= c1b;
      bool evolveLater = inX && y >= c2t && y <= c2b;
      if (evolveYes) {
        int16_t old = pet.speciesId;
        bool wasDigimon = pet.currentIsDigimon();
        pet.evolve();
        if (wasDigimon) {
          if (evoPmd.loaded) evoPmd.unload();
        } else {
          evoPmd.load(old, pet.shiny);
        }
        // The evolution animation is a home-screen scene. Close the stats card
        // immediately so the player sees the transformation rather than the
        // card covering all five seconds of it.
        cardOpen = false;
      } else if (evolveLater) {
        pet.declineEvolve();
      }
      if (evolveYes || evolveLater) choiceKind = 0;
      return;
    }
    if (cardPage == 0 && y < 84) openKeyboard();  // tocar el nombre = renombrar
    else if (cardPage == 1 && pet.wantEvolveButton() &&
             x >= CARD_EVO_X && x <= CARD_EVO_X + CARD_EVO_W &&
             y >= CARD_EVO_Y && y <= CARD_EVO_Y + CARD_EVO_H) {
      choiceKind = 1;
      choiceUntil = millis() + 12000;
      sfxPlay(SFX_TAP);
    }
    else if (cardPage == 2) {
      for (int i = 0; i < MOVE_SLOTS; i++) {   // tap a slot to change it
        int ry = MOVE_ROW_Y(i);
        if (x < 70 || x > 396 || y < ry || y > ry + 50) continue;
        sfxPlay(SFX_TAP);
        movePickParty = 0;      // the live pet
        movePickSlot = i;
        movePickPage = 0;
        movePickOpen = true;
        return;
      }
      cardOpen = false;            // anywhere else on the page still exits
    } else {
      cardOpen = false;
    }
    return;
  }
  if (spdOpen) {
    spdTap(x, y);
    return;
  }
  if (gameOpen) {
    gameTap(x, y);
    return;
  }
  if (extras.hasEvent()) {
    extras.claimEvent();
    sfxPlay(SFX_MEDAL);
    return;
  }
  if (choiceKind) {          // dialogo de decision: boton accion (arriba) / mantener (abajo)
    int c1t, c1b, c2t, c2b;
    uiConfirmRects(&c1t, &c1b, &c2t, &c2b);
    const bool inX = (x >= CONFIRM_BTN_X && x <= CONFIRM_BTN_X + CONFIRM_BTN_W);
    bool b1 = inX && y >= c1t && y <= c1b;   // accion
    bool b2 = inX && y >= c2t && y <= c2b;   // mantener / quedaros
    if (choiceKind == 1) {                 // evolucion
      if (b1) {
        int16_t old = pet.speciesId;
        bool wasDigimon = pet.currentIsDigimon();
        pet.evolve();
        if (wasDigimon) { if (evoPmd.loaded) evoPmd.unload(); }
        else evoPmd.load(old, pet.shiny);
      }
      else if (b2) pet.declineEvolve();
    } else if (choiceKind == 3) {          // retirada a peticion
      if (b1) pet.startRetire();
      // b2 is simply "no": nothing to decline, the row is always there
    } else if (choiceKind == 2) {          // despedida
      if (b1) pet.startFarewell();
      else if (b2) pet.declineFarewell();
    }
    choiceKind = 0;
    return;
  }
  if (confirmUntil) {        // dialogo "soltar?": SI / NO
    if (millis() < confirmUntil && x >= 118 && x <= 218 && y >= 252 && y <= 304) {
      pet.release();
    }
    confirmUntil = 0;
    return;
  }
  if (feedMenuUntil) {       // selector rapido: 4 comidas + items + TM
    if (millis() < feedMenuUntil && y >= 286 && y <= 354 && x >= 52 && x <= 414) {
      int slot = (x - 56) / 59;
      if (slot < 0) slot = 0;
      if (slot > 5) slot = 5;
      if (slot < 4) {
        if (slot == 3) pet.feedCandy();
        else pet.feedBerry((uint8_t)slot);
        feedMenuUntil = 0;       // one food per opening, like the original UI
        sfxPlay(SFX_EAT);
      } else {
        feedMenuUntil = 0;
        bagOpen = true;
        bagTab = (slot == 5) ? 1 : 0;
        bagPage = 0;
        tmPendingType = T_NONE;
        tmPendingMove = 0;
        sfxPlay(SFX_TAP);
      }
    } else {
      feedMenuUntil = 0;
    }
    return;
  }
  if (careTabTap(x, y)) return;
  if (pet.isEgg()) {
    // the region pill first, or choosing a region would also crack the egg --
    // and a near miss is swallowed rather than counted, since three taps hatch
    if (eggRegionTap(x, y)) return;
    pet.eggTap();
    sfxPlay(SFX_TAP);
    return;
  }
  // The main screen never prompts for a good farewell/retirement anymore.
  // A neglected creature can still expose the runaway CTA, but voluntary
  // endings are opened ONLY from Menu -> 좋은 이별 / 은퇴. This keeps the
  // companion from periodically asking to leave while the player still wants
  // to keep raising it.
  if (x >= FAR_BTN_X && x <= FAR_BTN_X + FAR_BTN_W &&
      y >= FAR_BTN_Y && y <= FAR_BTN_Y + FAR_BTN_H) {
    if (pet.canRunawayNow()) { pet.startRunaway(); return; }
  }
  for (int i = 0; i < BTN_COUNT; i++) {
    int dx = x - buttons[i].cx, dy = y - buttons[i].cy;
    if (dx * dx + dy * dy <= BTN_HIT * BTN_HIT) {
      if (uiButtonDisabled(i)) { sfxPlay(SFX_DENY); return; }
      sfxPlay(SFX_TAP);
      if (i == BTN_FOOD) { feedMenuUntil = millis() + 6000; }
      else if (i == BTN_LIGHT) pet.toggleLight();
      else if (i == BTN_BATH) startBath();
      else trainOpen = true;
      return;
    }
  }
  // tapping the name/status band opens the menu. This band was inert before,
  // and it sits clear of inPetZone (which starts at y 95).
  if (y >= 28 && y < 94) {
    menuOpen = true;
    sfxPlay(SFX_TAP);
    return;
  }
  // tocar al bicho = caricia
  if (inPetZone(x, y)) {
    pet.caress();
    digiReact(DIGI_ANIM_TOUCH, HEART_MS);
    if (!pet.sleeping) sfxPlay(SFX_HEART);
  }
}

// v3.20 live-care tabs ------------------------------------------------------
void drawCareTabs() {
  if (pet.ceremony != CER_NONE) return;
  for (uint8_t i = 0; i < CARE_SLOT_COUNT; i++) {
    int x = CARE_TAB_X0 + i * (CARE_TAB_W + CARE_TAB_GAP);
    bool active = i == careSlots.active();
    bool has = active ? true : careSlots.has(i);
    int16_t dex = -1;
    if (active) dex = pet.speciesId;
    else if (has && careSlots.snapshot(i)) dex = careSlots.snapshot(i)->speciesId;
    uint16_t edge = UI_TRACK;
    if (isCreatureId(dex)) edge = typeColor(creatureType1(dex));
    else if (has) edge = UI_BAR_WARN;
    gfx->fillRoundRect(x, CARE_TAB_Y, CARE_TAB_W, CARE_TAB_H, 8,
                       active ? UI_WHITE : UI_BG_DAY);
    gfx->drawRoundRect(x, CARE_TAB_Y, CARE_TAB_W, CARE_TAB_H, 8, active ? edge : UI_TRACK);
    gfx->setTextColor(active ? edge : UI_INK);
    uiSetTextSize(1);
    char lab[6];
    if (!has) snprintf(lab, sizeof(lab), "%u+", (unsigned)(i + 1));
    else snprintf(lab, sizeof(lab), "%u", (unsigned)(i + 1));
    uiSetCursor(x + (CARE_TAB_W - uiTextWidth(lab, 1)) / 2, CARE_TAB_Y + 7);
    gfx->print(lab);
  }
}

bool careTabTap(int16_t x, int16_t y) {
  if (pet.ceremony != CER_NONE || y < CARE_TAB_Y - 4 || y > CARE_TAB_Y + CARE_TAB_H + 4) return false;
  for (uint8_t i = 0; i < CARE_SLOT_COUNT; i++) {
    int tx = CARE_TAB_X0 + i * (CARE_TAB_W + CARE_TAB_GAP);
    if (x < tx - 3 || x > tx + CARE_TAB_W + 3) continue;
    if (i == careSlots.active()) { sfxPlay(SFX_TAP); return true; }
    bool targetWasEgg = careSlots.has(i) && careSlots.snapshot(i) && careSlots.snapshot(i)->speciesId < 1;
    uint32_t e = rtcEpoch();
    if (!e) e = pet.lastSeenEpoch;
    if (!careSlots.switchTo(i, pet, e)) { sfxPlay(SFX_DENY); return true; }
      // A tab switch itself is not research. The one exception is an egg that
    // actually hatched while parked; that is a real new observation and earns
    // the same +2 as an egg hatching on-screen.
    if (targetWasEgg && pet.speciesId >= 1) extras.addResearch(pet.speciesId, 2);
    extras.syncObservedPet(pet);
    feedMenuUntil = 0;
    choiceKind = 0;
    confirmUntil = 0;
    monFor = -2;                 // force sprite resync even if same species
    sdDirty = true;
    beh.mode = 0; beh.until = 0; beh.x = beh.targetX = CX;
    sfxPlay(SFX_TAP);
    return true;
  }
  return false;
}

// ---------- render ----------

bool gNight = false;  // real night by clock only; rest no longer darkens the interface
uint16_t inkColor() { return gNight ? UI_INK_NIGHT : UI_INK; }

// ---------- escena de fondo: bioma del tipo + hora real del RTC ----------

#define C565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define HORIZON 232  // linea donde el cielo se encuentra con el suelo

uint16_t lerp565(uint16_t a, uint16_t b, int i, int n) {
  if (n <= 0) return a;
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  return (uint16_t)((((ar + (br - ar) * i / n) << 11)) |
                    (((ag + (bg - ag) * i / n) << 5)) | (ab + (bb - ab) * i / n));
}

// hora del dia 0-23 (de la hora real cacheada cada 30s; 13 si no hay reloj)
int sceneHour() {
  uint32_t e = pet.lastSeenEpoch;
  return e ? (int)((e / 3600) % 24) : 13;
}

// Primary-type habitats. These are drawn procedurally in firmware so the
// GitHub/Netlify install flow stays unchanged: no extra image files on the
// repository, no SD assets to manage, but each type still gets a distinct scene.
static uint8_t currentSceneType() {
  if (pet.isEgg() || !isCreatureId(pet.speciesId)) return T_NORMAL;
  uint8_t t = creatureType1(pet.speciesId);
  return t < TYPE_COUNT ? t : T_NORMAL;
}

void drawClouds(uint32_t now, uint16_t col) {
  // Keep a clean center corridor for the Pokemon name.
  // Clouds stay smaller and hug the corners so they do not wash out the title.
  int drift = (int)((now / 3200UL) % 14UL);

  int cx1 = 54 + drift;
  int cy1 = 36;
  gfx->fillCircle(cx1, cy1, 12, col);
  gfx->fillCircle(cx1 + 14, cy1 + 2, 10, col);
  gfx->fillCircle(cx1 - 12, cy1 + 3, 9, col);
  gfx->fillRoundRect(cx1 - 12, cy1 + 3, 28, 8, 4, col);

  int cx2 = 404 - drift;
  int cy2 = 86;
  gfx->fillCircle(cx2, cy2, 13, col);
  gfx->fillCircle(cx2 + 15, cy2 + 2, 10, col);
  gfx->fillCircle(cx2 - 13, cy2 + 3, 10, col);
  gfx->fillRoundRect(cx2 - 13, cy2 + 4, 30, 8, 4, col);
}

static void typeSkyColors(uint8_t type, bool night, int h, uint16_t &top, uint16_t &bot) {
  if (night) {
    switch (type) {
      case T_FIRE:     top = C565(0x24, 0x0c, 0x0c); bot = C565(0x4a, 0x16, 0x12); break;
      case T_WATER:    top = C565(0x0b, 0x22, 0x3d); bot = C565(0x1a, 0x4c, 0x72); break;
      case T_ELECTRIC: top = C565(0x1e, 0x1a, 0x30); bot = C565(0x3d, 0x34, 0x5d); break;
      case T_GRASS:    top = C565(0x0f, 0x22, 0x18); bot = C565(0x1c, 0x46, 0x2d); break;
      case T_ICE:      top = C565(0x12, 0x24, 0x34); bot = C565(0x2c, 0x4d, 0x66); break;
      case T_FIGHTING: top = C565(0x1d, 0x14, 0x12); bot = C565(0x44, 0x28, 0x1c); break;
      case T_POISON:   top = C565(0x20, 0x10, 0x28); bot = C565(0x4c, 0x22, 0x60); break;
      case T_GROUND:   top = C565(0x2a, 0x1c, 0x14); bot = C565(0x56, 0x38, 0x26); break;
      case T_FLYING:   top = C565(0x15, 0x21, 0x3a); bot = C565(0x34, 0x4f, 0x73); break;
      case T_PSYCHIC:  top = C565(0x24, 0x0f, 0x32); bot = C565(0x60, 0x2a, 0x66); break;
      case T_BUG:      top = C565(0x15, 0x20, 0x0f); bot = C565(0x34, 0x50, 0x1c); break;
      case T_ROCK:     top = C565(0x21, 0x1d, 0x18); bot = C565(0x4a, 0x3d, 0x2f); break;
      case T_GHOST:    top = C565(0x14, 0x14, 0x24); bot = C565(0x30, 0x30, 0x4d); break;
      case T_DRAGON:   top = C565(0x18, 0x14, 0x30); bot = C565(0x3a, 0x2d, 0x63); break;
      case T_DARK:     top = C565(0x10, 0x10, 0x16); bot = C565(0x26, 0x26, 0x30); break;
      case T_STEEL:    top = C565(0x16, 0x1f, 0x28); bot = C565(0x38, 0x4a, 0x5b); break;
      case T_FAIRY:    top = C565(0x2a, 0x18, 0x30); bot = C565(0x5f, 0x3c, 0x60); break;
      default:         top = C565(0x0c, 0x12, 0x24); bot = C565(0x1e, 0x26, 0x46); break;
    }
    return;
  }

  bool dawn = h < 8;
  bool dusk = h >= 18;
  switch (type) {
    case T_FIRE:
      top = dawn ? C565(0xea, 0x82, 0x58) : dusk ? C565(0xde, 0x58, 0x34) : C565(0xf8, 0xb4, 0x72);
      bot = dawn ? C565(0xff, 0xcd, 0x92) : dusk ? C565(0xff, 0xae, 0x68) : C565(0xff, 0xe0, 0x9d);
      break;
    case T_WATER:
      top = dawn ? C565(0x76, 0xb0, 0xd8) : dusk ? C565(0x4f, 0x84, 0xb8) : C565(0x7d, 0xc9, 0xef);
      bot = dawn ? C565(0xb8, 0xda, 0xec) : dusk ? C565(0x94, 0xb4, 0xdc) : C565(0xe0, 0xf2, 0xfa);
      break;
    case T_ELECTRIC:
      top = dawn ? C565(0xf0, 0xba, 0x49) : dusk ? C565(0xe8, 0x9b, 0x2e) : C565(0xff, 0xd9, 0x4d);
      bot = dawn ? C565(0xff, 0xe1, 0x7f) : dusk ? C565(0xff, 0xc1, 0x66) : C565(0xff, 0xf1, 0xad);
      break;
    case T_GRASS:
      top = dawn ? C565(0x76, 0xc0, 0x74) : dusk ? C565(0x4d, 0x9f, 0x55) : C565(0x8a, 0xdb, 0x8a);
      bot = dawn ? C565(0xba, 0xe8, 0xad) : dusk ? C565(0x90, 0xd4, 0x82) : C565(0xe1, 0xf7, 0xd2);
      break;
    case T_ICE:
      top = dawn ? C565(0x99, 0xc8, 0xea) : dusk ? C565(0x75, 0xa3, 0xc8) : C565(0xb8, 0xe0, 0xf7);
      bot = dawn ? C565(0xd8, 0xec, 0xfb) : dusk ? C565(0xbd, 0xd9, 0xed) : C565(0xf3, 0xfb, 0xff);
      break;
    case T_FIGHTING:
      top = dawn ? C565(0xcc, 0x87, 0x60) : dusk ? C565(0xb5, 0x5d, 0x44) : C565(0xd8, 0xa0, 0x77);
      bot = dawn ? C565(0xef, 0xc1, 0x97) : dusk ? C565(0xdf, 0x95, 0x73) : C565(0xf4, 0xdd, 0xb1);
      break;
    case T_POISON:
      top = dawn ? C565(0xb6, 0x73, 0xc4) : dusk ? C565(0x8e, 0x52, 0xac) : C565(0xce, 0x92, 0xdc);
      bot = dawn ? C565(0xe0, 0xaf, 0xe8) : dusk ? C565(0xc0, 0x8a, 0xd3) : C565(0xf0, 0xd4, 0xf5);
      break;
    case T_GROUND:
      top = dawn ? C565(0xd6, 0xa3, 0x62) : dusk ? C565(0xc4, 0x7d, 0x4c) : C565(0xe3, 0xbd, 0x7d);
      bot = dawn ? C565(0xf4, 0xd1, 0x8f) : dusk ? C565(0xe8, 0xb0, 0x70) : C565(0xfa, 0xe6, 0xb5);
      break;
    case T_FLYING:
      top = dawn ? C565(0x94, 0xc0, 0xf0) : dusk ? C565(0x76, 0x9e, 0xd9) : C565(0xaf, 0xd4, 0xff);
      bot = dawn ? C565(0xd7, 0xea, 0xff) : dusk ? C565(0xb1, 0xce, 0xf2) : C565(0xf3, 0xfb, 0xff);
      break;
    case T_PSYCHIC:
      top = dawn ? C565(0xe2, 0x86, 0xb8) : dusk ? C565(0xcb, 0x5f, 0xa1) : C565(0xf0, 0xa2, 0xcb);
      bot = dawn ? C565(0xf7, 0xc1, 0xde) : dusk ? C565(0xe8, 0x96, 0xc0) : C565(0xff, 0xdf, 0xf0);
      break;
    case T_BUG:
      top = dawn ? C565(0xa4, 0xc2, 0x5a) : dusk ? C565(0x86, 0xa0, 0x38) : C565(0xb7, 0xdb, 0x67);
      bot = dawn ? C565(0xd6, 0xed, 0x9b) : dusk ? C565(0xbc, 0xd7, 0x6a) : C565(0xf0, 0xfa, 0xc9);
      break;
    case T_ROCK:
      top = dawn ? C565(0xb1, 0x99, 0x76) : dusk ? C565(0x98, 0x79, 0x56) : C565(0xc9, 0xb1, 0x8a);
      bot = dawn ? C565(0xdd, 0xcf, 0xa9) : dusk ? C565(0xc8, 0xab, 0x86) : C565(0xf2, 0xe4, 0xc0);
      break;
    case T_GHOST:
      top = dawn ? C565(0x8d, 0x8d, 0xb4) : dusk ? C565(0x72, 0x72, 0x98) : C565(0xa3, 0xa3, 0xc6);
      bot = dawn ? C565(0xcd, 0xcd, 0xe2) : dusk ? C565(0x9c, 0x9c, 0xbd) : C565(0xeb, 0xeb, 0xf7);
      break;
    case T_DRAGON:
      top = dawn ? C565(0x7b, 0x77, 0xde) : dusk ? C565(0x56, 0x51, 0xc4) : C565(0x94, 0x8b, 0xf3);
      bot = dawn ? C565(0xb9, 0xb6, 0xfa) : dusk ? C565(0x86, 0x80, 0xd8) : C565(0xdd, 0xdc, 0xff);
      break;
    case T_DARK:
      top = dawn ? C565(0x7b, 0x76, 0x8d) : dusk ? C565(0x53, 0x4f, 0x63) : C565(0x92, 0x8d, 0xa1);
      bot = dawn ? C565(0xb5, 0xb0, 0xc4) : dusk ? C565(0x7b, 0x77, 0x8d) : C565(0xd8, 0xd4, 0xe2);
      break;
    case T_STEEL:
      top = dawn ? C565(0x96, 0xa8, 0xb7) : dusk ? C565(0x77, 0x8e, 0xa0) : C565(0xb0, 0xc4, 0xd3);
      bot = dawn ? C565(0xd0, 0xdb, 0xe4) : dusk ? C565(0xa5, 0xb9, 0xc9) : C565(0xef, 0xf4, 0xf7);
      break;
    case T_FAIRY:
      top = dawn ? C565(0xf3, 0xa0, 0xc9) : dusk ? C565(0xe7, 0x7d, 0xaf) : C565(0xff, 0xbf, 0xdb);
      bot = dawn ? C565(0xff, 0xd4, 0xe9) : dusk ? C565(0xff, 0xb2, 0xcf) : C565(0xff, 0xed, 0xf6);
      break;
    default:
      if (dawn) { top = C565(0xd1, 0x6a, 0x86); bot = C565(0xf3, 0xb8, 0x7c); }
      else if (dusk) { top = C565(0xc7, 0x5a, 0x4a); bot = C565(0xf0, 0xae, 0x64); }
      else { top = C565(0x8f, 0xc8, 0xea); bot = C565(0xdc, 0xee, 0xe6); }
      break;
  }
}

static uint8_t rgb565Luma(uint16_t c) {
  uint16_t r = ((c >> 11) & 0x1F) * 255 / 31;
  uint16_t g = ((c >> 5) & 0x3F) * 255 / 63;
  uint16_t b = (c & 0x1F) * 255 / 31;
  return (uint8_t)((54u * r + 183u * g + 19u * b) >> 8);
}

static uint16_t scenePokemonNameColor(uint8_t type, bool night) {
  if (type >= TYPE_COUNT) type = T_NORMAL;
  uint16_t top, bot;
  typeSkyColors(type, night, sceneHour(), top, bot);
  // Prefer black by default. Only truly dark skies switch to a warm ivory.
  uint16_t bg = lerp565(top, bot, 58, HORIZON);
  return rgb565Luma(bg) < 84 ? C565(0xff, 0xf6, 0xdf) : RGB565_BLACK;
}

static uint16_t typeGroundColor(uint8_t type) {
  switch (type) {
    case T_FIRE:     return C565(0x6e, 0x39, 0x2f);
    case T_WATER:    return C565(0x61, 0x8f, 0xaa);
    case T_ELECTRIC: return C565(0xd8, 0xba, 0x4d);
    case T_GRASS:    return C565(0x6b, 0xbb, 0x68);
    case T_ICE:      return C565(0xe7, 0xf3, 0xfb);
    case T_FIGHTING: return C565(0xb7, 0x8c, 0x69);
    case T_POISON:   return C565(0x8f, 0x5b, 0x95);
    case T_GROUND:   return C565(0xcb, 0xa0, 0x61);
    case T_FLYING:   return C565(0xc5, 0xd9, 0xeb);
    case T_PSYCHIC:  return C565(0xd9, 0x8b, 0xb1);
    case T_BUG:      return C565(0x98, 0xb8, 0x3e);
    case T_ROCK:     return C565(0xa8, 0x8d, 0x69);
    case T_GHOST:    return C565(0x78, 0x72, 0x95);
    case T_DRAGON:   return C565(0x79, 0x6f, 0xc5);
    case T_DARK:     return C565(0x5a, 0x56, 0x63);
    case T_STEEL:    return C565(0x9f, 0xb0, 0xbc);
    case T_FAIRY:    return C565(0xf2, 0xbe, 0xd5);
    default:         return C565(0x7e, 0xc0, 0x7f);
  }
}

static void drawBackdropLayer(int x, int y, int w, int h, uint16_t col) {
  gfx->fillRoundRect(x, y, w, h, h / 2, col);
}

static void drawTinyFlower(int x, int y, uint16_t petal, uint16_t core, uint16_t stem) {
  gfx->drawLine(x, y + 2, x, y + 10, stem);
  gfx->fillCircle(x, y, 3, petal);
  gfx->fillCircle(x, y, 1, core);
}

static void drawBushCluster(int x, int y, uint16_t leafA, uint16_t leafB) {
  gfx->fillCircle(x, y, 13, leafA);
  gfx->fillCircle(x - 12, y + 4, 10, leafB);
  gfx->fillCircle(x + 13, y + 4, 10, leafB);
  gfx->fillCircle(x - 2, y - 7, 9, leafB);
}

static void drawRockSprite(int x, int y, uint16_t base, uint16_t shade) {
  gfx->fillCircle(x - 10, y + 4, 8, base);
  gfx->fillCircle(x + 2, y + 2, 10, base);
  gfx->fillCircle(x + 12, y + 6, 7, shade);
  gfx->fillRect(x - 14, y + 6, 30, 7, base);
}

static void drawReedCluster(int x, int y, uint16_t col) {
  gfx->drawLine(x, y + 10, x + 1, y - 6, col);
  gfx->drawLine(x + 5, y + 10, x + 5, y - 10, col);
  gfx->drawLine(x + 10, y + 10, x + 9, y - 4, col);
  gfx->drawLine(x + 3, y - 6, x + 8, y - 10, col);
}

static void drawCrystalSprite(int x, int y, uint16_t face, uint16_t edge) {
  gfx->fillTriangle(x, y - 18, x - 8, y + 2, x + 8, y + 2, face);
  gfx->drawLine(x, y - 18, x - 8, y + 2, edge);
  gfx->drawLine(x - 8, y + 2, x + 8, y + 2, edge);
  gfx->drawLine(x + 8, y + 2, x, y - 18, edge);
  gfx->drawLine(x, y - 18, x, y + 2, edge);
}

static void drawBareTree(int x, int y, uint16_t trunk) {
  gfx->fillRect(x, y - 30, 5, 34, trunk);
  gfx->drawLine(x + 2, y - 22, x - 10, y - 12, trunk);
  gfx->drawLine(x + 2, y - 18, x + 12, y - 10, trunk);
  gfx->drawLine(x + 2, y - 12, x - 8, y - 2, trunk);
  gfx->drawLine(x + 2, y - 8, x + 11, y + 1, trunk);
}

static void drawScenicBackdrop(uint8_t type, uint32_t now, bool night, int horizon,
                               uint16_t top, uint16_t bot, uint16_t soil) {
  uint16_t far = lerp565(bot, night ? C565(0x0c, 0x12, 0x24) : UI_WHITE, night ? 2 : 5, 16);
  uint16_t mid = lerp565(soil, bot, night ? 5 : 3, 8);
  if (type == T_DARK || type == T_GHOST) {
    far = lerp565(far, C565(0x18, 0x18, 0x24), 1, 2);
    mid = lerp565(mid, C565(0x22, 0x22, 0x32), 1, 2);
  } else if (type == T_STEEL) {
    far = lerp565(far, C565(0xd2, 0xdf, 0xe8), 1, 3);
    mid = lerp565(mid, C565(0x7f, 0x92, 0xa0), 1, 3);
  }

  drawBackdropLayer(-70, horizon - 108, 214, 82, far);
  drawBackdropLayer(70, horizon - 124, 214, 92, far);
  drawBackdropLayer(246, horizon - 104, 236, 82, far);
  drawBackdropLayer(-20, horizon - 64, 210, 64, mid);
  drawBackdropLayer(150, horizon - 54, 222, 68, mid);
  drawBackdropLayer(314, horizon - 60, 176, 64, mid);

  if (!night && type != T_GHOST && type != T_DARK && type != T_STEEL) {
    for (int i = 0; i < 3; ++i) {
      int cx = 48 + ((int)(now / 3500UL) + i * 132) % 356;
      int cy = 44 + (i & 1) * 14;
      gfx->fillCircle(cx, cy, 12, UI_WHITE);
      gfx->fillCircle(cx + 13, cy + 4, 10, UI_WHITE);
      gfx->fillCircle(cx - 11, cy + 5, 9, UI_WHITE);
      gfx->fillRoundRect(cx - 15, cy + 5, 32, 9, 5, UI_WHITE);
    }
  }

  switch (type) {
    case T_GRASS:
    case T_BUG:
    case T_FAIRY:
    case T_NORMAL: {
      uint16_t trunk = lerp565(soil, C565(0x58, 0x34, 0x1d), 1, 2);
      uint16_t leafA = night ? lerp565(soil, C565(0x4a, 0x7d, 0x43), 1, 3) : C565(0x79, 0xbf, 0x62);
      uint16_t leafB = night ? lerp565(soil, C565(0x63, 0x95, 0x58), 1, 3) : C565(0x97, 0xd8, 0x7b);
      for (int tx : { 48, 408 }) {
        gfx->fillRect(tx, horizon - 48, 10, 58, trunk);
        gfx->fillCircle(tx + 5, horizon - 60, 22, leafA);
        gfx->fillCircle(tx - 10, horizon - 52, 16, leafB);
        gfx->fillCircle(tx + 18, horizon - 50, 17, leafB);
      }
      drawBushCluster(120, horizon - 8, leafA, leafB);
      drawBushCluster(340, horizon - 10, leafB, leafA);
      break;
    }
    case T_WATER: {
      uint16_t shore = night ? C565(0x9d, 0xc9, 0xe8) : C565(0xd9, 0xf3, 0xff);
      uint16_t reed = night ? C565(0x7d, 0xa9, 0x7a) : C565(0x6f, 0xb6, 0x5c);
      gfx->fillRoundRect(16, horizon - 20, 164, 12, 6, shore);
      gfx->fillRoundRect(306, horizon - 14, 124, 10, 5, shore);
      gfx->fillRoundRect(198, horizon - 8, 48, 10, 5, shore);
      drawReedCluster(52, horizon - 4, reed);
      drawReedCluster(380, horizon - 2, reed);
      gfx->fillCircle(224, horizon - 2, 9, C565(0x7c, 0xc7, 0x7a));
      gfx->fillCircle(233, horizon + 2, 8, C565(0x88, 0xd6, 0x88));
      break;
    }
    case T_FIRE: {
      uint16_t peak = lerp565(soil, C565(0x44, 0x36, 0x32), night ? 1 : 3, 4);
      gfx->fillTriangle(84, horizon - 34, 24, horizon + 10, 144, horizon + 10, peak);
      gfx->fillTriangle(364, horizon - 28, 290, horizon + 16, 440, horizon + 16, peak);
      gfx->fillRect(206, horizon - 2, 48, 8, C565(0xcc, 0x4f, 0x22));
      gfx->fillRect(214, horizon + 1, 30, 4, C565(0xff, 0xb4, 0x4b));
      break;
    }
    case T_ROCK:
    case T_DRAGON: {
      uint16_t peak = lerp565(soil, C565(0x44, 0x36, 0x32), night ? 1 : 3, 4);
      gfx->fillTriangle(90, horizon - 38, 26, horizon + 12, 154, horizon + 12, peak);
      gfx->fillTriangle(362, horizon - 30, 286, horizon + 18, 444, horizon + 18, peak);
      drawRockSprite(206, horizon + 6, peak, lerp565(peak, RGB565_BLACK, 1, 4));
      break;
    }
    case T_ICE: {
      uint16_t face = night ? C565(0xcb, 0xe6, 0xf8) : C565(0xe8, 0xf7, 0xff);
      uint16_t edge = night ? C565(0x79, 0xa9, 0xc8) : C565(0x8d, 0xc0, 0xe4);
      drawCrystalSprite(88, horizon + 8, face, edge);
      drawCrystalSprite(144, horizon + 12, face, edge);
      drawCrystalSprite(338, horizon + 6, face, edge);
      drawCrystalSprite(392, horizon + 10, face, edge);
      break;
    }
    case T_GROUND: {
      uint16_t cactus = night ? C565(0x6a, 0x8e, 0x58) : C565(0x74, 0xaf, 0x62);
      gfx->fillRoundRect(-32, horizon - 4, 230, 48, 24, lerp565(soil, UI_WHITE, 3, 8));
      gfx->fillRoundRect(168, horizon + 2, 278, 50, 26, lerp565(soil, UI_WHITE, 2, 8));
      gfx->fillRect(84, horizon - 16, 8, 30, cactus);
      gfx->fillRect(77, horizon - 4, 7, 15, cactus);
      gfx->fillRect(92, horizon - 10, 7, 15, cactus);
      gfx->fillRect(352, horizon - 18, 8, 32, cactus);
      gfx->fillRect(345, horizon - 6, 7, 15, cactus);
      gfx->fillRect(360, horizon - 12, 7, 15, cactus);
      break;
    }
    case T_FLYING: {
      gfx->fillRoundRect(54, horizon + 6, 98, 18, 10, UI_WHITE);
      gfx->fillRoundRect(306, horizon + 12, 124, 18, 10, UI_WHITE);
      gfx->fillTriangle(118, horizon + 6, 138, horizon + 42, 90, horizon + 42, mid);
      gfx->fillTriangle(352, horizon + 12, 376, horizon + 50, 324, horizon + 50, mid);
      break;
    }
    case T_PSYCHIC: {
      uint16_t aura = night ? C565(0xd9, 0xc9, 0xff) : C565(0xff, 0xee, 0xff);
      gfx->drawCircle(118, horizon - 26, 22, aura);
      gfx->drawCircle(118, horizon - 26, 13, aura);
      gfx->drawCircle(348, horizon - 18, 24, aura);
      gfx->drawCircle(348, horizon - 18, 15, aura);
      break;
    }
    case T_POISON: {
      uint16_t ooze = C565(0x8a, 0xe9, 0x73);
      gfx->fillRoundRect(46, horizon + 8, 126, 22, 11, ooze);
      gfx->fillRoundRect(236, horizon + 14, 154, 18, 9, C565(0x88, 0xef, 0x76));
      gfx->fillRect(76, horizon - 24, 5, 26, mid);
      gfx->fillCircle(78, horizon - 28, 9, C565(0xc1, 0xa2, 0xde));
      gfx->fillRect(342, horizon - 26, 5, 28, mid);
      gfx->fillCircle(344, horizon - 31, 10, C565(0xc1, 0xa2, 0xde));
      break;
    }
    case T_FIGHTING: {
      gfx->fillRect(78, horizon - 18, 310, 30, C565(0xd8, 0xc5, 0x96));
      gfx->drawRect(78, horizon - 18, 310, 30, mid);
      gfx->fillRect(108, horizon - 56, 8, 48, mid);
      gfx->fillRect(346, horizon - 56, 8, 48, mid);
      gfx->fillRect(96, horizon - 50, 30, 6, mid);
      gfx->fillRect(334, horizon - 50, 30, 6, mid);
      break;
    }
    case T_ELECTRIC: {
      gfx->fillRect(88, horizon - 54, 6, 64, mid);
      gfx->fillRect(356, horizon - 62, 6, 72, mid);
      gfx->fillRect(72, horizon - 48, 38, 4, mid);
      gfx->fillRect(340, horizon - 56, 38, 4, mid);
      gfx->fillCircle(208, horizon - 18, 6, C565(0xff, 0xe4, 0x60));
      gfx->fillCircle(258, horizon - 12, 5, C565(0xff, 0xd0, 0x44));
      break;
    }
    case T_GHOST:
    case T_DARK: {
      uint16_t trunk = night ? C565(0x74, 0x72, 0x86) : C565(0x8a, 0x86, 0x98);
      drawBareTree(70, horizon + 8, trunk);
      drawBareTree(376, horizon + 10, trunk);
      gfx->fillRoundRect(176, horizon + 14, 110, 10, 5, lerp565(mid, UI_WHITE, 1, 5));
      break;
    }
    case T_STEEL: {
      gfx->fillRect(0, horizon - 8, 466, 38, lerp565(UI_WHITE, soil, 1, 4));
      gfx->fillRect(0, horizon + 12, 466, 4, mid);
      for (int x = 18; x < 466; x += 52) gfx->fillCircle(x, horizon + 6, 3, mid);
      for (int px : { 108, 233, 358 }) {
        gfx->fillRect(px - 5, horizon - 54, 10, 60, mid);
        gfx->fillRect(px - 26, horizon - 44, 52, 6, mid);
      }
      break;
    }
    default:
      break;
  }

  if (!night && type != T_STEEL && type != T_DARK && type != T_GHOST && type != T_WATER) {
    uint16_t stem = lerp565(soil, C565(0x4c, 0x74, 0x38), 1, 2);
    for (int fx : { 34, 60, 92, 372, 402, 430 }) {
      uint16_t petal = (fx % 3 == 0) ? C565(0xff, 0xb7, 0xd2)
                       : (fx % 3 == 1) ? C565(0xff, 0xec, 0x97)
                                       : C565(0xb9, 0xe7, 0xff);
      drawTinyFlower(fx, horizon + 24 + (fx % 2) * 3, petal, UI_WHITE, stem);
    }
  }
}

static void drawForegroundAccents(uint8_t type, bool night, int horizon, uint16_t soil) {
  uint16_t grass = night ? lerp565(soil, C565(0x2c, 0x42, 0x28), 1, 2)
                         : lerp565(soil, C565(0x74, 0xa6, 0x56), 1, 2);
  switch (type) {
    case T_WATER: {
      uint16_t ripple = night ? C565(0xb6, 0xdd, 0xf4) : C565(0xe6, 0xf8, 0xff);
      for (int x = 22; x < 444; x += 56) {
        gfx->fillRoundRect(x, horizon + 18, 28, 4, 2, ripple);
      }
      break;
    }
    case T_ICE: {
      uint16_t snow = night ? C565(0xdd, 0xf0, 0xff) : UI_WHITE;
      for (int x = 16; x < 450; x += 34) gfx->fillCircle(x, horizon + 28 + ((x / 34) & 1), 2, snow);
      break;
    }
    case T_ROCK:
    case T_GROUND:
    case T_DRAGON: {
      uint16_t peb = lerp565(soil, C565(0x48, 0x3c, 0x36), 1, 2);
      for (int x = 22; x < 446; x += 40) drawRockSprite(x, horizon + 26, peb, soil);
      break;
    }
    case T_GHOST:
    case T_DARK: {
      uint16_t mist = lerp565(soil, UI_WHITE, 1, 8);
      for (int x = 12; x < 446; x += 62) gfx->fillRoundRect(x, horizon + 22, 34, 6, 3, mist);
      break;
    }
    case T_STEEL: {
      uint16_t bolt = lerp565(soil, C565(0x3b, 0x4c, 0x5d), 1, 2);
      for (int x = 18; x < 446; x += 52) {
        gfx->fillRect(x, horizon + 22, 26, 6, bolt);
        gfx->fillCircle(x + 5, horizon + 25, 2, UI_WHITE);
        gfx->fillCircle(x + 21, horizon + 25, 2, UI_WHITE);
      }
      break;
    }
    default: {
      for (int gx = 12; gx < 466; gx += 24) {
        gfx->drawLine(gx, horizon + 30, gx + 2, horizon + 20, grass);
        gfx->drawLine(gx + 4, horizon + 30, gx + 4, horizon + 18, grass);
        gfx->drawLine(gx + 8, horizon + 30, gx + 6, horizon + 21, grass);
      }
      break;
    }
  }
}

static void drawTypeTerrainDetails(uint8_t type, uint32_t now, bool night, int horizon, uint16_t soil, uint16_t ink) {
  uint16_t dk = lerp565(soil, C565(0x10, 0x18, 0x20), night ? 11 : 7, 16);
  uint16_t hi = lerp565(soil, UI_WHITE, 6, 16);
  switch (type) {
    case T_FIRE:
      gfx->fillTriangle(82, horizon, 24, horizon + 34, 140, horizon + 34, dk);
      gfx->fillTriangle(384, horizon + 2, 330, horizon + 36, 438, horizon + 36, dk);
      gfx->fillRect(188, horizon + 8, 90, 12, C565(0xcf, 0x53, 0x22));
      gfx->fillRect(208, horizon + 12, 50, 4, C565(0xff, 0xab, 0x45));
      for (int e = 0; e < 4; e++) gfx->fillRect(120 + e * 66, horizon - 12 - (e & 1) * 7, 4, 6, C565(0xff, 0xae, 0x55));
      break;
    case T_WATER:
      gfx->fillRect(0, horizon - 26, 466, 28, night ? C565(0x1c, 0x34, 0x52) : C565(0x4f, 0x96, 0xc4));
      for (int i = 0; i < 4; i++) {
        int wy = horizon - 22 + i * 6;
        uint16_t fc = night ? C565(0x3a, 0x58, 0x78) : C565(0xbf, 0xe6, 0xf5);
        gfx->fillRect(40 + ((now / 60 + i * 44) % 120), wy, 34, 2, fc);
        gfx->fillRect(292 - ((now / 60 + i * 31) % 100), wy, 28, 2, fc);
      }
      gfx->fillCircle(86, horizon + 18, 12, dk);
      gfx->fillCircle(364, horizon + 14, 8, dk);
      break;
    case T_ELECTRIC:
      gfx->fillRect(86, horizon - 56, 6, 64, dk);
      gfx->fillRect(360, horizon - 62, 6, 70, dk);
      gfx->fillRect(70, horizon - 50, 38, 4, dk);
      gfx->fillRect(344, horizon - 56, 38, 4, dk);
      gfx->fillTriangle(215, horizon - 82, 190, horizon - 22, 225, horizon - 42, C565(0xff, 0xe2, 0x54));
      gfx->fillTriangle(232, horizon - 42, 248, horizon - 84, 210, horizon - 60, C565(0xff, 0xc8, 0x2e));
      break;
    case T_GRASS:
      for (int gx : { 74, 132, 222, 310, 388 }) {
        for (int b = -1; b <= 1; b++) gfx->fillRect(gx + b * 5, horizon + 6, 2, 10 + (b == 0 ? 6 : 0), dk);
      }
      for (int fx : { 104, 188, 286, 352 }) {
        gfx->fillRect(fx, horizon + 12, 3, 10, dk);
        gfx->fillRect(fx - 4, horizon + 16, 11, 3, C565(0xff, 0xf2, 0x88));
        gfx->fillRect(fx, horizon + 12, 3, 11, C565(0xff, 0x8b, 0xb3));
      }
      break;
    case T_ICE:
      for (int x : { 70, 120, 188, 280, 360, 412 }) {
        gfx->fillTriangle(x, horizon, x - 12, horizon + 28, x + 12, horizon + 28, hi);
        gfx->drawLine(x, horizon, x - 12, horizon + 28, dk);
        gfx->drawLine(x - 12, horizon + 28, x + 12, horizon + 28, dk);
        gfx->drawLine(x + 12, horizon + 28, x, horizon, dk);
      }
      for (int f = 0; f < 12; f++) {
        int fx = (f * 39 + now / 35) % 466;
        int fy = (f * 57 + now / 20) % horizon;
        gfx->fillRect(fx, fy, 3, 3, UI_WHITE);
      }
      break;
    case T_FIGHTING:
      gfx->fillRect(78, horizon - 18, 310, 30, C565(0xd8, 0xc5, 0x96));
      gfx->drawRect(78, horizon - 18, 310, 30, dk);
      for (int px : { 108, 358 }) {
        gfx->fillRect(px, horizon - 56, 8, 48, dk);
        gfx->fillRect(px - 12, horizon - 52, 32, 6, dk);
      }
      gfx->fillCircle(CX, horizon - 3, 18, C565(0xb8, 0x30, 0x30));
      gfx->fillCircle(CX, horizon - 3, 10, C565(0xdc, 0xca, 0x94));
      break;
    case T_POISON:
      gfx->fillRoundRect(54, horizon + 8, 118, 22, 10, C565(0x75, 0xdd, 0x62));
      gfx->fillRoundRect(238, horizon + 14, 150, 18, 9, C565(0x88, 0xef, 0x76));
      gfx->fillCircle(96, horizon - 12, 7, C565(0xcb, 0xf7, 0xba));
      gfx->fillCircle(128, horizon - 22, 5, C565(0xcb, 0xf7, 0xba));
      gfx->fillCircle(334, horizon - 10, 6, C565(0xcb, 0xf7, 0xba));
      gfx->fillRect(398, horizon - 44, 4, 44, dk);
      gfx->fillRect(62, horizon - 38, 4, 38, dk);
      gfx->fillRect(394, horizon - 44, 14, 4, dk);
      gfx->fillRect(58, horizon - 38, 14, 4, dk);
      break;
    case T_GROUND:
      gfx->fillRoundRect(-40, horizon - 2, 240, 44, 24, hi);
      gfx->fillRoundRect(154, horizon + 4, 280, 48, 28, lerp565(hi, soil, 3, 8));
      for (int cx : { 92, 348 }) {
        gfx->fillRect(cx, horizon - 18, 6, 26, dk);
        gfx->fillRect(cx - 12, horizon - 6, 30, 6, dk);
        gfx->fillRect(cx - 16, horizon - 14, 14, 5, dk);
        gfx->fillRect(cx + 8, horizon - 20, 14, 5, dk);
      }
      break;
    case T_FLYING:
      drawClouds(now, UI_WHITE);
      gfx->fillRoundRect(48, horizon + 8, 98, 18, 10, hi);
      gfx->fillRoundRect(300, horizon + 14, 124, 18, 10, hi);
      gfx->fillTriangle(118, horizon + 8, 136, horizon + 42, 92, horizon + 42, dk);
      gfx->fillTriangle(346, horizon + 14, 370, horizon + 50, 320, horizon + 50, dk);
      break;
    case T_PSYCHIC:
      gfx->drawCircle(116, horizon - 28, 18, hi);
      gfx->drawCircle(116, horizon - 28, 11, hi);
      gfx->drawCircle(350, horizon - 20, 24, hi);
      gfx->drawCircle(350, horizon - 20, 15, hi);
      for (int sx : { 84, 160, 234, 308, 392 }) {
        gfx->fillRect(sx, horizon + 10, 4, 4, UI_WHITE);
        gfx->fillRect(sx - 3, horizon + 13, 10, 2, UI_WHITE);
      }
      break;
    case T_BUG:
      for (int tx : { 84, 160, 302, 386 }) {
        gfx->fillRect(tx - 4, horizon - 36, 8, 40, dk);
        gfx->fillCircle(tx, horizon - 42, 18, dk);
        gfx->fillCircle(tx - 12, horizon - 34, 11, dk);
        gfx->fillCircle(tx + 12, horizon - 34, 11, dk);
      }
      for (int mx : { 120, 250, 348 }) {
        gfx->fillRect(mx, horizon + 10, 4, 10, C565(0xff, 0xe8, 0xdc));
        gfx->fillCircle(mx + 2, horizon + 10, 8, C565(0xdf, 0x6a, 0x4c));
        gfx->fillRect(mx - 5, horizon + 8, 14, 3, C565(0xff, 0xff, 0xff));
      }
      break;
    case T_ROCK:
      gfx->fillTriangle(120, horizon - 44, 42, horizon + 10, 196, horizon + 10, dk);
      gfx->fillTriangle(334, horizon - 30, 250, horizon + 16, 418, horizon + 16, dk);
      gfx->fillCircle(88, horizon + 20, 12, dk);
      gfx->fillCircle(252, horizon + 24, 16, dk);
      gfx->fillCircle(390, horizon + 18, 10, dk);
      break;
    case T_GHOST:
      if (!night) for (auto &st : STARS) gfx->fillRect(st[0], st[1], 3, 3, UI_WHITE);
      gfx->fillRoundRect(78, horizon + 4, 20, 24, 5, dk);
      gfx->fillRect(86, horizon - 8, 4, 20, dk);
      gfx->fillRect(78, horizon, 20, 4, dk);
      gfx->fillRoundRect(334, horizon + 6, 24, 28, 5, dk);
      gfx->fillRect(344, horizon - 8, 4, 22, dk);
      gfx->fillRect(334, horizon + 2, 24, 4, dk);
      gfx->fillRoundRect(170, horizon + 16, 126, 10, 5, hi);
      break;
    case T_DRAGON:
      gfx->fillTriangle(94, horizon, 38, horizon + 34, 150, horizon + 34, dk);
      gfx->fillTriangle(370, horizon, 300, horizon + 42, 440, horizon + 42, dk);
      for (int px : { 178, 260 }) {
        gfx->fillRect(px, horizon - 46, 12, 56, dk);
        gfx->fillTriangle(px - 8, horizon - 46, px + 6, horizon - 60, px + 20, horizon - 46, dk);
      }
      break;
    case T_DARK:
      gfx->fillCircle(360, 74, 18, C565(0xf0, 0xec, 0xe2));
      gfx->fillCircle(368, 70, 16, lerp565(C565(0x10, 0x10, 0x16), C565(0x26, 0x26, 0x30), 74, 120));
      for (int tx : { 64, 126, 186, 330, 396 }) {
        gfx->fillTriangle(tx, horizon - 34, tx - 16, horizon + 8, tx + 16, horizon + 8, dk);
      }
      gfx->fillRect(0, horizon + 20, 466, 22, lerp565(dk, RGB565_BLACK, 1, 2));
      break;
    case T_STEEL:
      gfx->fillRect(0, horizon - 8, 466, 38, hi);
      gfx->fillRect(0, horizon + 12, 466, 4, dk);
      for (int x = 18; x < 466; x += 52) gfx->fillCircle(x, horizon + 6, 3, dk);
      for (int px : { 108, 233, 358 }) {
        gfx->fillRect(px - 5, horizon - 54, 10, 60, dk);
        gfx->fillRect(px - 26, horizon - 44, 52, 6, dk);
      }
      break;
    case T_FAIRY:
      for (int mx : { 92, 164, 286, 362 }) {
        gfx->fillRect(mx, horizon + 12, 5, 13, dk);
        gfx->fillCircle(mx + 2, horizon + 10, 10, C565(0xff, 0xb1, 0xcf));
        gfx->fillCircle(mx - 4, horizon + 14, 8, C565(0xff, 0xd7, 0xe8));
        gfx->fillCircle(mx + 8, horizon + 14, 8, C565(0xff, 0xd7, 0xe8));
      }
      for (int sx : { 122, 214, 332, 408 }) {
        gfx->fillRect(sx, horizon - 12, 3, 12, UI_WHITE);
        gfx->fillRect(sx - 4, horizon - 8, 11, 3, UI_WHITE);
      }
      break;
    default:
      for (int gx : { 80, 175, 300, 395 })
        for (int b = -1; b <= 1; b++)
          gfx->fillRect(gx + b * 5, horizon + 6, 2, 8 + (b == 0 ? 4 : 0), dk);
      break;
  }
}

static void drawWeatherOverlay(uint8_t weather, uint32_t now, int horizon) {
  if (weather >= WEATHER_COUNT) return;
  if (weather == WEATHER_RAIN || weather == WEATHER_STORM) {
    uint16_t rc = weather == WEATHER_STORM ? C565(0x9f,0xb8,0xd8) : C565(0xa9,0xd4,0xed);
    for (int i=0;i<22;i++) {
      int x=(i*43+(int)(now/18))%490-12;
      int y=(i*71+(int)(now/11))%horizon;
      gfx->drawLine(x,y,x-7,y+14,rc);
    }
    if (weather == WEATHER_STORM && ((now/700)%9)==0) {
      gfx->drawLine(116,34,94,84,C565(0xff,0xef,0x88));
      gfx->drawLine(94,84,112,80,C565(0xff,0xef,0x88));
      gfx->drawLine(112,80,86,130,C565(0xff,0xef,0x88));
    }
  } else if (weather == WEATHER_SNOW) {
    for(int i=0;i<18;i++){int x=(i*37+(int)(now/32))%466;int y=(i*53+(int)(now/20))%horizon;gfx->fillCircle(x,y,2,UI_WHITE);}
  } else if (weather == WEATHER_SAND) {
    uint16_t sc=C565(0xe2,0xc2,0x7f);
    for(int i=0;i<24;i++){int x=(i*29+(int)(now/13))%480-8;int y=70+(i*47+(int)(now/29))%(horizon>90?horizon-80:10);gfx->fillRect(x,y,6,2,sc);}
  } else if (weather == WEATHER_FOG) {
    // v3.42: fog weather is retired. Keep this legacy branch as a no-op so
    // older saved values cannot cover the UI while they are being migrated.
    return;
  }
}

static void drawTypeScene(uint8_t type, uint32_t now, bool night, int horizon, int sceneBottom = 466) {
  int h = sceneHour();
  if (type >= TYPE_COUNT) type = T_NORMAL;
  uint16_t top, bot;
  typeSkyColors(type, night, h, top, bot);

  for (int y = 0; y < horizon; y += 8) gfx->fillRect(0, y, 466, 8, lerp565(top, bot, y, horizon));

  if (night) {
    uint16_t moon = (type == T_GHOST || type == T_FAIRY) ? C565(0xf3, 0xe7, 0xfa) : C565(0xe8, 0xee, 0xf5);
    gfx->fillCircle(360, 78, 24, moon);
    gfx->fillCircle(370, 72, 22, lerp565(top, bot, 78, horizon));
    for (auto &st : STARS) gfx->fillRect(st[0], st[1], 4, 4, UI_WHITE);
  } else if (h < 18) {
    uint16_t sun = (type == T_ELECTRIC) ? C565(0xff, 0xe0, 0x54)
                    : (type == T_FIRE) ? C565(0xff, 0xc3, 0x73)
                    : (type == T_FAIRY) ? C565(0xff, 0xdd, 0xef)
                    : C565(0xff, 0xe7, 0x9f);
    gfx->fillCircle(360, 84, 26, sun);
    if (type != T_FLYING) drawClouds(now, C565(0xff, 0xff, 0xff));
  } else {
    gfx->fillCircle(233, horizon - 6, 34, (type == T_FIRE) ? C565(0xff, 0xd0, 0x8a) : C565(0xff, 0xf1, 0xc8));
  }

  uint16_t soil = typeGroundColor(type);
  if (night) soil = lerp565(soil, C565(0x16, 0x1c, 0x30), 9, 16);
  drawScenicBackdrop(type, now, night, horizon, top, bot, soil);
  if (sceneBottom < horizon) sceneBottom = horizon;
  if (sceneBottom > LCD_HEIGHT) sceneBottom = LCD_HEIGHT;
  gfx->fillRect(0, horizon, 466, sceneBottom - horizon, soil);

  uint16_t hill = lerp565(soil, night ? C565(0x0c, 0x12, 0x24) : UI_WHITE, 3, 16);
  if (type == T_WATER || type == T_FLYING || type == T_PSYCHIC || type == T_FAIRY)
    hill = lerp565(soil, night ? C565(0x0c, 0x12, 0x24) : UI_WHITE, 1, 8);
  gfx->fillRoundRect(-60, horizon - 14, 586, 60, 30, hill);
  drawTypeTerrainDetails(type, now, night, horizon, soil, night ? UI_INK_NIGHT : UI_INK);
  drawForegroundAccents(type, night, horizon, soil);
  drawWeatherOverlay(extras.weatherId(pet), now, horizon);
}

void drawScene(uint8_t type, uint32_t now, bool night) {
  // Home controls own rows 312..465. Do not repaint those 154 rows of PSRAM on
  // every animation frame only to cover them with the same bars/buttons again.
  // Farewell/runaway ceremonies intentionally use the full scenic ground.
  drawTypeScene(type, now, night, HORIZON, pet.ceremony ? LCD_HEIGHT : 312);
}


static void drawWeatherIcon(uint8_t w, int x, int y, uint16_t col) {
  // Tiny procedural icons: no SD asset, no allocation, safe on every boot.
  switch (w) {
    case WEATHER_CLEAR:
      gfx->fillCircle(x + 7, y + 7, 4, col);
      gfx->drawLine(x + 7, y, x + 7, y + 2, col); gfx->drawLine(x + 7, y + 12, x + 7, y + 14, col);
      gfx->drawLine(x, y + 7, x + 2, y + 7, col); gfx->drawLine(x + 12, y + 7, x + 14, y + 7, col);
      break;
    case WEATHER_RAIN:
      gfx->fillCircle(x + 5, y + 6, 4, col); gfx->fillCircle(x + 10, y + 6, 5, col);
      gfx->fillRect(x + 3, y + 6, 11, 4, col);
      gfx->drawLine(x + 5, y + 11, x + 3, y + 14, col); gfx->drawLine(x + 11, y + 11, x + 9, y + 14, col);
      break;
    case WEATHER_SNOW:
      gfx->drawLine(x + 7, y, x + 7, y + 14, col); gfx->drawLine(x, y + 7, x + 14, y + 7, col);
      gfx->drawLine(x + 2, y + 2, x + 12, y + 12, col); gfx->drawLine(x + 12, y + 2, x + 2, y + 12, col);
      break;
    case WEATHER_SAND:
      gfx->drawLine(x, y + 4, x + 11, y + 4, col); gfx->drawLine(x + 3, y + 8, x + 14, y + 8, col);
      gfx->drawLine(x, y + 12, x + 9, y + 12, col); gfx->fillCircle(x + 13, y + 12, 1, col);
      break;
    case WEATHER_STORM:
      gfx->fillCircle(x + 5, y + 5, 4, col); gfx->fillCircle(x + 10, y + 5, 5, col); gfx->fillRect(x + 3, y + 5, 11, 4, col);
      gfx->fillTriangle(x + 8, y + 8, x + 4, y + 14, x + 8, y + 13, C565(0xff,0xe4,0x62));
      gfx->fillTriangle(x + 8, y + 11, x + 12, y + 10, x + 7, y + 16, C565(0xff,0xe4,0x62));
      break;
    case WEATHER_FOG:
      gfx->drawLine(x, y + 4, x + 14, y + 4, col); gfx->drawLine(x + 3, y + 8, x + 13, y + 8, col);
      gfx->drawLine(x, y + 12, x + 10, y + 12, col);
      break;
  }
}

static void drawWeatherBadge() {
  uint8_t w = extras.weatherId(pet);
  if (w == WEATHER_FOG || w >= WEATHER_COUNT) w = WEATHER_CLEAR;
  const char *nm = extras.weatherNameKo(w);
  uint16_t col = w==WEATHER_RAIN ? C565(0x5f,0x9d,0xca) : w==WEATHER_SNOW ? C565(0xa8,0xcf,0xea)
                 : w==WEATHER_SAND ? C565(0xc5,0x9e,0x4d) : w==WEATHER_STORM ? C565(0x78,0x6e,0xae)
                 : C565(0xe8,0xb9,0x3f);
  int ww = uiTextWidth(nm, 1) + 42;
  if (ww < 64) ww = 64;
  const int x = 22, y = 14;
  uint16_t bg = gNight ? lerp565(UI_BG_NIGHT, col, 3, 16) : lerp565(UI_WHITE, col, 2, 16);
  gfx->fillRoundRect(x, y, ww, 26, 9, bg);
  gfx->drawRoundRect(x, y, ww, 26, 9, col);
  drawWeatherIcon(w, x + 8, y + 6, col);
  gfx->setTextColor(gNight ? UI_INK_NIGHT : UI_INK);
  uiSetTextSize(1);
  uiSetCursor(x + 28, y + 8);
  gfx->print(nm);
}


// primera partida: elige inicial entre Bulbasaur / Charmander / Squirtle
void renderStarterSelect() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  const char *t = T(S_CHOOSE_STARTER);
  gfx->setTextColor(UI_INK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(t, 2), 68);
  gfx->print(t);
  for (int i = 0; i < starterCountShown(pet.region); i++) {
    int16_t d = starterOf(pet.region, i);
    const DexEntry &de = DEX_TBL[d];
    int ry = STARTER_ROW_Y + i * (STARTER_ROW_H + STARTER_ROW_GAP);
    gfx->fillRoundRect(70, ry, 326, STARTER_ROW_H, 14, lerp565(de.accent, UI_WHITE, 6, 8));
    gfx->drawRoundRect(70, ry, 326, STARTER_ROW_H, 14, de.accent);
    const uint8_t *th = thumbs.get(d);     // miniatura del inicial (si la SD esta lista)
    if (th) drawThumb(th, 76, ry - 5, 3, false);
    gfx->setTextColor(UI_INK);
    uiSetTextSize(3);
    uiSetCursor(178, ry + 24);
    gfx->print(localizedSpeciesName(d));
  }
  gfx->flush();
}

// ---------- crash breadcrumbs ----------
//
// "It has a mini crash" is not a bug report you can act on, because the board
// never says WHY it restarted. These three live in RTC memory, which survives
// a panic, a watchdog and a software reset -- everything except pulling the
// power -- so the next boot can say what the firmware was doing when it died.
//
// Written every frame. That is one word to RTC RAM per render, which costs
// nothing next to a full-screen redraw, and it means the crumb always names
// the screen that was actually on the panel rather than the last one opened.
#define CRUMB_MAGIC 0x7AABE10C
RTC_NOINIT_ATTR uint32_t gCrumbMagic;
RTC_NOINIT_ATTR uint32_t gCrumbScreen;
RTC_NOINIT_ATTR uint32_t gCrumbHeap;


// Which screen is on the panel RIGHT NOW, in the same order render() tests.
uint8_t uiCurrentScreen() {
  if (pet.awaitingStarter()) return starterRegionDone ? SCR_STARTER : SCR_REGION;
  // Team selection is a modal overlay. It must win over stale source-screen
  // flags (party/box, boss or gym), otherwise those screens hide the picker.
  if (pickOpen) return SCR_PICK;
  if (digiOpen || digiDexOpen) return SCR_MENU;
  if (bagOpen || missionOpen || adventureOpen || exploreOpen || bossOpen || towerOpen || treasureOpen || rivalOpen) return SCR_MENU;
  if (galleryOpen) return galleryPick ? SCR_DEXPICK : SCR_GALLERY;
  if (movePickOpen) return SCR_MOVEPICK;
  if (partyOpen) return boxOpen ? SCR_BOX : SCR_PARTY;
  if (kbOpen) return SCR_KEYBOARD;
  if (cardOpen) return SCR_CARD;
  if (playerOpen) return SCR_PLAYER;
  if (clockOpen) return SCR_CLOCK;
  if (btlWinUntil) return SCR_WIN;
  if (battleOpen) return SCR_BATTLE;
  if (lanOpen) return SCR_LAN;
  if (gymOpen) return gymPick ? SCR_GYMPICK : SCR_GYM;
  // Match render()/touch priority exactly. A level-up move offer can appear
  // while a training session is open; it must wait until training closes.
  if (gameOpen || sackOpen || spdOpen || hpOpen) return SCR_GAME;
  if (trainOpen) return SCR_TRAIN;
  if (pet.hasLearnOffer()) return SCR_LEARN;
  if (menuOpen) return SCR_MENU;
  return SCR_MAIN;
}

static void crumbDrop() {
  // Screen identity only changes on navigation. Updating RTC RAM and querying
  // heap every 85-100 ms added needless work to the render hot path. Keep the
  // crash breadcrumb just as useful by refreshing immediately on a screen
  // change and once per second while remaining on that screen.
  static uint8_t lastScreen = 0xFF;
  static uint32_t lastCrumbAt = 0;
  uint8_t scr = uiCurrentScreen();
  uint32_t now = millis();
  if (scr == lastScreen && now - lastCrumbAt < 1000UL) return;
  lastScreen = scr;
  lastCrumbAt = now;
  gCrumbMagic = CRUMB_MAGIC;
  gCrumbScreen = scr;
  gCrumbHeap = ESP.getFreeHeap();
}

static const char *resetReasonName(int r) {
  switch (r) {
    case ESP_RST_POWERON:  return "power on";
    case ESP_RST_EXT:      return "reset pin";
    case ESP_RST_SW:       return "software (our own restart)";
    case ESP_RST_PANIC:    return "PANIC -- a crash";
    case ESP_RST_INT_WDT:  return "INTERRUPT WATCHDOG";
    case ESP_RST_TASK_WDT: return "TASK WATCHDOG -- something blocked too long";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "BROWNOUT -- the supply sagged";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default: return "unknown";
  }
}

// Printed once at boot. On a clean start it is one line; after a crash it says
// which screen was up and how much heap was left, which is the whole point.
void bootReport() {
  int r = (int)esp_reset_reason();
  bool bad = (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT ||
              r == ESP_RST_TASK_WDT || r == ESP_RST_WDT || r == ESP_RST_BROWNOUT);
  Serial.printf("boot: reset=%s heap=%u psram=%u\n", resetReasonName(r),
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
  if (bad && gCrumbMagic == CRUMB_MAGIC) {
    Serial.printf("CRASH: it died on the '%s' screen with heap=%u\n",
                  gCrumbScreen < SCR_COUNT ? SCREEN_NAME[gCrumbScreen] : "?",
                  (unsigned)gCrumbHeap);
  } else if (bad) {
    Serial.println("CRASH: no breadcrumb (RTC memory was lost too)");
  }
  gCrumbMagic = 0;   // one report per crash, not on every later boot
}

static bool gHomePanelValid = false;
static bool gHomeNeedFullThisFrame = true;
static uint32_t gHomeBottomSig = 0;
void invalidateHomePresent() { gHomePanelValid = false; }

static uint32_t homeBottomSignature() {
  // FNV-1a over the state that affects y>=312. Header/Pokemon animation live in
  // the upper band and therefore do not need to force a full-screen transfer.
  uint32_t h = 2166136261UL;
  auto mix = [&](uint32_t v) { h ^= v; h *= 16777619UL; };
  mix(pet.fullness); mix(pet.joy); mix(pet.energy); mix(pet.hygiene);
  mix(pet.poops); mix(pet.sleeping ? 1 : 0); mix(gNight ? 1 : 0); mix(gLang);
  uint32_t disabled = 0;
  for (uint8_t i = 0; i < BTN_COUNT; ++i) if (uiButtonDisabled(i)) disabled |= (1UL << i);
  mix(disabled);
  return h;
}

// Anything in this set draws across the 312-row partial-present boundary.
// A frame containing one of these is NOT a clean cached home panel: if we mark
// it valid, closing the overlay can update only rows 0..311 and leave its lower
// buttons physically stuck on the AMOLED.  Keep this predicate shared by the
// decision and the cache commit so opening/closing paths cannot drift apart.
static bool homeCrossBandActive() {
  return pet.isEgg() || menuOpen || feedMenuUntil || confirmUntil || choiceKind;
}

static bool homeNeedsFullPresent(uint32_t now) {
  const uint32_t sig = homeBottomSignature();
  const bool overlayCrossesBand = homeCrossBandActive();
  // No periodic full-screen refresh: it created a predictable long frame every
  // 10 seconds. The lower band is deterministic and gets a full present whenever
  // it actually changes, an overlay crosses it, or we return to the home screen.
  (void)now;
  return !gHomePanelValid || overlayCrossesBand || sig != gHomeBottomSig;
}

static void presentHomeFrame() {
  const uint32_t now = millis();
  const uint32_t sig = homeBottomSignature();
  if (gHomeNeedFullThisFrame) {
    gfx->flush();
    // A full transfer that contains an overlay is visually complete, but it is
    // not a reusable HOME lower-panel cache.  Leaving this false forces one
    // clean full transfer on the first frame after the overlay disappears.
    // Afterwards normal 0..311 partial presents resume with no FPS penalty.
    const bool cleanHome = !homeCrossBandActive();
    gHomePanelValid = cleanHome;
    if (cleanHome) gHomeBottomSig = sig;
  } else {
    // Row 312 is where the care panel starts. The Pokemon/poop/scene animation
    // ends above it, so transferring 0..311 preserves full animation FPS while
    // eliminating ~33% of QSPI traffic on unchanged home frames.
    canvasPresentBand(0, 312);
  }
}

void render() {
  crumbDrop();   // so a crash can name the screen it happened on
  if (uiCurrentScreen() != SCR_MAIN) gHomePanelValid = false;
  if (pet.awaitingStarter()) {  // primera partida: region y luego inicial
    if (!starterRegionDone) renderRegionPick(RPICK_FOR_START);
    else renderStarterSelect();
    return;
  }
  // Modal team picker first: a stale party/box or hub flag may never paint on
  // top of gym/boss selection.
  if (pickOpen) { renderPick(); return; }
  if (digiDexOpen) { renderDigiDex(); return; }
  if (digiOpen) { renderDigi(); return; }
  if (bagOpen) { renderBag(); return; }
  if (missionOpen) { renderMissions(); return; }
  if (exploreOpen) { renderExplore(); return; }
  if (bossOpen) { renderBoss(); return; }
  if (towerOpen) { renderTower(); return; }
  if (treasureOpen) { renderTreasure(); return; }
  if (rivalOpen) { renderRival(); return; }
  if (adventureOpen) { renderAdventure(); return; }
  if (galleryOpen) {
    if (galleryPick) { renderRegionPick(RPICK_FOR_DEX); return; }
    renderGallery();
    return;
  }
  if (movePickOpen) {
    renderMovePick();
    return;
  }
  if (partyOpen) {
    if (boxOpen) {
      if (boxDetail) renderBoxDetail();
      else renderBox();
    } else if (partyDetail) renderPartyDetail();
    else renderParty();
    return;
  }
  if (gameOpen) {
    renderGame();
    return;
  }
  if (sackOpen) {
    renderSack();
    return;
  }
  if (spdOpen) {
    renderSpeed();
    return;
  }
  if (hpOpen) {
    renderVitality();
    return;
  }
  if (trainOpen) {
    renderTrain();
    return;
  }
  if (kbOpen) {
    renderKeyboard();
    return;
  }
  if (clockOpen) {
    renderClock();
    return;
  }
  if (battleOpen) {
    btlLinkPoll();
    renderBattle();
    return;
  }
  if (lanOpen) {
    renderLan();
    return;
  }
  if (gymOpen) {
    if (gymPick) renderRegionPick(RPICK_FOR_GYMS);
    else renderGyms();
    return;
  }
  if (playerOpen) {
    renderPlayer();
    return;
  }
  if (pet.hasLearnOffer()) {
    renderLearn();
    return;
  }
  if (cardOpen) {
    renderCard();
    return;
  }
  // Random events wait until the player is back on the home screen instead of
  // interrupting a battle, menu, Pokedex page or minigame.
  if (extras.hasEvent()) { renderRandomEvent(); return; }
  int h = sceneHour();
  gNight = h < 6 || h >= 20;
  gHomeNeedFullThisFrame = homeNeedsFullPresent(millis());
  // The scene is rendered off-screen first; no black intermediate frame is sent.
  // que un flush DMA solapado nunca capture negro a medias (anti-parpadeo)
  drawScene(currentSceneType(), millis(), gNight);
  drawWeatherBadge();   // v3.39+: always-visible compact current-weather indicator

  if (pet.ceremony) {
    const char *msg = (pet.ceremony == CER_FAREWELL) ? T(S_FAREWELL)
                      : (pet.ceremony == CER_RUNAWAY) ? T(S_RUNAWAY)
                                                      : T(S_GOODBYE);
    drawHeader(creatureName(pet.speciesId), scenePokemonNameColor(currentSceneType(), gNight), msg);
    drawCeremony();
    gfx->flush();
    return;
  }

  if (pet.isEgg()) {
    drawHeader(T(S_EGG_HDR), inkColor(), eggMsg());
    drawCareTabs();
    int s = 5, x = CX - 16 * s, y = PET_CY - 16 * s;
    drawMap(SPR_EGG, SPRITE_H, x, y, s, false);
    if (pet.eggCracks() >= 1)
      for (auto &c : CRACK1) gfx->fillRect(x + c[0] * s, y + c[1] * s, s, s, INK_K);
    if (pet.eggCracks() >= 2)
      for (auto &c : CRACK2) gfx->fillRect(x + c[0] * s, y + c[1] * s, s, s, INK_K);
    if (pet.eggRarity() >= R_RARO) {
      const char *rar = (pet.eggRarity() == R_LEGENDARIO) ? T(S_EGG_LEGEND) : T(S_EGG_RARE);
      gfx->setTextColor(pet.eggRarity() == R_LEGENDARIO ? UI_BAR_WARN : 0x4C98);
      uiSetTextSize(2);
      uiSetCursor(CX - uiTextHalfWidth(rar, 2), 316);
      gfx->print(rar);
    }
    char reg[24];
    snprintf(reg, sizeof(reg), T(S_POKEDEX_FMT), pet.registeredCount(), regionDexCount(REGION_ALL));
    gfx->fillRect(0, 312, 466, 154, gNight ? UI_BG_NIGHT : UI_BG_DAY);
    gfx->setTextColor(inkColor());
    uiSetTextSize(2);
    uiSetCursor(CX - uiTextHalfWidth(reg, 2), 344);
    gfx->print(reg);

    if (pet.huntTargetDex() >= 1) {
      char hunt[48];
      int16_t hd = pet.huntTargetDex();
      if (pet.isRegistered(hd))
        snprintf(hunt, sizeof(hunt), "찾기: %s  %u/6", localizedSpeciesName(hd), pet.huntMisses());
      else
        snprintf(hunt, sizeof(hunt), "찾기: N.%03d  %u/6", hd, pet.huntMisses());
      gfx->setTextColor(inkColor());
      uiDrawCenteredFit(hunt, CX, 358, 420, 2, 1);
    }

    // Which generation this egg comes from. It lives HERE rather than in the
    // settings screen because this is the only moment it does anything: the
    // species is decided when the egg appears, so choosing the region is
    // something you do to the egg in front of you.
    drawEggRegion();
  } else {
    char name[48];
    const char *base = pet.nick[0] ? pet.nick : creatureName(pet.speciesId);
    snprintf(name, sizeof(name), T(S_NAME_FMT), pet.shiny ? "*" : "", base, pet.level());
    drawHeader(name, scenePokemonNameColor(currentSceneType(), gNight), statusMsg());
    drawCareTabs();
    drawStreakBadge();
    drawPet();
    drawBath();
    drawPoops();
    // The lower controls are static for many animation frames. Draw them only
    // when their signature changed (or a full safety/overlay present is due).
    // On partial frames the physical AMOLED keeps the already-correct lower
    // panel while the upper 312 rows continue at the full animation cadence.
    if (gHomeNeedFullThisFrame) {
      gfx->fillRect(0, 312, 466, 154, gNight ? UI_BG_NIGHT : UI_BG_DAY);
      drawBars();
      drawButtons();
    }
    drawCelebration();
    if (pet.canRunawayNow()) drawRunawayButton();          // CTA sombrio: escapada (abandono)
    // Voluntary farewell/retirement is intentionally menu-only.
  }

  if (pet.sleeping) {
    gfx->setTextColor(UI_INK_NIGHT);
    uiSetTextSize(3);
    uiSetCursor(320, 130);
    gfx->print("Zz");
  }

  // Classic one-tap food selector, expanded with two portals instead of
  // replacing it with a persistent inventory screen. From left to right:
  // red / blue / green berry / sweet candy / earned items / type TMs.
  if (feedMenuUntil) {
    if (millis() > feedMenuUntil) {
      feedMenuUntil = 0;
    } else {
      gfx->fillRoundRect(52, 286, 362, 68, 14, UI_WHITE);
      gfx->drawRoundRect(52, 286, 362, 68, 14, inkColor());
      const int cx6[6] = { 85, 144, 203, 262, 321, 380 };
      drawMap(SPR_ICON_FOOD,    16, cx6[0] - 24, 296, 3, false);
      drawMap(SPR_ICON_BERRY_B, 16, cx6[1] - 24, 296, 3, false);
      drawMap(SPR_ICON_BERRY_G, 16, cx6[2] - 24, 296, 3, false);
      drawMap(SPR_ICON_CANDY,   16, cx6[3] - 24, 296, 3, false);

      // Earned-item bag: procedural icon, so no new SD/GitHub image asset.
      uint16_t ik = inkColor();
      gfx->drawRoundRect(cx6[4] - 19, 302, 38, 34, 7, ik);
      gfx->drawRoundRect(cx6[4] - 10, 296, 20, 12, 6, ik);
      gfx->fillRect(cx6[4] - 2, 311, 4, 16, ik);
      gfx->fillRect(cx6[4] - 8, 317, 16, 4, ik);

      // TM portal: deliberately text-like and high contrast at 466x466.
      gfx->fillRoundRect(cx6[5] - 21, 299, 42, 38, 8, C565(0x55, 0x69, 0xb7));
      gfx->drawRoundRect(cx6[5] - 21, 299, 42, 38, 8, ik);
      gfx->setTextColor(UI_WHITE);
      uiSetTextSize(2);
      uiSetCursor(cx6[5] - uiTextHalfWidth("TM", 2), 309);
      gfx->print("TM");
    }
  }

  // dialogo "soltar?" (pulsacion larga sobre el bicho)
  if (confirmUntil) {
    if (millis() > confirmUntil) {
      confirmUntil = 0;
    } else {
      gfx->fillRoundRect(94, 168, 278, 152, 16, UI_WHITE);
      gfx->drawRoundRect(94, 168, 278, 152, 16, UI_INK);
      char q[96];
      snprintf(q, sizeof(q), T(S_RELEASE_FMT), creatureName(pet.speciesId));
      gfx->setTextColor(UI_INK);
      uiSetTextSize(2);
      uiSetCursor(CX - uiTextHalfWidth(q, 2), 196);
      gfx->print(q);
      gfx->fillRoundRect(118, 252, 100, 52, 12, UI_BAR_OK);
      gfx->setTextColor(UI_WHITE);
      uiSetCursor(118 + (100 - uiTextWidth(T(S_YES), 2)) / 2, 270);
      gfx->print(T(S_YES));
      gfx->fillRoundRect(248, 252, 100, 52, 12, UI_BAR_BAD);
      uiSetCursor(248 + (100 - uiTextWidth(T(S_NO), 2)) / 2, 270);
      gfx->print(T(S_NO));
    }
  }

  // dialogo de decision (evolucionar/mantener, despedirse/quedaros)
  if (choiceKind) {
    if (millis() > choiceUntil) choiceKind = 0;
    else drawChoiceDialog();
  }

  // "<name> joined the party!" after a farewell or release
  if (partyBannerUntil) {
    if (millis() > partyBannerUntil) {
      partyBannerUntil = 0;
    } else {
      char b[96];
      snprintf(b, sizeof(b), T(S_PARTY_JOINED), partyBannerName);
      gfx->fillRoundRect(53, 176, 360, 74, 16, UI_BAR_OK);
      gfx->drawRoundRect(53, 176, 360, 74, 16, UI_INK);
      gfx->setTextColor(UI_WHITE);
      uiSetTextSize(2);
      uiSetCursor(CX - uiTextHalfWidth(b, 2), 206);
      gfx->print(b);
    }
  }

  if (menuOpen) drawMenu();

  presentHomeFrame();
}

// ---------- 방어 훈련: 게이지 타이밍 ----------

static int16_t defenseNeedleX(uint32_t now) {
  const int range = DEF_GAUGE_W - 12;
  uint16_t speed = (uint16_t)(155 + defRound * 10);  // px/s, gently faster each round
  uint32_t oneWay = (uint32_t)range * 1000UL / (speed ? speed : 1);
  if (oneWay < 650) oneWay = 650;
  uint32_t cycle = oneWay * 2UL;
  uint32_t t = cycle ? (now - defRoundStarted) % cycle : 0;
  int dx = (t <= oneWay)
             ? (int)((uint64_t)range * t / oneWay)
             : (int)((uint64_t)range * (cycle - t) / oneWay);
  // Alternate the starting side so twelve rounds do not feel identical.
  if (defRound & 1) dx = range - dx;
  return (int16_t)(DEF_GAUGE_X + 6 + dx);
}

static int defensePerfectHalf() {
  int v = 31 - (int)defRound;
  return v < 19 ? 19 : v;
}
static int defenseGoodHalf() {
  int v = 68 - (int)defRound * 2;
  return v < 46 ? 46 : v;
}
static int defenseBlockHalf() {
  int v = 108 - (int)defRound * 2;
  return v < 84 ? 84 : v;
}

// v3.63.6 training drops ----------------------------------------------------
// A properly completed training session always earns five IV berries. There is a
// 30% bonus roll that upgrades the total reward to nine. A small 10% redirect keeps HP IV berries
// obtainable even though HP has no dedicated minigame.
static uint8_t grantTrainingIvBerry(uint8_t primary, bool completed, uint8_t &count) {
  count = 0;
  if (!completed) return XITEM_COUNT;
  uint8_t id = primary;
  count = random(100) < 30 ? 9 : 5;
  extras.giveItem(id, count);
  return id;
}

// Separate from the original XITEM_SHINY/반짝부적. This rare berry changes the
// CURRENT Pokemon and never consumes or replaces the next-egg Shiny boost.
static bool grantTrainingShinyBerry(bool completed) {
  if (!completed || random(100) >= 30) return false;  // 30% per proper completion
  extras.giveItem(XITEM_SHINY_BERRY, 1);
  return true;
}

static void finishDefense() {
  if (gameOverUntil) return;
  gameNewHi = gameScore > pet.gameHi;
  extras.beginBatch();
  gameGain = pet.playResult(gameScore);
  bool rewardReady = defRound >= DEF_ROUNDS;
  gameIvReward = grantTrainingIvBerry(XITEM_IV_DEF, rewardReady, gameIvRewardCount);
  gameShinyBerryReward = grantTrainingShinyBerry(rewardReady);
  extras.endBatch(false);
  queueTrainingPersist();
  sfxPlay(gameShinyBerryReward || gameIvRewardCount > 1 || (gameNewHi && gameScore) ? SFX_MEDAL : SFX_LEVEL);
  gameOverUntil = millis() ? millis() : 1;
}

void startGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  gameOpen = true;
  gameOverUntil = 0;
  gameScore = 0;
  gameGain = 0;
  gameNewHi = false;
  gameIvReward = XITEM_COUNT;
  gameIvRewardCount = 0;
  gameShinyBerryReward = false;
  defRound = 0;
  defBlocks = defGoods = defPerfects = 0;
  defRoundStarted = millis();
  defFeedbackUntil = 0;
  defFeedback = 0;
  defStoppedX = CX;
}

void leaveGame() {
  if (!gameOverUntil && gameOpen) finishDefense();
  gameOpen = false;
  digiReact(DIGI_ANIM_TRAIN);
}

void leaveSack() {
  if (!sackOverUntil) {
    extras.beginBatch();
    pet.trainStrength(sackHits);
    extras.endBatch(false);
    queueTrainingPersist();
  }
  sackOpen = false;
  digiReact(DIGI_ANIM_TRAIN);
}

void leaveSpeed() {
  if (!spdOverUntil) {
    extras.beginBatch();
    pet.trainSpeed(spdHits);
    extras.endBatch(false);
    queueTrainingPersist();
  }
  spdOpen = false;
  digiReact(DIGI_ANIM_TRAIN);
}

void gameTap(int16_t x, int16_t y) {
  (void)x; (void)y;
  if (gameOverUntil) { gameOpen = false; digiReact(DIGI_ANIM_TRAIN); return; }
  uint32_t now = millis();
  // Ignore extra touches during the short result flash. This is both easier to
  // understand and prevents accidental double scoring from touch bounce.
  if (defFeedbackUntil && (int32_t)(defFeedbackUntil - now) > 0) return;
  if (defRound >= DEF_ROUNDS) return;

  int16_t nx = defenseNeedleX(now);
  defStoppedX = nx;
  int dist = abs((int)nx - CX);
  int pHalf = defensePerfectHalf();
  int gHalf = defenseGoodHalf();
  int bHalf = defenseBlockHalf();

  if (dist <= pHalf) {
    gameScore += 3;
    defPerfects++;
    defFeedback = 3;
    // Long, bright cue: short HEART notes could finish while the PA was waking.
    sfxPlay(SFX_MEDAL);
  } else if (dist <= gHalf) {
    gameScore += 2;
    defGoods++;
    defFeedback = 2;
    sfxPlay(SFX_LEVEL);
  } else if (dist <= bHalf) {
    gameScore += 1;
    defBlocks++;
    defFeedback = 1;
    sfxPlay(SFX_PLAY);
  } else {
    defFeedback = 4;
    sfxPlay(SFX_DENY);
  }

  defRound++;
  defFeedbackUntil = now + DEF_FEEDBACK_MS;
}

// Kept under the historical name because loop() already calls it. The new
// gauge game has no physics or collisions: this function only advances from a
// feedback flash to the next round, which makes it extremely cheap and stable.
static void stepDefenseGauge(uint32_t now) {
  if (gameOverUntil) return;
  if (defFeedbackUntil && (int32_t)(now - defFeedbackUntil) >= 0) {
    defFeedbackUntil = 0;
    defFeedback = 0;
    if (defRound >= DEF_ROUNDS) {
      finishDefense();
      return;
    }
    defRoundStarted = now;
  }
}

// Fast training backdrop: keeps the current Pokemon type palette but avoids the
// many tiny decorative draw calls of the full habitat. The full habitat remains
// on the home screen; training prioritizes consistent frame pacing and contrast.
static void drawTrainingBackdrop(uint8_t type, uint32_t now, float scroll, int groundY) {
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t top, bot;
  typeSkyColors(type, night, sceneHour(), top, bot);
  gfx->fillRect(0, 0, 466, groundY / 2, top);
  gfx->fillRect(0, groundY / 2, 466, groundY - groundY / 2, bot);
  uint16_t soil = typeGroundColor(type);
  if (night) soil = lerp565(soil, C565(0x16, 0x1c, 0x30), 9, 16);
  uint16_t dark = lerp565(soil, C565(0x10, 0x18, 0x20), night ? 11 : 7, 16);
  uint16_t pale = lerp565(soil, UI_WHITE, night ? 2 : 6, 16);
  gfx->fillRect(0, groundY, 466, 466 - groundY, soil);
  gfx->fillRect(0, groundY - 5, 466, 5, dark);

  // Low-cost parallax landmarks and ground dashes communicate motion.
  int off = ((int)(scroll * 0.22f)) % 150;
  for (int i = -1; i < 4; i++) {
    int x = i * 150 - off;
    gfx->fillRoundRect(x, groundY - 56, 92, 30, 15, pale);
  }
  int goff = ((int)scroll) % 92;
  for (int i = -1; i < 7; i++) {
    int x = i * 92 - goff;
    gfx->fillRect(x, groundY + 28, 48, 5, dark);
  }
}

static void drawGaugeBackdrop(uint8_t type) {
  // Deliberately tiny draw budget: no SD sprite, no parallax, no particles.
  // The old defence game could hitch while drawing a PMD frame at the same time
  // as touch/audio work. A few solid primitives keep frame pacing predictable.
  uint16_t base = lerp565(typeColor(type), UI_WHITE, 12, 16);
  uint16_t rim  = lerp565(typeColor(type), RGB565_BLACK, 5, 16);
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, base);
  gfx->drawCircle(CX, CY, 229, rim);
  gfx->drawCircle(CX, CY, 228, rim);
}

void renderGame() {
  uint32_t now = millis();
  uint8_t type = currentSceneType();
  drawGaugeBackdrop(type);

  if (gameOverUntil) {
    gfx->fillRoundRect(62, 82, 342, 300, 24, UI_WHITE);
    gfx->drawRoundRect(62, 82, 342, 300, 24, UI_INK);
    gfx->setTextColor(UI_INK);
    uiSetTextSize(3);
    uiSetCursor(CX - uiTextHalfWidth("방어 훈련 종료!", 3), 108);
    gfx->print("방어 훈련 종료!");
    char b[64];
    snprintf(b, sizeof(b), "점수 %u / 36", (unsigned)gameScore);
    uiSetTextSize(4); uiSetCursor(CX-uiTextHalfWidth(b,4),158); gfx->print(b);
    snprintf(b, sizeof(b), "PERFECT %u · GOOD %u", (unsigned)defPerfects,
             (unsigned)defGoods);
    uiSetTextSize(2); uiSetCursor(CX-uiTextHalfWidth(b,2),216); gfx->print(b);
    snprintf(b, sizeof(b), "BLOCK %u", (unsigned)defBlocks);
    uiSetCursor(CX-uiTextHalfWidth(b,2),246); gfx->print(b);
    snprintf(b, sizeof(b), "방어 +%u", (unsigned)gameGain);
    gfx->setTextColor(UI_BAR_OK); uiSetTextSize(3);
    uiSetCursor(CX-uiTextHalfWidth(b,3),286); gfx->print(b);
    if (gameIvReward < XITEM_COUNT && gameIvRewardCount) {
      snprintf(b, sizeof(b), "훈련 보상: %s x%u", extras.itemNameKo(gameIvReward), (unsigned)gameIvRewardCount);
      gfx->setTextColor(UI_BAR_OK);
      uiDrawCenteredFit(b, CX, 306, 318, 2, 1);
    }
    if (gameShinyBerryReward) {
      snprintf(b, sizeof(b), "희귀 보상: %s x1", extras.itemNameKo(XITEM_SHINY_BERRY));
      gfx->setTextColor(UI_BAR_WARN);
      uiDrawCenteredFit(b, CX, 326, 318, 2, 1);
    }
    const char *foot = gameNewHi ? "신기록!  터치해서 돌아가기" : "터치해서 돌아가기";
    gfx->setTextColor(gameNewHi ? UI_BAR_WARN : UI_TRACK); uiSetTextSize(2);
    uiSetCursor(CX-uiTextHalfWidth(foot,2),352); gfx->print(foot);
    gfx->flush();
    return;
  }

  bool feedbackActive = defFeedbackUntil && (int32_t)(defFeedbackUntil-now)>0;

  // Header: fixed 12 attempts, so there is no timer and no sudden game-over.
  gfx->fillRoundRect(82, 28, 302, 72, 16, UI_WHITE);
  gfx->drawRoundRect(82, 28, 302, 72, 16, UI_INK);
  gfx->setTextColor(UI_INK); uiSetTextSize(3);
  uiSetCursor(CX-uiTextHalfWidth("방어 게이지",3),43); gfx->print("방어 게이지");
  char b[40];
  uint8_t shownRound = feedbackActive ? defRound : (uint8_t)(defRound < DEF_ROUNDS ? defRound + 1 : DEF_ROUNDS);
  snprintf(b, sizeof(b), "%u / %u   점수 %u", (unsigned)shownRound,
           (unsigned)DEF_ROUNDS, (unsigned)gameScore);
  gfx->setTextColor(UI_TRACK); uiSetTextSize(1);
  uiSetCursor(CX-uiTextHalfWidth(b,1),82); gfx->print(b);

  // Small shield emblem made only of primitives.
  uint16_t tc = typeColor(type);
  gfx->fillCircle(CX, 154, 42, UI_WHITE);
  gfx->drawCircle(CX, 154, 42, UI_INK);
  gfx->fillRoundRect(CX-25,126,50,48,12,tc);
  gfx->drawRoundRect(CX-25,126,50,48,12,UI_INK);
  gfx->fillTriangle(CX-25,154,CX+25,154,CX,190,tc);
  gfx->drawLine(CX-25,154,CX,190,UI_INK);
  gfx->drawLine(CX+25,154,CX,190,UI_INK);

  int pHalf = defensePerfectHalf();
  int gHalf = defenseGoodHalf();
  int bHalf = defenseBlockHalf();

  // Gauge body and nested success zones. The zones narrow only gently as the
  // rounds progress, keeping the game accessible while still rewarding timing.
  gfx->fillRoundRect(DEF_GAUGE_X, DEF_GAUGE_Y, DEF_GAUGE_W, DEF_GAUGE_H, 13, UI_WHITE);
  gfx->drawRoundRect(DEF_GAUGE_X, DEF_GAUGE_Y, DEF_GAUGE_W, DEF_GAUGE_H, 13, UI_INK);
  uint16_t blockC = lerp565(UI_BAR_OK, UI_WHITE, 9, 16);
  uint16_t goodC  = lerp565(UI_BAR_WARN, UI_WHITE, 7, 16);
  gfx->fillRect(CX-bHalf, DEF_GAUGE_Y+5, bHalf*2, DEF_GAUGE_H-10, blockC);
  gfx->fillRect(CX-gHalf, DEF_GAUGE_Y+5, gHalf*2, DEF_GAUGE_H-10, goodC);
  gfx->fillRect(CX-pHalf, DEF_GAUGE_Y+5, pHalf*2, DEF_GAUGE_H-10, UI_BAR_WARN);
  gfx->drawFastVLine(CX, DEF_GAUGE_Y-8, DEF_GAUGE_H+16, UI_INK);

  int16_t needleX = feedbackActive ? defStoppedX : defenseNeedleX(now);
  gfx->fillRoundRect(needleX-4, DEF_GAUGE_Y-13, 8, DEF_GAUGE_H+26, 4, UI_BAR_BAD);
  gfx->drawFastVLine(needleX, DEF_GAUGE_Y-11, DEF_GAUGE_H+22, UI_WHITE);

  gfx->setTextColor(UI_TRACK); uiSetTextSize(1);
  const char *zones = "바깥 BLOCK · 안쪽 GOOD · 중앙 PERFECT";
  uiSetCursor(CX-uiTextHalfWidth(zones,1), DEF_GAUGE_Y+56);
  gfx->print(zones);

  if (feedbackActive) {
    const char *msg = defFeedback==3 ? "PERFECT! +3" : defFeedback==2 ? "GOOD! +2" :
                      defFeedback==1 ? "BLOCK! +1" : "MISS! +0";
    uint16_t c = defFeedback==3 ? UI_BAR_WARN : defFeedback==2 ? UI_BAR_OK :
                 defFeedback==1 ? typeColor(type) : UI_BAR_BAD;
    gfx->fillRoundRect(121,326,224,54,14,UI_WHITE);
    gfx->drawRoundRect(121,326,224,54,14,c);
    gfx->setTextColor(c); uiSetTextSize(3);
    uiSetCursor(CX-uiTextHalfWidth(msg,3),341); gfx->print(msg);
  } else {
    gfx->setTextColor(UI_INK); uiSetTextSize(2);
    const char *hint = "바늘이 가운데 올 때 터치!";
    uiSetCursor(CX-uiTextHalfWidth(hint,2),342); gfx->print(hint);
  }

  gfx->setTextColor(UI_TRACK); uiSetTextSize(1);
  const char *sub = "제한시간 없음 · 12번 도전 · 중앙에 가까울수록 높은 점수";
  uiSetCursor(CX-uiTextHalfWidth(sub,1),410); gfx->print(sub);
  gfx->flush();
}

// ---------- saco de entrenamiento (entrena la fuerza) ----------

void startSack() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  sackOpen = true;
  sackUntil = millis() + 15000;
  sackOverUntil = 0;
  sackHits = 0;
  sackShake = 0;
  sackNewHi = false;
  sackIvReward = XITEM_COUNT;
  sackIvRewardCount = 0;
  sackShinyBerryReward = false;
}

void sackTap() {
  if (millis() >= sackUntil) return;  // ya termino el tiempo
  sackHits++;
  sackShake = 16;  // sacude el saco
}

void drawGameScene();  // prototipo (definida mas abajo)

void renderSack() {
  uint32_t now = millis();
  // v3.22: the full type habitat was expensive to redraw during rapid tapping.
  // Use the same lightweight training backdrop as speed/defence so hit input
  // remains responsive while keeping the current Pokemon's type palette.
  drawTrainingBackdrop(currentSceneType(), now, 0.0f, 376);
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;

  // pantalla de resultado
  if (sackOverUntil) {
    if (now > sackOverUntil) { sackOpen = false; digiReact(DIGI_ANIM_TRAIN); return; }
    char b[20];
    snprintf(b, sizeof(b), T(S_HITS_FMT), sackHits);
    gfx->setTextColor(ink);
    uiSetTextSize(4);
    uiSetCursor(CX - uiTextHalfWidth(b, 4), 150);
    gfx->print(b);
    char g[18];
    snprintf(g, sizeof(g), T(S_STR_GAIN_FMT), sackGain);
    gfx->setTextColor(UI_BAR_BAD);
    uiSetTextSize(3);
    uiSetCursor(CX - uiTextHalfWidth(g, 3), 210);
    gfx->print(g);
    uiSetTextSize(2);
    if (sackNewHi && sackHits > 0) {
      gfx->setTextColor(UI_BAR_WARN);
      uiSetCursor(CX - uiTextHalfWidth(T(S_NEW_RECORD), 2), 256);
      gfx->print(T(S_NEW_RECORD));
    } else {
      char r[18];
      snprintf(r, sizeof(r), T(S_RECORD_FMT), pet.strHi);
      gfx->setTextColor(ink);
      uiSetCursor(CX - uiTextHalfWidth(r, 2), 256);
      gfx->print(r);
    }
    if (sackIvReward < XITEM_COUNT && sackIvRewardCount) {
      char rw[72]; snprintf(rw, sizeof(rw), "훈련 보상: %s x%u", extras.itemNameKo(sackIvReward), (unsigned)sackIvRewardCount);
      gfx->setTextColor(UI_BAR_OK);
      uiDrawCenteredFit(rw, CX, 298, 390, 2, 1);
    }
    if (sackShinyBerryReward) {
      char rw[72]; snprintf(rw, sizeof(rw), "희귀 보상: %s x1", extras.itemNameKo(XITEM_SHINY_BERRY));
      gfx->setTextColor(UI_BAR_WARN);
      uiDrawCenteredFit(rw, CX, 320, 390, 2, 1);
    }
    gfx->flush();
    return;
  }

  // se acabaron los 15 s: aplicar entrenamiento
  if (now >= sackUntil) {
    sackNewHi = (sackHits > pet.strHi);
    extras.beginBatch();
    sackGain = pet.trainStrength(sackHits);
    bool rewardReady = sackHits > 0;
    sackIvReward = grantTrainingIvBerry(XITEM_IV_ATK, rewardReady, sackIvRewardCount);
    sackShinyBerryReward = grantTrainingShinyBerry(rewardReady);
    extras.endBatch(false);
    queueTrainingPersist();
    sfxPlay(sackShinyBerryReward || sackIvRewardCount > 1 || sackNewHi ? SFX_MEDAL : SFX_PLAY);
    sackOverUntil = now + 3500;
    gfx->flush();
    return;
  }

  // aporreo activo
  sackShake *= 0.84f;
  int off = (int)(sackShake * sinf(now * 0.05f));
  int sx = CX + off, top = 86, sy = 150;
  gfx->fillRect(CX - 3, 56, 6, top - 56, ink);          // gancho/cuerda
  gfx->fillRect(sx - 4, top - 30, 8, 34, ink);          // cadena
  gfx->fillRoundRect(sx - 42, top, 84, 150, 26, C565(0xb5, 0x3a, 0x3a));  // saco
  gfx->fillRoundRect(sx - 42, top, 84, 22, 18, C565(0x7e, 0x28, 0x28));   // tapa
  gfx->drawRoundRect(sx - 42, top, 84, 150, 26, ink);
  gfx->fillRect(sx - 42, top + 70, 84, 4, C565(0x7e, 0x28, 0x28));        // costura

  // contador de golpes
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", sackHits);
  gfx->setTextColor(ink);
  uiSetTextSize(6);
  uiSetCursor(CX - uiTextHalfWidth(buf, 6), 268);
  gfx->print(buf);

  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(T(S_HIT_FAST), 2), 322);
  gfx->print(T(S_HIT_FAST));

  // barra de tiempo
  uint32_t left = sackUntil - now;
  int bw = 280, fw = (int)((uint32_t)bw * left / 15000);
  gfx->fillRoundRect(CX - bw / 2, 350, bw, 16, 5, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(CX - bw / 2, 350, fw, 16, 5, UI_BAR_OK);

  gfx->flush();
}

// fondo del minijuego: hatibat del bicho (cielo por hora + suelo del bioma)
void drawGameScene() {
  int hh = sceneHour();
  bool night = hh < 6 || hh >= 20;
  drawTypeScene(currentSceneType(), millis(), night, 376);
}

// ---------- ficha del bicho (deslizar vertical) ----------

// una fila de la ficha: etiqueta, barra, valor y (si iv != IV_NONE) el valor
// individual que fija el techo de ese stat
// (sin argumento por defecto: el generador de prototipos de Arduino los
// descarta y las llamadas que lo omitan no compilarian)
#define IV_NONE 0xFF
void drawCardStat(int y, const char *label, uint16_t val, uint16_t maxBar,
                  uint16_t color, uint8_t iv) {
  gfx->setTextColor(UI_INK);
  uiSetTextSize(2);
  uiSetCursor(70, y);
  gfx->print(label);
  // The bar used to start at 112, which leaves 42px for a label drawn at size 2
  // -- three characters. BOND (EN), LIEN (FR) and LACO (PT) are four, so the
  // label ran under the bar. 132 fits five, with the bar narrowed to keep the
  // number clear of it.
  int bw = 130;
  int fw = (int)val * bw / maxBar;
  if (fw > bw) fw = bw;
  gfx->fillRoundRect(132, y + 2, bw, 11, 3, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(132, y + 2, fw, 11, 3, color);
  char num[8];
  snprintf(num, sizeof(num), "%u", val);
  uiSetCursor(272, y);
  gfx->print(num);
  if (iv != IV_NONE) {
    char b[10];
    snprintf(b, sizeof(b), T(S_IV_FMT), iv);
    // IV is decision-critical information, so ordinary values use full black
    // instead of the low-contrast disabled-text grey. Prefer size 3; localized
    // labels that need more room automatically fall back to size 2.
    gfx->setTextColor(iv >= 31 ? UI_BAR_WARN : UI_INK);
    uiDrawLeftFit(b, 326, y - 4, 100, 3, 2);
  }
}

// ---------- ajuste de hora en pantalla (deslizar abajo) ----------
// El usuario pone su hora LOCAL a ojo; el firmware la usa tal cual, asi que
// no hay que gestionar zona horaria. Preserva el dia (no rompe racha/edad).

void openClock() {
  uint32_t e = pet.lastSeenEpoch ? pet.lastSeenEpoch : rtcEpoch();
  clockH = (e / 3600) % 24;
  clockM = (e / 60) % 60;
  clockOpen = true;
}

void applyClock() {
  uint32_t base = pet.lastSeenEpoch ? pet.lastSeenEpoch : rtcEpoch();
  uint32_t e = (base / 86400) * 86400 + (uint32_t)clockH * 3600 + (uint32_t)clockM * 60;
  rtcSetEpoch(e);
  pet.setClock(e);
  clockOpen = false;
}

void drawClockBtn(int x, int y, const char *l) {
  gfx->fillRoundRect(x, y, 58, 58, 12, UI_WHITE);
  gfx->drawRoundRect(x, y, 58, 58, 12, UI_INK);
  gfx->setTextColor(UI_INK);
  uiSetTextSize(4);
  uiSetCursor(x + 17, y + 15);
  gfx->print(l);
}

#define AUDIO_TOGGLE_GAME 0
#define AUDIO_TOGGLE_BUTTON 1

// Keep the parameter as a built-in type. Arduino's .ino preprocessor may emit
// function prototypes before user-defined enum declarations, which caused the
// v3.41 GitHub build failure (AudioToggleKind has not been declared).
static void drawAudioTogglePill(int x, int y, int w, int h, bool on, uint8_t kind) {
  uint16_t fill = UI_WHITE;
  if (on) fill = (kind == AUDIO_TOGGLE_GAME) ? UI_BAR_OK : UI_BAR_WARN;
  gfx->fillRoundRect(x, y, w, h, 8, fill);
  gfx->drawRoundRect(x, y, w, h, 8, UI_INK);

  uint16_t ink = on ? UI_BG_DAY : UI_INK;
  int ix = x + 10;
  int iy = y + 6;

  if (kind == AUDIO_TOGGLE_GAME) {
    // Pixel-style music note icon.
    gfx->drawLine(ix + 9, iy + 1, ix + 9, iy + 13, ink);
    gfx->drawLine(ix + 18, iy, ix + 18, iy + 11, ink);
    gfx->drawLine(ix + 9, iy + 1, ix + 18, iy, ink);
    gfx->fillCircle(ix + 6, iy + 15, 3, ink);
    gfx->fillCircle(ix + 16, iy + 13, 3, ink);
  } else {
    // Small button + sound waves icon for UI beeps.
    gfx->fillRoundRect(ix + 1, iy + 4, 12, 10, 3, ink);
    gfx->drawLine(ix + 14, iy + 9, ix + 17, iy + 9, ink);
    gfx->drawLine(ix + 18, iy + 7, ix + 18, iy + 11, ink);
    gfx->drawLine(ix + 20, iy + 7, ix + 23, iy + 9, ink);
    gfx->drawLine(ix + 20, iy + 11, ix + 23, iy + 9, ink);
    gfx->drawLine(ix + 24, iy + 5, ix + 28, iy + 9, ink);
    gfx->drawLine(ix + 24, iy + 13, ix + 28, iy + 9, ink);
  }

  const int cx = x + w - 18;
  const int cy = y + h / 2;
  if (on) {
    uint16_t badge = (kind == AUDIO_TOGGLE_GAME) ? C565(0x28, 0x87, 0x45)
                                                  : C565(0xd2, 0x82, 0x25);
    gfx->fillCircle(cx, cy, 7, badge);
    gfx->drawLine(cx - 4, cy + 0, cx - 1, cy + 3, UI_BG_DAY);
    gfx->drawLine(cx - 1, cy + 3, cx + 4, cy - 3, UI_BG_DAY);
  } else {
    gfx->drawCircle(cx, cy, 7, UI_TRACK);
    gfx->drawLine(cx - 4, cy + 4, cx + 4, cy - 4, UI_TRACK);
  }
}

// Sound settings share the time/settings screen. Two compact rows keep game
// audio and the handheld-style button beep independently controllable.
#define GAME_SND_Y 286
#define BTN_SND_Y 324
#define SND_ROW_H 30
#define SND_TOGGLE_X 24
#define SND_TOGGLE_W 112
#define VOL_MINUS_X 148
#define VOL_PLUS_X 278
#define VOL_BTN_W 44
#define VOL_METER_X 202
#define VOL_METER_W 62
#define LANG_PILL_Y GAME_SND_Y
#define LANG_PILL_H SND_ROW_H
#define LANG_PILL_X 338
#define LANG_PILL_W 94
#define SND_TEST_X 338
#define SND_TEST_W 94
#define CLOCK_OK_Y 370
static const char *const LANG_CODES[LANG_COUNT] = { "ES", "EN", "FR", "DE", "IT", "PT", "KO" };

void renderClock() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  uiSetTextSize(3);
  uiSetCursor(CX - uiTextHalfWidth(T(S_SET_TIME), 3), 44);
  gfx->print(T(S_SET_TIME));

  char t[8];
  snprintf(t, sizeof(t), "%02d:%02d", clockH, clockM);
  uiSetTextSize(7);
  // v3.42: center the large clock from Arduino_GFX's actual rendered bounds.
  // This avoids guessing the QuanPixel ASCII advance width. getTextBounds()
  // returns the real painted x-offset and width for the currently selected
  // font/scale, so the visible glyph box itself is centered on CX.
  int16_t tbx = 0, tby = 0;
  uint16_t tbw = 0, tbh = 0;
  gfx->getTextBounds(t, 0, 0, &tbx, &tby, &tbw, &tbh);
  int clockX = CX - ((int)tbx + (int)tbw / 2);
#ifndef TAMAPOKE_EMU
  if (gLang == LANG_KO) clockX -= KO_FONT_X_BIAS * gUiTextScale;
#endif
  uiSetCursor(clockX, 108);
  gfx->print(t);

  drawClockBtn(104, 190, "-");  // hora -
  drawClockBtn(170, 190, "+");  // hora +
  drawClockBtn(252, 190, "-");  // min -
  drawClockBtn(318, 190, "+");  // min +
  uiSetTextSize(2);
  gfx->setTextColor(UI_TRACK);
  uiSetCursor(120, 256);
  gfx->print(T(S_HOUR));
  uiSetCursor(276, 256);
  gfx->print(T(S_MIN));

  // Game sound row ---------------------------------------------------------
  bool snd = audioEnabled();
  drawAudioTogglePill(SND_TOGGLE_X, GAME_SND_Y, SND_TOGGLE_W, SND_ROW_H, snd,
                      AUDIO_TOGGLE_GAME);

  auto drawVolumeRow = [&](int y, uint8_t v) {
    for (int i = 0; i < 2; i++) {
      int bx = i ? VOL_PLUS_X : VOL_MINUS_X;
      bool live = i ? (v < 10) : (v > 0);
      gfx->fillRoundRect(bx, y, VOL_BTN_W, SND_ROW_H, 8, live ? UI_WHITE : UI_TRACK);
      gfx->drawRoundRect(bx, y, VOL_BTN_W, SND_ROW_H, 8, UI_INK);
      gfx->setTextColor(live ? UI_INK : 0x8410);
      uiSetTextSize(2);
      uiSetCursor(bx + VOL_BTN_W / 2 - 6, y + 8);
      gfx->print(i ? "+" : "-");
    }
    char vl[16];
    if (gLang == LANG_KO) snprintf(vl, sizeof(vl), "음량 %u", v);
    else snprintf(vl, sizeof(vl), "VOL %u", v);
    gfx->setTextColor(v ? UI_INK : UI_TRACK);
    uiSetTextSize(1);
    uiSetCursor(VOL_METER_X + (VOL_METER_W - uiTextWidth(vl, 1)) / 2, y + 3);
    gfx->print(vl);
    gfx->fillRoundRect(VOL_METER_X, y + 19, VOL_METER_W, 7, 3, UI_TRACK);
    if (v) gfx->fillRoundRect(VOL_METER_X, y + 19, VOL_METER_W * v / 10, 7, 3, UI_BAR_OK);
  };
  drawVolumeRow(GAME_SND_Y, audioVolume());

  // Button sound row: short two-note electronic beep, independently stored.
  bool btnSnd = audioButtonEnabled();
  drawAudioTogglePill(SND_TOGGLE_X, BTN_SND_Y, SND_TOGGLE_W, SND_ROW_H, btnSnd,
                      AUDIO_TOGGLE_BUTTON);
  drawVolumeRow(BTN_SND_Y, audioButtonVolume());

  // Forced audio-path diagnostic. This is intentionally independent of mute
  // toggles: if this button is silent, the issue is below the UI/settings layer.
  bool aok = audioReady();
  const char *testLbl = (gLang == LANG_KO) ? (aok ? "소리 테스트" : "오디오 오류")
                                            : (aok ? "TEST" : "AUDIO ERR");
  gfx->fillRoundRect(SND_TEST_X, BTN_SND_Y, SND_TEST_W, SND_ROW_H, 8,
                     aok ? UI_BAR_OK : UI_BAR_BAD);
  gfx->drawRoundRect(SND_TEST_X, BTN_SND_Y, SND_TEST_W, SND_ROW_H, 8, UI_INK);
  gfx->setTextColor(UI_BG_DAY);
  uiSetTextSize(1);
  uiSetCursor(SND_TEST_X + (SND_TEST_W - uiTextWidth(testLbl, 1)) / 2, BTN_SND_Y + 8);
  gfx->print(testLbl);

  // Language stays on the game-audio row so the extra button row costs no
  // additional screen depth.
  gfx->fillRoundRect(LANG_PILL_X, LANG_PILL_Y, LANG_PILL_W, LANG_PILL_H, 8, UI_WHITE);
  gfx->drawRoundRect(LANG_PILL_X, LANG_PILL_Y, LANG_PILL_W, LANG_PILL_H, 8, UI_INK);
  char lp[10];
  snprintf(lp, sizeof(lp), "%s >", LANG_CODES[gLang]);
  gfx->setTextColor(UI_INK);
  uiSetTextSize(2);
  uiSetCursor(LANG_PILL_X + (LANG_PILL_W - uiTextWidth(lp, 2)) / 2, LANG_PILL_Y + 8);
  gfx->print(lp);

  gfx->fillRoundRect(133, CLOCK_OK_Y, 200, 44, 14, UI_BAR_OK);
  gfx->setTextColor(UI_BG_DAY);
  uiSetTextSize(3);
  uiSetCursor(CX - 18, CLOCK_OK_Y + 9);
  gfx->print("OK");

  gfx->setTextColor(UI_TRACK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(T(S_CLOCK_CANCEL), 2), 420);
  gfx->print(T(S_CLOCK_CANCEL));

  // version del firmware (discreta, abajo del todo)
  char ver[20];
  snprintf(ver, sizeof(ver), "TamaPoke v%s", FW_VERSION);
  uiSetTextSize(1);
  uiSetCursor(CX - uiTextHalfWidth(ver, 1), 443);
  gfx->print(ver);
  gfx->flush();
}

void clockTap(int16_t x, int16_t y) {
  if (y >= 190 && y <= 248) {  // hour / minute +/-
    bool changed = true;
    if (x >= 104 && x < 162) clockH = (clockH + 23) % 24;
    else if (x >= 170 && x < 228) clockH = (clockH + 1) % 24;
    else if (x >= 252 && x < 310) clockM = (clockM + 59) % 60;
    else if (x >= 318 && x < 376) clockM = (clockM + 1) % 60;
    else changed = false;
    if (changed) sfxPlay(SFX_TAP);
    return;
  }

  if (y >= GAME_SND_Y && y <= GAME_SND_Y + SND_ROW_H) {
    if (x >= SND_TOGGLE_X && x < SND_TOGGLE_X + SND_TOGGLE_W) {
      audioSetEnabled(!audioEnabled());
      // The preview is a game cue, so its new game-volume setting is audible.
      if (audioEnabled()) sfxPlay(SFX_PLAY);
      return;
    }
    if (x >= VOL_MINUS_X && x < VOL_MINUS_X + VOL_BTN_W) {
      if (audioVolume() > 0) audioSetVolume(audioVolume() - 1);
      sfxPlay(SFX_PLAY);
      return;
    }
    if (x >= VOL_PLUS_X && x < VOL_PLUS_X + VOL_BTN_W) {
      if (audioVolume() < 10) audioSetVolume(audioVolume() + 1);
      sfxPlay(SFX_PLAY);
      return;
    }
    if (x >= LANG_PILL_X && x < LANG_PILL_X + LANG_PILL_W) {
      setLang((Lang)((gLang + 1) % LANG_COUNT));
      applyLanguageFont();
      sfxPlay(SFX_TAP);
      return;
    }
  }

  if (y >= BTN_SND_Y && y <= BTN_SND_Y + SND_ROW_H) {
    if (x >= SND_TEST_X && x < SND_TEST_X + SND_TEST_W) {
      audioSelfTest();
      return;
    }
    if (x >= SND_TOGGLE_X && x < SND_TOGGLE_X + SND_TOGGLE_W) {
      audioSetButtonEnabled(!audioButtonEnabled());
      if (audioButtonEnabled()) sfxPlay(SFX_TAP);
      return;
    }
    if (x >= VOL_MINUS_X && x < VOL_MINUS_X + VOL_BTN_W) {
      if (audioButtonVolume() > 0) audioSetButtonVolume(audioButtonVolume() - 1);
      sfxPlay(SFX_TAP);
      return;
    }
    if (x >= VOL_PLUS_X && x < VOL_PLUS_X + VOL_BTN_W) {
      if (audioButtonVolume() < 10) audioSetButtonVolume(audioButtonVolume() + 1);
      sfxPlay(SFX_TAP);
      return;
    }
  }

  if (y >= CLOCK_OK_Y && y <= CLOCK_OK_Y + 44 && x >= 133 && x <= 333) {
    sfxPlay(SFX_TAP);
    applyClock();
    return;
  }
}

// llama + numero de racha arriba a la izquierda
void drawStreakBadge() {
  if (pet.streak < 1) return;
  int x = 26, y = 16;
  gfx->fillTriangle(x + 8, y, x + 1, y + 17, x + 15, y + 17, UI_BAR_BAD);
  gfx->fillTriangle(x + 8, y + 7, x + 4, y + 17, x + 12, y + 17, UI_BAR_WARN);
  char s[6];
  snprintf(s, sizeof(s), "%u", pet.streak);
  gfx->setTextColor(inkColor());
  uiSetTextSize(2);
  uiSetCursor(x + 22, y + 2);
  gfx->print(s);
}

// banner temporal: medalla nueva o hito de racha
void drawCelebration() {
  const char *l1 = nullptr, *l2 = nullptr;
  char buf[20];
  if (pet.showMedal()) {
    for (int i = 0; i < MED_COUNT; i++)
      if (pet.newMedal & (1 << i)) { l2 = medalName(i); break; }
    l1 = T(S_MEDAL_BANNER);
  } else if (pet.showMilestone()) {
    snprintf(buf, sizeof(buf), T(S_STREAK_DAYS_FMT), pet.streak);
    l1 = T(S_GREAT);
    l2 = buf;
  }
  if (!l1) return;
  gfx->fillRoundRect(73, 150, 320, 96, 16, UI_BAR_WARN);
  gfx->drawRoundRect(73, 150, 320, 96, 16, UI_INK);
  gfx->setTextColor(UI_INK);
  uiSetTextSize(3);
  uiSetCursor(CX - uiTextHalfWidth(l1, 3), 176);
  gfx->print(l1);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(l2, 2), 212);
  gfx->print(l2);
}

// medallas en la ficha: badge con etiqueta, color si conseguida
void drawMedalBadge(int x, int y, int i) {
  bool got = pet.hasMedal(1 << i);
  gfx->fillRoundRect(x, y, 100, 24, 6, got ? UI_BAR_OK : UI_TRACK);
  if (!got) gfx->drawRoundRect(x, y, 100, 24, 6, UI_TRACK);
  gfx->setTextColor(got ? UI_BG_DAY : 0x9492);
  uiSetTextSize(2);
  uiSetCursor(x + (100 - uiTextWidth(medalLabel(i), 2)) / 2, y + 5);
  gfx->print(medalLabel(i));
}

// pagina 0: perfil (retrato grande, identidad, racha, vinculo, baya)
void renderCardProfile() {
  const char *nm = pet.nick[0] ? pet.nick : creatureName(pet.speciesId);
  char head[48];
  snprintf(head, sizeof(head), T(S_NAME_FMT), pet.shiny ? "*" : "", nm, pet.level());
  gfx->setTextColor(RGB565_BLACK);
  // Names on the light profile panel use one neutral color for maximum readability.
  // auto-encoge: a tamano 3 los nombres largos no caben en la franja estrecha de
  // arriba de la pantalla redonda, asi que se cortaban por el borde
  int hlen = (int)utf8GlyphCount(head);
  int hts = (hlen <= 11) ? 3 : 2;
  uiSetTextSize(hts);
  uiSetCursor(CX - uiTextHalfWidth(head, hts), hts == 3 ? 34 : 40);
  gfx->print(head);
  if (pet.nick[0]) {  // especie real bajo el apodo
    gfx->setTextColor(RGB565_BLACK);
    uiSetTextSize(2);
    const char *species = creatureName(pet.speciesId);
    uiSetCursor(CX - uiTextHalfWidth(species, 2) - uiTextHalfWidth("()", 2), 64);
    gfx->printf("(%s)", species);
  }

  // Profile/affection always shows a calm idle portrait. Digimon use frame 0
  // instead of inheriting the home-screen walk, joy, eating or discomfort state.
  if (pet.currentIsDigimon())
    drawDigiFrameCentered(digimonIndex(pet.speciesId),0,CX,220,2,false,false);
  else if (pmd.loaded)
    drawPmdAct(PMD_IDLE, CX, 206, millis(), true, false, 4);

  // racha con llama
  int sx = 138, sy = 224;
  gfx->fillTriangle(sx + 8, sy, sx + 1, sy + 18, sx + 15, sy + 18, UI_BAR_BAD);
  gfx->fillTriangle(sx + 8, sy + 7, sx + 4, sy + 18, sx + 12, sy + 18, UI_BAR_WARN);
  char rl[64];
  snprintf(rl, sizeof(rl), T(S_STREAK_FMT), pet.streak, pet.bestStreak);
  gfx->setTextColor(UI_INK);
  uiSetTextSize(2);
  uiSetCursor(sx + 24, sy + 2);
  gfx->print(rl);

  drawCardStat(258, T(S_VIN), pet.bond, 100, C565(0xd4, 0x52, 0x7e), IV_NONE);

  const char *berry = !pet.berryKnown ? T(S_BERRY_UNK)
                      : pet.lovesBerry(0) ? T(S_BERRY_RED)
                      : pet.lovesBerry(1) ? T(S_BERRY_BLUE)
                                          : T(S_BERRY_GREEN);
  char info[40];
  snprintf(info, sizeof(info), T(S_INFO_FMT), berry,
           (unsigned long)(pet.ageMinutes / 1440));
  gfx->setTextColor(UI_INK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(info, 2), 296);
  gfx->print(info);

  uint8_t pers = personalityIdFor(pet.speciesId, pet.ivAtk, pet.ivDef, pet.ivSpe, pet.ivHp);
  char ps[64]; snprintf(ps, sizeof(ps), "성격: %s · %s", personalityNameKo(pers), personalityEffectKo(pers));
  gfx->setTextColor(UI_BAR_BAD);
  uiDrawCenteredFit(ps, CX, 318, 410, 2, 1);

  gfx->setTextColor(UI_TRACK); uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(T(S_RENAME_HINT), 2), 344);
  gfx->print(T(S_RENAME_HINT));
}

// pagina 1: combate (4 barras + boton de entrenar)
void renderCardStats() {
  gfx->setTextColor(UI_INK);
  uiSetTextSize(3);
  uiSetCursor(CX - uiTextHalfWidth(T(S_BATTLE), 3), 44);
  gfx->print(T(S_BATTLE));

  // typing, in the accent colour of the species (English in every language,
  // same as the species names themselves)
  uint8_t t1=creatureType1(pet.speciesId),t2=creatureType2(pet.speciesId);
  char ty[24];
  if (t2 == T_NONE) snprintf(ty, sizeof(ty), "%s", localizedTypeName(t1));
  else snprintf(ty, sizeof(ty), "%s/%s", localizedTypeName(t1), localizedTypeName(t2));
  gfx->setTextColor(creatureAccent(pet.speciesId));
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(ty, 2), 76);
  gfx->print(ty);

  // 360 de tope de barra: a nivel 73 (fin de ciclo) el stat mas alto de toda
  // la dex es la vitalidad de CHANSEY (355). El 260 anterior ya se desbordaba.
  drawCardStat(104, T(S_STAT_ATK), pet.atkStat(), 360, UI_BAR_BAD, pet.ivAtk);
  drawCardStat(144, T(S_STAT_DEF), pet.defStat(), 360, 0x4C98, pet.ivDef);
  drawCardStat(184, T(S_STAT_SPE), pet.speStat(), 360, UI_BAR_WARN, pet.ivSpe);
  drawCardStat(224, T(S_STAT_VIT), pet.vitStat(), 360, UI_BAR_OK, pet.ivHp);
  drawCardStat(264, T(S_STAT_WGT), pet.weight, 100, 0xB3C8, IV_NONE);

  if (pet.wantEvolveButton()) {
    gfx->fillRoundRect(CARD_EVO_X, CARD_EVO_Y, CARD_EVO_W, CARD_EVO_H, 12, UI_BAR_BAD);
    gfx->drawRoundRect(CARD_EVO_X, CARD_EVO_Y, CARD_EVO_W, CARD_EVO_H, 12, UI_INK);
    gfx->setTextColor(UI_WHITE);
    uiDrawCenteredFit("진화하기", CX, CARD_EVO_Y + 12, CARD_EVO_W - 24, 3, 2);
  } else if (pet.canEvolveNow()) {
    gfx->setTextColor(UI_TRACK);
    uiDrawCenteredFit("다음 레벨에서 다시 선택할 수 있어요", CX, CARD_EVO_Y + 14,
                      CARD_EVO_W, 2, 1);
  }

}

// Draws one move as a row: name, its type in the type's own colour, and either
// power or a STATUS marker. Shared by the moves page and the picker so a move
// looks the same wherever you meet it.
// A filled chip in the type's own colour, label in whichever of black/white
// reads on it. Returns its width so a caller can lay out beside it.
int drawTypeChip(int x, int y, uint8_t type) {
  const char *nm = localizedTypeName(type);
  int w = uiTextWidth(nm, 1) + 10;
  gfx->fillRoundRect(x, y, w, 15, 4, typeColor(type));
  uiSetTextSize(1);
  gfx->setTextColor(typeColorIsLight(type) ? UI_INK : UI_WHITE);
  uiSetCursor(x + 5, y + 4);
  gfx->print(nm);
  return w;
}

void drawMoveRow(int y, uint8_t mv, bool highlight, int16_t dex) {
  gfx->fillRoundRect(70, y, 326, 50, 12, highlight ? UI_BAR_WARN : UI_BG_DAY);
  gfx->drawRoundRect(70, y, 326, 50, 12, UI_INK);
  if (mv >= MOVE_COUNT) mv = 0;  // corrupted/legacy slot: never index outside MOVE_TBL
  if (!mv) {
    gfx->setTextColor(UI_TRACK);
    uiSetTextSize(2);
    uiSetCursor(CX - uiTextHalfWidth(T(S_MOVE_EMPTY), 2), y + 17);
    gfx->print(T(S_MOVE_EMPTY));
    return;
  }
  const MoveEntry &m = MOVE_TBL[mv];
  gfx->setTextColor(UI_INK);
  uiSetTextSize(2);
  uiSetCursor(82, y + 8);
  gfx->print(localizedMoveName(mv));
  // There is no per-type palette (DexEntry.accent is per species), and inventing
  // one by hand would duplicate what gen_dex.py generates. Colouring same-type
  // moves in the species accent is more useful anyway: STAB is a 1.5x damage
  // bonus, so this marks the moves that actually hit hardest for this creature.
  // The chip carries the TYPE; STAB moved onto the power figure, where it
  // belongs -- STAB is a damage bonus, so saying it next to the damage reads
  // straight, and it leaves the type free to be its own colour.
  bool stab = (creatureType1(dex)==m.type||creatureType2(dex)==m.type) && m.cat != MC_STATUS;
  int cw = drawTypeChip(82, y + 29, m.type);
  if (stab) {
    gfx->setTextColor(creatureAccent(dex));
    uiSetTextSize(1);
    uiSetCursor(82 + cw + 6, y + 33);
    gfx->print("STAB");
  }
  char pw[16];
  if (m.cat == MC_STATUS) snprintf(pw, sizeof(pw), "%s", T(S_MOVE_STATUS));
  else snprintf(pw, sizeof(pw), T(S_MOVE_PWR), m.power);
  gfx->setTextColor(stab ? creatureAccent(dex) : UI_INK);
  uiSetTextSize(1);
  uiSetCursor(384 - uiTextWidth(pw, 1), y + 33);
  gfx->print(pw);
}

// card page 4: the four known moves. Tapping a slot opens the picker.
void renderCardMoves() {
  gfx->setTextColor(UI_INK);
  uiSetTextSize(3);
  uiSetCursor(CX - uiTextHalfWidth(T(S_MOVES), 3), 44);
  gfx->print(T(S_MOVES));
  for (int i = 0; i < MOVE_SLOTS; i++) drawMoveRow(MOVE_ROW_Y(i), pet.moves[i], false, pet.speciesId);
  gfx->setTextColor(UI_TRACK);
  uiDrawCenteredFit(T(S_MOVE_TAP), CX, 336, 390, 2, 1);
}

// Every move the species can learn by this level, so a slot can be swapped for
// anything legal -- not just the handful a level-up would have offered.
uint8_t learnableFor(int16_t dex, uint8_t lvl, uint8_t *out, uint8_t max) {
  if (isDigimonId(dex)) return digimonLearnableMoves(digimonIndex(dex), lvl, out, max);
  if (dex < 1 || dex > DEX_COUNT) return 0;
  uint8_t n = learnCount(dex), w = 0;
  if (n == 0 && DEX_NATDEX[dex] > 809)
    return fallbackMovesForDex(dex, lvl, out, max);
  for (uint8_t i = 0; i < n && w < max; i++) {
    // moveUnlockLevel(), NOT learnLevel(): a TM is stored as level 0 and would
    // otherwise clear this check at level 1. That is how a level 22 Charmeleon
    // came to be offered FIRE BLAST -- the same class of bug as the level 1
    // Squirtle with SURF, in the one path that fix did not reach.
    if (moveUnlockLevel(dex, i) > lvl) continue;
    uint8_t mv = learnMove(dex, i);
    if (!mv || mv >= MOVE_COUNT) continue;
    bool dup = false;
    for (uint8_t j = 0; j < w; j++)
      if (out[j] == mv) { dup = true; break; }
    if (!dup) out[w++] = mv;
  }
  return w;
}

// The picker targets either the live pet or a banked member. A banked one keeps
// its frozen level, so it can only relearn what it could have known back then.
uint8_t learnableList(uint8_t *out, uint8_t max) {
  if (movePickParty) {
    const PartyMon &m = party.slots[movePickParty - 1];
    return learnableFor(m.dex, (uint8_t)m.level, out, max);
  }
  return pet.isEgg() ? 0 : learnableFor(pet.speciesId, pet.level(), out, max);
}

uint8_t *pickTargetMoves() {
  return movePickParty ? party.slots[movePickParty - 1].moves : pet.moves;
}
int16_t pickTargetDex() {
  return movePickParty ? party.slots[movePickParty - 1].dex : pet.speciesId;
}

void renderMovePick() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(T(S_MOVE_PICK), 2), 40);
  gfx->print(T(S_MOVE_PICK));

  uint8_t all[64];
  uint8_t n = learnableList(all, sizeof(all));
  uint8_t pages = n ? (n + MOVE_PICK_PER_PAGE - 1) / MOVE_PICK_PER_PAGE : 1;
  if (movePickPage >= pages) movePickPage = 0;
  for (uint8_t i = 0; i < MOVE_PICK_PER_PAGE; i++) {
    uint8_t idx = movePickPage * MOVE_PICK_PER_PAGE + i;
    if (idx >= n) break;
    // the move already in this slot is highlighted, so replacing like for like
    // is obvious rather than a guess
    drawMoveRow(MOVE_PICK_Y(i), all[idx], all[idx] == pickTargetMoves()[movePickSlot], pickTargetDex());
  }
  for (uint8_t i = 0; i < pages && pages > 1; i++) {
    if (i == movePickPage) gfx->fillCircle(CX - (pages - 1) * 13 + i * 26, 380, 5, UI_INK);
    else gfx->drawCircle(CX - (pages - 1) * 13 + i * 26, 380, 4, UI_INK);
  }
  gfx->setTextColor(UI_TRACK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(T(S_BACK), 2), 402);
  gfx->print(T(S_BACK));
  gfx->flush();
}

// ---------- battle ----------

// Draws a battle backdrop scaled 2x. Emitted as runs of identical indices
// rather than a write per pixel: this is flat pixel art with long horizontal
// runs, and 240x112 at 2x would otherwise be 26,880 fillRect calls a frame.
// The round bezel crops the overhang physically, so nothing is clipped here.
static void drawBack(const BackScene &b, int y0) {
  const int SC = 2;
  int x0 = CX - (b.w * SC) / 2;
  for (int r = 0; r < b.h; r++) {
    const uint8_t *row = b.idx + (uint32_t)r * b.w;
    int c = 0;
    while (c < b.w) {
      uint8_t v = row[c];
      int run = 1;
      while (c + run < b.w && row[c + run] == v) run++;
      uint16_t col = b.pal[v];
      canvasFillRectFast(x0 + c * SC, y0 + r * SC, run * SC, SC, col);
      c += run;
    }
  }
}

// Which scene: the FOE's biome, since a battle happens where it lives, and the
// same day/night split the main screen already uses.
static void drawBattleBack() {
  int16_t dex = btlFoe.dex;
  if (!isCreatureId(dex)) { gfx->fillCircle(CX, CY, 231, UI_BG_DAY); return; }
  uint8_t bi = isDigimonId(dex)?0:DEX_TBL[dex].biome;
  if (bi >= BACK_BIOMES) bi = 0;
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  drawBack(BACKS[bi][night ? 1 : 0], 30);
}

// Streams a side's sprite if it is not already the one loaded. Called whenever
// a creature steps in, never per frame.
static void btlSyncSprite(uint8_t who, const Combatant &c) {
  int16_t key = c.dex * (c.shiny ? -1 : 1);
  if (btlPmdDex[who] == key && btlPmd[who].loaded) return;
  btlPmd[who].unload();
  btlPmdDex[who] = 0;
  if (c.dex < 1 || c.dex > DEX_COUNT) return;
  if (btlPmd[who].load(c.dex, c.shiny)) btlPmdDex[who] = key;   // NOT (uint8_t): Hoenn runs past 255
}

static void btlFreeSprites() {
  for (int i = 0; i < 2; i++) { btlPmd[i].unload(); btlPmdDex[i] = 0; btlDigiFor[i] = -1; }
}

static void btlSay(const char *fmt, ...) {
  if (btlMsgCount >= 6) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(btlMsg[btlMsgCount], sizeof(btlMsg[0]), fmt, ap);
  va_end(ap);
  btlMsgCount++;
}

// Combatant.name remains the original short ASCII identifier (or a nickname)
// because it is also used by the battle/link layer. Localize only the canonical
// species case at draw/narration time; nicknames continue to display verbatim.
static const char *btlDisplayName(const Combatant &c) {
  if (isDigimonId(c.dex)) return c.name[0]?c.name:creatureName(c.dex);
  if (c.dex < 1 || c.dex > DEX_COUNT) return c.name;
  char canonical[sizeof(c.name)];
  snprintf(canonical, sizeof(canonical), "%s", DEX_TBL[c.dex].name);
  if (c.name[0] && strcmp(c.name, canonical) != 0) return c.name;
  return creatureName(c.dex);
}

// Turns a TurnLog into narration. Everything here was already decided by the
// engine -- nothing is recomputed, so the text can never disagree with the maths.
// Picks the cue for an action from the TurnLog, so the sound can never
// disagree with what actually happened.
static void btlSfxFor(const TurnLog &lg) {
  if (lg.targetFainted) { sfxPlay(SFX_FAINT); return; }
  if (lg.inflicted) { sfxPlay(SFX_STATUS); return; }
  if (lg.damage && lg.effPct > 100) { sfxPlay(SFX_SUPER); return; }
  if (lg.damage) {
    sfxPlay(lg.move && lg.move < MOVE_COUNT && MOVE_TBL[lg.move].cat == MC_SPEC ? SFX_BEAM : SFX_HIT);
    return;
  }
  if (lg.move && lg.move < MOVE_COUNT && MOVE_TBL[lg.move].cat == MC_STATUS && !lg.missed) sfxPlay(SFX_STATUS);
}

static void btlNarrate(const Combatant &actor, const Combatant &target, const TurnLog &lg) {
  if (lg.skipped) return;
  btlSfxFor(lg);
  if (lg.hurtSelf) { btlSay(T(S_BTL_HURTSELF)); return; }
  if (lg.charged) { btlSay(T(S_BTL_USED), btlDisplayName(actor), localizedMoveName(lg.move)); return; }
  if (lg.move) btlSay(T(S_BTL_USED), btlDisplayName(actor), localizedMoveName(lg.move));
  if (lg.missed) { btlSay(T(S_BTL_MISS), btlDisplayName(actor)); return; }
  if (lg.immune) { btlSay(T(S_BTL_IMMUNE)); return; }
  if (lg.crit) btlSay(T(S_BTL_CRIT));
  if (lg.stoleStages) btlSay("상대의 능력 상승을 빼앗았다!");
  if (lg.stageDelta > 0) btlSay("능력이 올랐다!");
  else if (lg.stageDelta < 0) btlSay("상대의 능력이 떨어졌다!");
  if (lg.healed) btlSay("HP를 회복했다!");
  if (lg.damage && lg.effPct > 100) btlSay(T(S_BTL_SUPER));
  else if (lg.damage && lg.effPct < 100) btlSay(T(S_BTL_WEAK));
  if (lg.inflicted) {
    static const StrId AIL_STR[] = { S_AIL_PARA, S_AIL_PARA, S_AIL_BURN, S_AIL_POISON,
                                     S_AIL_SLEEP, S_AIL_FREEZE, S_AIL_CONFUSE };
    if (lg.inflicted < 7) btlSay(T(S_BTL_STATUS), btlDisplayName(target), T(AIL_STR[lg.inflicted]));
  }
  if (lg.targetFainted) btlSay(T(S_BTL_FAINT), btlDisplayName(target));
}

// Builds one opponent through Pet, so it gets the same stat formula and the
// same learnset-driven moveset the player's creatures do.
static int16_t trainerDexWithArt(int16_t dex, uint8_t type) {
  if (dex >= 1 && dex <= DEX_COUNT && speciesHasArt(dex)) return dex;
  // A modern roster may reference a Pokemon whose PMDCollab behavior sheet is
  // temporarily missing. Keep the battle playable and animated with a stable
  // same-type classic fallback instead of showing a bare number.
  static const int16_t FALLBACK[TYPE_COUNT] = {
    143, 59, 9, 26, 3, 131, 68, 89, 112, 18, 65, 127, 76, 94, 149, 229, 208, 35
  };
  return type < TYPE_COUNT ? FALLBACK[type] : 143;
}

static void foeFromSpecies(Combatant &c, int16_t dex, uint8_t lvl, uint8_t iv) {
  Pet foe;
  foe.dbgHatchAs(dex, false);
  foe.ivAtk = foe.ivDef = foe.ivSpe = foe.ivHp = iv;
  foe.ageMinutes = (uint32_t)(lvl ? lvl - 1 : 0) * MINUTES_PER_LEVEL;
  foe.levelMinutes = foe.ageMinutes;
  foe.relearnFromLevel();
  combatantFromPet(c, foe);
}


static void btlSetCurrentWeather() {
  btlWeather = extras.weatherId(pet);
}

static void btlApplyWeatherToPlayerSquad() {
  for (uint8_t i = 0; i < btlSquadN; i++) extras.applyWeatherToCombatant(btlSquad[i], btlWeather);
  if (btlSquadN) btlYou = btlSquad[btlSquadAt];
}

static uint16_t pctStat(uint16_t v, uint16_t pct) {
  uint32_t n = (uint32_t)v * pct / 100UL;
  return (uint16_t)(n > 65535UL ? 65535UL : n);
}

static const char *bossPhaseEffectKo(uint8_t type) {
  static const char *const E[TYPE_COUNT] = {
    "공격·속도 상승", "폭주: 공격 크게 상승", "재생: HP 회복·방어 상승", "과충전: 속도·공격 상승",
    "광합성: HP 회복·방어 상승", "빙벽: 방어 크게 상승", "투지: 공격 크게 상승", "독안개: 독 + 공격 상승",
    "대지갑옷: 방어·공격 상승", "질풍: 속도·공격 상승", "초능력 폭주: 공격·방어 상승", "군체: 공격·속도 상승",
    "요새화: 방어 크게 상승", "원혼: 혼란 + 공격 상승", "용의분노: 모든 능력 상승", "암습: 공격 크게 상승",
    "철벽: 방어 크게 상승", "요정의빛: HP 회복·공격 상승"
  };
  return type < TYPE_COUNT ? E[type] : "능력 상승";
}

static void triggerBossPhase2() {
  if (!btlBoss || btlBossPhase2 || btlFoe.fainted() || btlFoe.hp * 2U > btlFoe.maxHp) return;
  btlBossPhase2 = true;
  switch (btlBossType) {
    case T_NORMAL: btlFoe.base[SI_ATK]=pctStat(btlFoe.base[SI_ATK],120); btlFoe.base[SI_SPE]=pctStat(btlFoe.base[SI_SPE],120); break;
    case T_FIRE: btlFoe.base[SI_ATK]=pctStat(btlFoe.base[SI_ATK],130); break;
    case T_WATER: btlFoe.hp=min<uint16_t>(btlFoe.maxHp,(uint16_t)(btlFoe.hp+btlFoe.maxHp/7)); btlFoe.base[SI_DEF]=pctStat(btlFoe.base[SI_DEF],122); break;
    case T_ELECTRIC: btlFoe.base[SI_SPE]=pctStat(btlFoe.base[SI_SPE],135); btlFoe.base[SI_ATK]=pctStat(btlFoe.base[SI_ATK],118); break;
    case T_GRASS: btlFoe.hp=min<uint16_t>(btlFoe.maxHp,(uint16_t)(btlFoe.hp+btlFoe.maxHp/6)); btlFoe.base[SI_DEF]=pctStat(btlFoe.base[SI_DEF],118); break;
    case T_ICE: btlFoe.base[SI_DEF]=pctStat(btlFoe.base[SI_DEF],130); break;
    case T_FIGHTING: btlFoe.base[SI_ATK]=pctStat(btlFoe.base[SI_ATK],135); btlFoe.base[SI_DEF]=pctStat(btlFoe.base[SI_DEF],110); break;
    case T_POISON: if(btlYou.ailment==AIL_NONE)btlYou.ailment=AIL_POISON; btlFoe.base[SI_ATK]=pctStat(btlFoe.base[SI_ATK],125); break;
    case T_GROUND: btlFoe.base[SI_DEF]=pctStat(btlFoe.base[SI_DEF],128); btlFoe.base[SI_ATK]=pctStat(btlFoe.base[SI_ATK],118); break;
    case T_FLYING: btlFoe.base[SI_SPE]=pctStat(btlFoe.base[SI_SPE],135); btlFoe.base[SI_ATK]=pctStat(btlFoe.base[SI_ATK],112); break;
    case T_PSYCHIC: btlFoe.base[SI_ATK]=pctStat(btlFoe.base[SI_ATK],130); btlFoe.base[SI_DEF]=pctStat(btlFoe.base[SI_DEF],125); break;
    case T_BUG: btlFoe.base[SI_ATK]=pctStat(btlFoe.base[SI_ATK],125); btlFoe.base[SI_SPE]=pctStat(btlFoe.base[SI_SPE],125); break;
    case T_ROCK: btlFoe.base[SI_DEF]=pctStat(btlFoe.base[SI_DEF],140); break;
    case T_GHOST: btlYou.confuseTurns=max<uint8_t>(btlYou.confuseTurns,3); btlFoe.base[SI_ATK]=pctStat(btlFoe.base[SI_ATK],125); break;
    case T_DRAGON: btlFoe.base[SI_ATK]=pctStat(btlFoe.base[SI_ATK],115); btlFoe.base[SI_DEF]=pctStat(btlFoe.base[SI_DEF],115); btlFoe.base[SI_SPE]=pctStat(btlFoe.base[SI_SPE],115); break;
    case T_DARK: btlFoe.base[SI_ATK]=pctStat(btlFoe.base[SI_ATK],125); break;
    case T_STEEL: btlFoe.base[SI_DEF]=pctStat(btlFoe.base[SI_DEF],135); break;
    case T_FAIRY: btlFoe.hp=min<uint16_t>(btlFoe.maxHp,(uint16_t)(btlFoe.hp+btlFoe.maxHp/8)); btlFoe.base[SI_ATK]=pctStat(btlFoe.base[SI_ATK],122); btlFoe.base[SI_DEF]=pctStat(btlFoe.base[SI_DEF],118); break;
  }
  btlFoe.base[SI_SPA] = btlFoe.base[SI_ATK];
  btlFoe.base[SI_SPD] = btlFoe.base[SI_DEF];
  btlHpShown[1] = btlFoe.hp;
  btlSay("보스 2페이즈!");
  btlSay("%s", bossPhaseEffectKo(btlBossType));
  sfxPlay(SFX_SUPER);
}

// Your side: the live pet first, then the banked party.
//
// Both ladders cap your LEVEL to the leader's best, so a gym is always fought
// on its own terms and grinding is never the answer -- the type chart, the
// movesets and the choices are. Hard additionally caps your team SIZE to the
// leader's, so Brock is two-on-two. The caps are applied while BUILDING the
// combatants, so nothing is ever written back to the stored creature, exactly
// like ailments.
static void buildSquad(uint8_t maxLvl, uint8_t maxCount, uint32_t mask) {
  btlSquadN = 0;
  btlSquadAt = 0;
  btlPetIn = false;
  if (maxCount > TRAINER_TEAM_MAX) maxCount = TRAINER_TEAM_MAX;
  if (!pet.isEgg() && creatureHasArt(pet.speciesId) && btlSquadN < maxCount && (mask & 1UL)) {
    Pet tmp = pet;                       // a copy: the real pet is untouched
    if (maxLvl && tmp.level() > maxLvl) {
      tmp.ageMinutes = (uint32_t)(maxLvl - 1) * MINUTES_PER_LEVEL;
      tmp.levelMinutes = tmp.ageMinutes;
    }
    combatantFromPet(btlSquad[btlSquadN], tmp);
    btlSquadN++;
    btlPetIn = true;      // the training reward goes to whoever fought for it
  }
  for (int i = 0; i < PARTY_SLOTS && btlSquadN < maxCount; i++) {
    if (party.slots[i].empty() || !speciesHasArt(party.slots[i].dex) || !(mask & (1UL << (i + 1)))) continue;
    PartyMon m = party.slots[i];
    if (maxLvl && m.level > maxLvl) m.level = maxLvl;
    combatantFromParty(btlSquad[btlSquadN++], m);
  }
  for (int i = 0; i < BOX_SLOTS && btlSquadN < maxCount; i++) {
    uint8_t bit = (uint8_t)(1 + PARTY_SLOTS + i);
    if (party.box[i].empty() || !speciesHasArt(party.box[i].dex) || !(mask & (1UL << bit))) continue;
    PartyMon m = party.box[i];
    if (maxLvl && m.level > maxLvl) m.level = maxLvl;
    combatantFromParty(btlSquad[btlSquadN++], m);
  }
  if (btlSquadN) btlYou = btlSquad[0];
}

// How many you may bring: the leader's own count in hard mode, six otherwise.
static uint8_t squadCapForRegion(uint8_t region, uint8_t idx, bool hard) {
  if (idx >= TRAINER_COUNT) return TRAINER_TEAM_MAX;
  const Trainer &t = TRAINER_SETS[region % GYM_REGIONS].list[idx];
  return hard ? t.count : TRAINER_TEAM_MAX;
}

uint8_t squadCap(uint8_t idx, bool hard) {
  if (idx >= TRAINER_COUNT) return TRAINER_TEAM_MAX;
  return squadCapForRegion(gymRegion, idx, hard);
}

// A fight against another device. The squads are already exchanged; the host
// owns resolution and the guest renders what it is sent.
void startLinkBattle() {
  btlBoss = false;
  btlRival = false;
  btlBossPhase2 = false;
  btlRivalSpecial = RIVSPEC_NONE;
  if (!lan.mineN || !lan.theirsN) return;
  // Rebuilt from lan.mine, NOT from squadMask. What we fight with has to be
  // exactly what the peer was told we have -- rebuilding from the party would
  // silently diverge if anything changed between offering and starting.
  btlSquadN = 0;
  btlSquadAt = 0;
  for (uint8_t i = 0; i < lan.mineN && i < TRAINER_TEAM_MAX; i++)
    linkMonTo(btlSquad[btlSquadN++], lan.mine[i]);
  if (!btlSquadN) return;
  btlYou = btlSquad[0];
  btlLink = true;
  btlLinkHost = lan.isHost;
  btlTrainer = -1;
  btlHard = false;
  btlFoeAt = 0;
  btlFoeSquadN = 0;
  for (uint8_t i = 0; i < lan.theirsN && i < TRAINER_TEAM_MAX; i++)
    linkMonTo(btlFoeSquad[btlFoeSquadN++], lan.theirs[i]);
  btlFoe = btlFoeSquad[0];
  btlMyAct = 0;
  btlMsgCount = 0;
  btlOver = false;
  btlWon = false;
  btlMenu = 0;
  btlWinUntil = 0;
  btlSwapWho = -1;
  btlFaintUntil[0] = btlFaintUntil[1] = 0;
  btlEnterUntil[0] = btlEnterUntil[1] = 0;
  btlHpShown[0] = btlYou.maxHp;
  btlHpShown[1] = btlFoe.maxHp;
  btlSyncSprite(0, btlYou);
  btlSyncSprite(1, btlFoe);
  audioMusic(MUS_BATTLE);
  battleOpen = true;
}

void startTrainerBattle(uint8_t idx, bool hard) {
  btlTower = false;
  btlBoss = false;
  btlRival = false;
  btlBossPhase2 = false;
  btlRivalSpecial = RIVSPEC_NONE;
  if (idx >= TRAINER_COUNT || pet.ceremony != CER_NONE) return;
  // The region is part of the battle identity. Freeze it BEFORE any roster is
  // read so the first foe, later replacements, win screen and badge all use
  // exactly the same ladder even after the gym UI is closed.
  btlRegion = pickRegion % GYM_REGIONS;
  const Trainer &tr = BTL_TRAINERS[idx];
  uint8_t top = 0;
  for (int k = 0; k < tr.count; k++)
    if (tr.team[k].level > top) top = tr.team[k].level;
  // BOTH ladders cap your level to the leader's best. Without it a L73 team
  // walks every trainer at 100% and the type chart never matters. Hard adds the
  // size cap on top, plus a smarter AI and better opposing IVs.
  buildSquad(top, hard ? tr.count : TRAINER_TEAM_MAX, squadMask);
  if (!btlSquadN) return;
  btlSetCurrentWeather();
  btlApplyWeatherToPlayerSquad();
  btlTrainer = (int8_t)idx;
  btlHard = hard;
  btlFoeAt = 0;
  const Trainer &t = BTL_TRAINERS[idx];
  foeFromSpecies(btlFoe, trainerDexWithArt(t.team[0].dex, t.type), t.team[0].level, hard ? HARD_IV : EASY_IV);
  extras.applyWeatherToCombatant(btlFoe, btlWeather);
  btlMsgCount = 0;
  btlOver = false;
  btlWon = false;
  btlMenu = 0;
  btlWinUntil = 0;
  btlSwapWho = -1;
  btlFaintUntil[0] = btlFaintUntil[1] = 0;
  btlEnterUntil[0] = btlEnterUntil[1] = 0;
  btlHpShown[0] = btlYou.maxHp;
  btlHpShown[1] = btlFoe.maxHp;
  btlSyncSprite(0, btlYou);
  btlSyncSprite(1, btlFoe);
  audioMusic(MUS_BATTLE);
  btlLungeUntil[0] = btlLungeUntil[1] = 0;
  btlHitUntil[0] = btlHitUntil[1] = 0;
  battleOpen = true;
}

void startBattle(int16_t dex, uint8_t lvl) {
  btlTower = false;
  btlBoss = false;
  btlRival = false;
  btlBossPhase2 = false;
  btlRivalSpecial = RIVSPEC_NONE;
  btlTrainer = -1;
  if (pet.isEgg() || pet.ceremony != CER_NONE) return;
  if (dex < 1 || dex > DEX_COUNT) return;
  buildSquad(0, TRAINER_TEAM_MAX, 0xFFFF);
  if (!btlSquadN) return;
  btlSetCurrentWeather();
  btlApplyWeatherToPlayerSquad();
  // The opponent is built through Pet so it gets the same stat formula and the
  // same learnset-driven moveset the player's creature does -- no special-cased
  // "enemy" maths that could quietly diverge.
  Pet foe;
  foe.dbgHatchAs(dex, false);
  foe.ivAtk = foe.ivDef = foe.ivSpe = foe.ivHp = 20;
  foe.ageMinutes = (uint32_t)(lvl ? lvl - 1 : 0) * MINUTES_PER_LEVEL;
  foe.levelMinutes = foe.ageMinutes;
  foe.relearnFromLevel();
  combatantFromPet(btlFoe, foe);
  extras.applyWeatherToCombatant(btlFoe, btlWeather);
  btlMsgCount = 0;
  btlOver = false;
  btlWon = false;
  btlMenu = 0;
  btlWinUntil = 0;
  btlSwapWho = -1;
  btlFaintUntil[0] = btlFaintUntil[1] = 0;
  btlEnterUntil[0] = btlEnterUntil[1] = 0;
  btlHpShown[0] = btlYou.maxHp;
  btlHpShown[1] = btlFoe.maxHp;
  btlSyncSprite(0, btlYou);
  btlSyncSprite(1, btlFoe);
  audioMusic(MUS_BATTLE);
  btlLungeUntil[0] = btlLungeUntil[1] = 0;
  btlHitUntil[0] = btlHitUntil[1] = 0;
  battleOpen = true;
}

// The guest's whole turn: copy in what the host resolved and play the same
// animations the host is playing. It runs no battle logic at all -- that is the
// entire point of one side being authoritative (see link.h).
static void btlApplyResult() {
  if (lan.resultN < sizeof(LinkResult)) { lan.resultNew = false; return; }
  LinkResult r;
  memcpy(&r, lan.result, sizeof(r));
  lan.resultNew = false;

  // The wire says "host"/"guest"; here we are always the guest, so their fields
  // are the foe's and ours are ours.
  uint32_t now = millis();
  if (r.guestIdx < btlSquadN && r.guestIdx != btlSquadAt) {
    btlSquad[btlSquadAt] = btlYou;
    btlSquadAt = r.guestIdx;
    btlYou = btlSquad[btlSquadAt];
    btlSyncSprite(0, btlYou);
    btlLungeUntil[0] = btlHitUntil[0] = btlFaintUntil[0] = 0;
    btlEnterUntil[0] = now + BTL_ENTER_MS;
  }
  if (r.hostIdx != btlFoeAt && r.hostIdx < lan.theirsN) {
    btlFoeAt = r.hostIdx;
    linkMonTo(btlFoe, lan.theirs[btlFoeAt]);
    btlHpShown[1] = btlFoe.maxHp;
    btlSyncSprite(1, btlFoe);
    btlLungeUntil[1] = btlHitUntil[1] = btlFaintUntil[1] = 0;
    btlEnterUntil[1] = now + BTL_ENTER_MS;
  }
  btlYou.hp = r.guestHp > btlYou.maxHp ? btlYou.maxHp : r.guestHp;
  btlFoe.hp = r.hostHp > btlFoe.maxHp ? btlFoe.maxHp : r.hostHp;
  btlYou.ailment = r.guestAil;
  btlFoe.ailment = r.hostAil;

  btlMsgCount = 0;
  if (r.hostMove) btlSay(T(S_BTL_USED), btlDisplayName(btlFoe), localizedMoveName(r.hostMove));
  if (r.guestMove) btlSay(T(S_BTL_USED), btlDisplayName(btlYou), localizedMoveName(r.guestMove));
  if (r.guestDmg) { btlHitUntil[0] = now + BTL_HIT_MS; sfxPlay(SFX_HIT); }
  if (r.hostDmg) { btlHitUntil[1] = now + BTL_HIT_MS; sfxPlay(SFX_HIT); }
  if (btlYou.fainted()) {
    btlFaintUntil[0] = now + BTL_FAINT_MS;
    btlSay(T(S_BTL_FAINT), btlDisplayName(btlYou));
  }
  if (btlFoe.fainted()) {
    btlFaintUntil[1] = now + BTL_FAINT_MS;
    btlSay(T(S_BTL_FAINT), btlDisplayName(btlFoe));
  }
}

// Radio packets land on another task, so the guest picks them up here, once a
// frame, rather than rendering from inside an interrupt.
static void btlLinkPoll() {
  if (!btlLink) return;

  // A peer that stopped answering. Ending the fight is the only honest thing to
  // do -- there is no result coming, and pretending otherwise is the hang this
  // whole layer exists to remove.
  if (!lan.live() && !btlOver) {
    btlOver = true;
    btlWon = false;
    audioMusic(MUS_NONE);
    btlMsgCount = 0;
    btlSay("%s", T(S_LAN_GONE));
    return;
  }

  if (btlLinkHost) {
    // Our own action was latched when it was tapped; theirs arrives whenever
    // the radio manages it. Whichever is second sets the turn going.
    if (btlMyAct && lan.hasPeerAct() && !btlOver && !btlMsgCount &&
        btlSwapWho < 0) {
      uint8_t act = btlMyAct;
      btlMyAct = 0;
      if (LINK_ACT_IS_SWITCH(act)) btlSwitchTo(LINK_ACT_SLOT(act));
      else btlResolve(btlYou.moves[LINK_ACT_SLOT(act) % MOVE_SLOTS]);
    }
    return;
  }

  if (lan.resultNew) btlApplyResult();
  if (lan.state == LINK_DONE && !btlOver) {
    btlOver = true;
    btlWon = lan.youWon;
    audioMusic(btlWon ? MUS_VICTORY : MUS_NONE);
    if (btlWon) sfxPlay(SFX_VICTORY);
    btlSay("%s", btlWon ? T(S_BTL_WIN) : T(S_BTL_LOSE));
  }
}

// Packs the outcome for the guest. Only the host ever calls this.
static void btlShipResult(uint8_t yourMove, uint8_t theirMove,
                          uint16_t hp0You, uint16_t hp0Foe) {
  LinkResult r = {};
  r.hostHp = btlYou.hp;   r.guestHp = btlFoe.hp;
  r.hostAil = btlYou.ailment; r.guestAil = btlFoe.ailment;
  r.hostMove = yourMove;  r.guestMove = theirMove;
  r.hostDmg = (hp0You > btlYou.hp) ? hp0You - btlYou.hp : 0;
  r.guestDmg = (hp0Foe > btlFoe.hp) ? hp0Foe - btlFoe.hp : 0;
  r.hostIdx = btlSquadAt; r.guestIdx = btlFoeAt;
  if (btlYou.fainted() || btlFoe.fainted()) r.flags |= 0x04;
  lan.sendResult((const uint8_t *)&r, (uint8_t)sizeof(r));
}

// One exchange: both sides act in speed order, then burn/poison chip.
static void btlResolve(uint8_t yourMove) {
  TurnLog lg;
  // Against another device the opponent's move comes off the wire, never from
  // the AI -- and the host is the only side that runs this at all.
  uint8_t foeMove;
  bool foeSwitched = false;
  uint32_t now = millis();
  if (btlLink) {
    // Off the wire, never from the AI. A switch is carried in the same message
    // as a move, and like our own switch it costs the turn: they change, we act.
    uint8_t act = lan.pendingAct;
    if (LINK_ACT_IS_SWITCH(act)) {
      uint8_t to = LINK_ACT_SLOT(act);
      if (to < btlFoeSquadN && to != btlFoeAt && !btlFoeSquad[to].fainted()) {
        btlFoeSquad[btlFoeAt] = btlFoe;     // remember how battered it was
        btlFoeAt = to;
        btlFoe = btlFoeSquad[to];
        btlHpShown[1] = btlFoe.hp;
        btlSyncSprite(1, btlFoe);
        btlLungeUntil[1] = btlHitUntil[1] = btlFaintUntil[1] = 0;
        btlEnterUntil[1] = now + BTL_ENTER_MS;
        btlSay(T(S_BTL_SENDS), lan.peerName, btlDisplayName(btlFoe));
        foeSwitched = true;
      }
      foeMove = 0;
    } else {
      foeMove = btlFoe.moves[LINK_ACT_SLOT(act) % MOVE_SLOTS];
    }
    lan.pendingAct = 0;
  } else {
    foeMove = aiChooseMove(btlFoe, btlYou, btlHard);
  }
  (void)foeSwitched;

  bool youFirst = battleMovesFirst(btlYou, yourMove, btlFoe, foeMove);
  Combatant *a = youFirst ? &btlYou : &btlFoe;
  Combatant *b = youFirst ? &btlFoe : &btlYou;
  uint8_t ma = youFirst ? yourMove : foeMove;
  uint8_t mb = youFirst ? foeMove : yourMove;

  uint16_t hp0You = btlYou.hp, hp0Foe = btlFoe.hp;
  battleAct(*a, *b, ma, lg);
  btlNarrate(*a, *b, lg);
  if (lg.damage && !lg.hurtSelf) btlLungeUntil[a == &btlYou ? 0 : 1] = now + BTL_LUNGE_MS;
  if (!b->fainted()) {
    battleAct(*b, *a, mb, lg);
    btlNarrate(*b, *a, lg);
    if (lg.damage && !lg.hurtSelf)
      btlLungeUntil[b == &btlYou ? 0 : 1] = now + BTL_LUNGE_MS + BTL_LUNGE_MS;
  }
  // whoever actually lost health flinches, whichever side dealt it
  if (btlYou.hp < hp0You) btlHitUntil[0] = now + BTL_HIT_MS;
  if (btlFoe.hp < hp0Foe) btlHitUntil[1] = now + BTL_HIT_MS;
  if (btlLink && btlLinkHost) btlShipResult(yourMove, foeMove, hp0You, hp0Foe);
  if (!btlYou.fainted() && !btlFoe.fainted()) {
    battleEndTurn(btlYou, lg);
    if (lg.damage) btlNarrate(btlYou, btlYou, lg);
    battleEndTurn(btlFoe, lg);
    if (lg.damage) btlNarrate(btlFoe, btlFoe, lg);
  }
  triggerBossPhase2();
  // Someone went down. The replacement is NOT swapped in here -- that made the
  // change instant and read as a jump cut. Flag it, let the sprite drop out of
  // frame, and swap when the player dismisses the message.
  if (btlFoe.fainted() && btlRival && btlFoeAt + 1 < btlFoeSquadN) {
    btlFaintUntil[1] = millis() + BTL_FAINT_MS;
    btlSwapWho = 1;
    return;
  }
  if (btlFoe.fainted() && btlLink && btlFoeAt + 1 < btlFoeSquadN) {
    btlFaintUntil[1] = millis() + BTL_FAINT_MS;
    btlSwapWho = 1;
    return;
  }
  if (btlFoe.fainted() && btlTrainer >= 0 && btlFoeAt + 1 < BTL_TRAINERS[btlTrainer].count) {
    btlFaintUntil[1] = millis() + BTL_FAINT_MS;
    btlSwapWho = 1;
    return;
  }
  if (btlYou.fainted() && btlSquadAt + 1 < btlSquadN) {
    btlFaintUntil[0] = millis() + BTL_FAINT_MS;
    btlSwapWho = 0;
    return;
  }
  if (btlFoe.fainted() || btlYou.fainted()) {
    btlOver = true;
    btlWon = btlFoe.fainted();
    btlNewBadge = false;
    btlTrainGain = 0;
    if (btlWon && !btlLink) {
      extras.missionAction(MIS_BATTLE, 1, pet);
      if (btlPetIn && !pet.isEgg()) extras.addResearch(pet.speciesId, 1);
    }
    // Persistent rival: no badge or tower streak. A win can award a map
    // fragment, TM or growth item; either result advances the rival's growth.
    if (btlRival) {
      audioMusic(btlWon ? MUS_VICTORY : MUS_NONE);
      btlMsgCount = 0;
      extras.rivalResult(btlWon, pet);
      if (btlWon) {
        sfxPlay(SFX_VICTORY);
        char rw[80]; extraRewardLabel(rw, sizeof(rw), extras.rivalRewardKind(), extras.rivalRewardId(), extras.rivalRewardCount());
        btlSay("라이벌 승리! %s", rw);
        if (btlRivalSpecial == RIVSPEC_TREASURE_RACE) btlSay("보물 경쟁 승리! 지도 조각 +1");
      } else {
        btlSay("라이벌 %s에게 패배했다.", extras.rivalNameKo());
        if (btlRivalSpecial == RIVSPEC_TREASURE_RACE) btlSay("보물 경쟁은 민호의 승리!");
      }
      return;
    }
    // Three-live-care-slot boss. It has no gym badge and no tower streak;
    // winning grants an adventure reward and returns to the boss hub.
    if (btlBoss) {
      audioMusic(btlWon ? MUS_VICTORY : MUS_NONE);
      btlMsgCount = 0;
      if (btlWon) {
        extras.bossWin(btlBossType);
        sfxPlay(SFX_VICTORY);
        char rw[80]; extraRewardLabel(rw, sizeof(rw), extras.bossRewardKind(), extras.bossRewardId(), extras.bossRewardCount());
        btlSay("보스 격파! %s", rw);
      } else {
        btlSay("타입 보스에게 패배했다.");
      }
      return;
    }
    // Battle Tower owns its streak/reward loop. It deliberately bypasses gym
    // badges and the gym victory sheet, then returns to the tower hub.
    if (btlTower) {
      audioMusic(btlWon ? MUS_VICTORY : MUS_NONE);
      if (btlWon) {
        extras.towerWin(pet);
        sfxPlay(SFX_VICTORY);
        btlMsgCount = 0;
        char tw[64]; snprintf(tw, sizeof(tw), "배틀 타워 %u연승!", extras.towerStreak());
        btlSay("%s", tw);
      } else {
        uint16_t ended = extras.towerStreak();
        extras.towerLose();
        btlMsgCount = 0;
        char tl[64]; snprintf(tl, sizeof(tl), "%u연승에서 도전 종료", ended);
        btlSay("%s", tl);
      }
      return;
    }
    if (btlWon && btlTrainer >= 0 && !pet.hasBadge(btlRegion, btlTrainer, btlHard)) {
      pet.winBadge(btlRegion, btlTrainer, btlHard);
      btlNewBadge = true;
    }
    // A badge and nothing else made the ladder a one-way checklist. A win now
    // trains the creature that fought for it -- so a leader you can already
    // beat is worth returning to. It goes to the LIVE pet only: banked members
    // are frozen at the level and training they were banked with, and battling
    // already costs the live pet energy, which is what rate-limits the grind
    // without needing a cooldown.
    if (btlWon && btlTrainer >= 0 && btlPetIn) {
      // Later leaders are worth more, and hard mode is worth roughly double.
      uint8_t amt = (btlHard ? 6 + random(5) : 3 + random(3)) + btlTrainer / 3;
      btlTrainGain = pet.rewardTraining(amt, btlTrainWhich);
    }
    audioMusic(btlWon ? MUS_VICTORY : MUS_NONE);
    if (btlWon) sfxPlay(SFX_VICTORY);
    // Tell the peer before anything else: if we stop here without sending, the
    // other device sits on a battle that will never take another turn.
    if (btlLink && btlLinkHost) lan.sendEnd(btlWon);
    if (btlLink) { btlSay("%s", btlWon ? T(S_BTL_WIN) : T(S_BTL_LOSE)); return; }
    if (btlWon && btlTrainer >= 0) { btlWinUntil = millis() + 60000; return; }
    btlSay("%s", T(S_BTL_LOSE));
  }
}

static void btlHpBar(int x, int y, int w, const Combatant &c, uint16_t shown) {
  int fw = c.maxHp ? (w - 4) * shown / c.maxHp : 0;
  uint16_t col = (shown * 2 > c.maxHp) ? UI_BAR_OK
                 : (shown * 4 > c.maxHp) ? UI_BAR_WARN : UI_BAR_BAD;
  gfx->fillRoundRect(x, y, w, 14, 4, UI_TRACK);
  if (fw > 0) gfx->fillRoundRect(x + 2, y + 2, fw, 10, 3, col);
  gfx->drawRoundRect(x, y, w, 14, 4, UI_INK);
}

static void btlSide(int tx, int ty, int sx, int sy, const Combatant &c, uint8_t who) {
  // the scenes are busy, so the name and bar sit on their own plate rather
  // than fighting the artwork for contrast
  int ph = (who == 0) ? 54 : 40;
  gfx->fillRoundRect(tx - 8, ty - 8, 158, ph, 8, UI_BG_DAY);
  gfx->drawRoundRect(tx - 8, ty - 8, 158, ph, 8, UI_INK);
  char l[48];
  snprintf(l, sizeof(l), "%s Lv.%u", btlDisplayName(c), c.level);
  gfx->setTextColor(UI_INK);
  uiSetTextSize(1);
  uiSetCursor(tx, ty);
  gfx->print(l);
  gfx->setTextColor(UI_BAR_WARN);
  uiSetCursor(tx, ty + 14);
  gfx->print("HP");
  btlHpBar(tx + 18, ty + 12, 122, c, btlHpShown[who]);
  if (who == 0) {                 // your own numbers, as the games do
    char hp[16];
    snprintf(hp, sizeof(hp), "%u/%u", btlHpShown[who], c.maxHp);
    gfx->setTextColor(UI_INK);
    uiSetCursor(tx + 140 - uiTextWidth(hp, 1), ty + 28);
    gfx->print(hp);
  }
  if (c.ailment != AIL_NONE) {   // a status is the thing you most need to see
    static const StrId AIL_STR[] = { S_AIL_PARA, S_AIL_PARA, S_AIL_BURN, S_AIL_POISON,
                                     S_AIL_SLEEP, S_AIL_FREEZE, S_AIL_CONFUSE };
    gfx->setTextColor(UI_BAR_BAD);
    uiSetCursor(tx + 18, ty + 28);
    gfx->print(T(AIL_STR[c.ailment]));
  }
  // a platform under each creature, so they stand in the scene rather than
  // floating over it
  uint32_t now = millis();
  int ox = 0, oy = 0;
  bool flash = false;
  if (now < btlFaintUntil[who]) {          // sinks out of frame as it faints
    uint32_t left = btlFaintUntil[who] - now;
    oy += (int)((BTL_FAINT_MS - left) * 70 / BTL_FAINT_MS);
  } else if (c.fainted() && btlSwapWho == (int8_t)who) {
    return;                                // gone, waiting to be replaced
  }
  if (now < btlEnterUntil[who]) {          // and the next one rises into place
    uint32_t left = btlEnterUntil[who] - now;
    oy += (int)(left * 70 / BTL_ENTER_MS);
  }
  if (now < btlLungeUntil[who]) {          // lean in, then back out
    uint32_t left = btlLungeUntil[who] - now;
    int amt = (int)(left > BTL_LUNGE_MS / 2 ? BTL_LUNGE_MS - left : left) * 22 / (BTL_LUNGE_MS / 2);
    ox = who == 0 ? amt : -amt;            // you lunge right, the foe lunges left
    oy = who == 0 ? -amt / 2 : amt / 2;
  }
  if (now < btlHitUntil[who]) {
    uint32_t left = btlHitUntil[who] - now;
    ox += ((left / 50) % 2) ? 5 : -5;      // jitter
  }

  // Real PMD playback when the sprite streamed: attack while lunging, hurt
  // while flinching, idle otherwise. `has()` guards every one, because not
  // every species ships every action -- falling through to idle, and to the
  // flat thumbnail if the sprite is missing entirely (no SD).
  if (btlPmd[who].loaded) {
    uint8_t act = PMD_IDLE;
    bool loop = true;
    uint32_t t = now;
    if (now < btlHitUntil[who] && btlPmd[who].has(PMD_HURT)) {
      act = PMD_HURT; loop = false; t = now - (btlHitUntil[who] - BTL_HIT_MS);
    } else if (now < btlLungeUntil[who] && btlPmd[who].has(PMD_ATTACK)) {
      act = PMD_ATTACK; loop = false; t = now - (btlLungeUntil[who] - BTL_LUNGE_MS);
    }
    drawPmdActM(btlPmd[who], act, sx + 24 + ox, sy + 78 + oy, t, loop, false, 4);
    return;
  }
  if(isDigimonId(c.dex)&&loadBattleDigi(who,digimonIndex(c.dex))){
    int scale=2,dx=sx+ox,dy=sy+oy;bool mirror=who==1;
    for(int py=0;py<48;py++)for(int px=0;px<48;px++){int ix=mirror?47-px:px;uint16_t col=btlDigiPixels[who][py*48+ix];if(col!=btlDigiTransparent[who])canvasFillRectFast(dx+px*scale,dy+py*scale,scale,scale,col);}
    return;
  }
  const uint8_t *th = thumbs.get(c.dex);
  if (!th) return;
  if (now < btlHitUntil[who]) flash = ((btlHitUntil[who] - now) / 60) % 2 == 0;
  drawThumb(th, sx + ox, sy + oy, 3, flash);
}

// Bars drain rather than snap: a hit that removes half your health should be
// visible as it happens, not as a value that was already different.
static void btlEaseBars() {
  const uint16_t real[2] = { btlYou.hp, btlFoe.hp };
  for (int i = 0; i < 2; i++) {
    int diff = (int)real[i] - (int)btlHpShown[i];
    if (!diff) continue;
    int step = diff / 5;
    if (!step) step = diff > 0 ? 1 : -1;
    btlHpShown[i] = (uint16_t)((int)btlHpShown[i] + step);
  }
}

// The moment the ladder builds toward. It used to be one more line in the same
// message box as "It's super effective!", with the badge awarded silently.
void renderWin() {
  const Trainer &t = BTL_TRAINERS[btlTrainer];
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);

  gfx->setTextColor(UI_BAR_WARN);
  uiSetTextSize(3);
  uiSetCursor(CX - uiTextHalfWidth(T(S_BTL_WIN), 3), 54);
  gfx->print(T(S_BTL_WIN));

  char l[40];
  snprintf(l, sizeof(l), T(S_BTL_BEAT), t.name);
  gfx->setTextColor(UI_INK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(l, 2), 96);
  gfx->print(l);

  // the badge, large, with the hard-mode halo if that is how it was won
  if (btlTrainer < TRAINER_GYMS) {
    int by = 190;
    if (btlHard) {
      for (int r = 62; r >= 56; r--) gfx->drawCircle(CX, by, r, r % 2 ? 0xFEA0 : 0xFF60);
    }
    const BadgeArt *a = badgeArtFor(btlRegion, btlTrainer);
    if (a) {
      for (int r = 0; r < BADGE_PX; r++)
        for (int c = 0; c < BADGE_PX; c++) {
          uint8_t v = a->idx[r * BADGE_PX + c];
          if (v == 0xFF) continue;
          // 3x, so it reads as a prize rather than a list entry
          gfx->fillRect(CX - BADGE_PX * 3 / 2 + c * 3, by - BADGE_PX * 3 / 2 + r * 3,
                        3, 3, a->pal[v]);
        }
    } else if (modernBadgeRegion(btlRegion)) {
      drawModernBadge(btlRegion, btlTrainer, CX, by, 4, true);
    } else {
      // Regions without dedicated art keep the honest type-coloured medal.
      const Trainer &tr = TRAINER_SETS[btlRegion % GYM_REGIONS].list[btlTrainer];
      gfx->fillCircle(CX, by, 26, typeColor(tr.type));
      gfx->drawCircle(CX, by, 26, UI_INK);
      char n[4];
      snprintf(n, sizeof(n), "%u", (unsigned)(btlTrainer + 1));
      uiSetTextSize(3);
      gfx->setTextColor(typeColorIsLight(tr.type) ? UI_INK : UI_WHITE);
      uiSetCursor(CX - uiTextHalfWidth(n, 3), by - 11);
      gfx->print(n);
    }
    if (btlNewBadge) {
      gfx->setTextColor(UI_BAR_OK);
      uiSetTextSize(2);
      uiSetCursor(CX - uiTextHalfWidth(T(S_BTL_NEWBADGE), 2), 286);
      gfx->print(T(S_BTL_NEWBADGE));
    }
  }
  snprintf(l, sizeof(l), T(S_BADGES_FMT), pet.badgeCountIn(btlRegion, btlHard));
  gfx->setTextColor(UI_INK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(l, 2), 316);
  gfx->print(l);

  // what the win was worth beyond the badge
  if (btlTrainGain) {
    static const StrId NAMES[3] = { S_TR_ATK, S_TR_DEF, S_TR_SPE };
    snprintf(l, sizeof(l), T(S_WIN_TRAIN_FMT),
             T(NAMES[btlTrainWhich % 3]), btlTrainGain);
    gfx->setTextColor(UI_BAR_OK);
    uiSetTextSize(2);
    uiSetCursor(CX - uiTextHalfWidth(l, 2), 344);
    gfx->print(l);
  } else if (btlPetIn && btlTrainer >= 0) {
    gfx->setTextColor(UI_TRACK);
    uiSetTextSize(1);
    uiSetCursor(CX - uiTextHalfWidth(T(S_WIN_MAXED), 1), 348);
    gfx->print(T(S_WIN_MAXED));
  }

  gfx->setTextColor(UI_TRACK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(T(S_BACK), 2), 380);
  gfx->print(T(S_BACK));
  gfx->flush();
}

void renderBattle() {
  if (btlWinUntil) { renderWin(); return; }
  btlEaseBars();
  gfx->fillScreen(RGB565_BLACK);
  drawBattleBack();
  if (!btlLink) drawWeatherOverlay(btlWeather, millis(), 254);
  // the lower band stays flat so the move grid and the HP text keep their
  // contrast against it
  gfx->fillRect(0, 254, 466, 212, UI_BG_DAY);

  // v3.25 weather is visible in every local battle. Link battles deliberately
  // skip weather stat modifiers to keep both devices deterministic.
  if (!btlLink) {
    const char *wn=extras.weatherNameKo(btlWeather);
    int bw=uiTextWidth(wn,1)+28; int bx=CX-bw/2;
    gfx->fillRoundRect(bx,30,bw,22,7,UI_WHITE); gfx->drawRoundRect(bx,30,bw,22,7,UI_TRACK);
    gfx->setTextColor(UI_INK); uiSetTextSize(1); uiSetCursor(bx+12,36); gfx->print(wn);
  }
  if (btlBoss && btlBossPhase2) {
    const char *ph="2페이즈"; int pw=uiTextWidth(ph,1)+26;
    gfx->fillRoundRect(CX-pw/2,55,pw,22,7,C565(0xff,0xc8,0xb8)); gfx->drawRoundRect(CX-pw/2,55,pw,22,7,UI_BAR_BAD);
    gfx->setTextColor(UI_BAR_BAD);uiSetTextSize(1);uiSetCursor(CX-pw/2+12,61);gfx->print(ph);
  }

  // x=82 not 58: at y=60 the round bezel starts around x=77, and a longer
  // name like BLASTOISE was losing its first characters off the edge
  btlSide(82, 82, 300, 40, btlFoe, 1);    // foe reads top-left, sprite top-right
  btlSide(250, 190, 76, 168, btlYou, 0);  // you read bottom-right, sprite bottom-left

  // Waiting on the other device. Without this the screen is identical to the
  // one where it is your turn, so a tap that has been sent and a tap that was
  // never registered look exactly the same.
  bool lanWait = btlLink && !btlOver &&
                 (btlLinkHost ? (btlMyAct && !lan.hasPeerAct())
                              : (lan.state == LINK_WAITING));
  if (lanWait && !btlMsgCount) {
    gfx->fillRoundRect(BTL_GRID_X, BTL_GRID_Y, 328, BTL_CELL_H * 2 + 8, 12, UI_WHITE);
    gfx->drawRoundRect(BTL_GRID_X, BTL_GRID_Y, 328, BTL_CELL_H * 2 + 8, 12, UI_INK);
    gfx->setTextColor(UI_TRACK);
    const char *w = T(S_LAN_WAITFOE);
    uiDrawCenteredFit(w, CX, BTL_GRID_Y + 36, 300, 2, 1);
  } else if (btlMsgCount) {            // narration takes over the menu area
    gfx->fillRoundRect(BTL_GRID_X, BTL_GRID_Y, 328, BTL_CELL_H * 2 + 8, 12, UI_WHITE);
    gfx->drawRoundRect(BTL_GRID_X, BTL_GRID_Y, 328, BTL_CELL_H * 2 + 8, 12, UI_INK);
    gfx->setTextColor(UI_INK);
    for (uint8_t i = 0; i < btlMsgCount && i < 4; i++) {
      // Battle narration is essential feedback. Prefer size 2 for short and
      // medium Korean sentences; long names/messages safely fall back to 1.
      uiDrawCenteredFit(btlMsg[i], CX, BTL_GRID_Y + 8 + i * 19,
                        306, 2, 1);
    }
    gfx->setTextColor(UI_TRACK);
    uiDrawCenteredFit("터치", CX, BTL_GRID_Y + 84, 100, 1, 1);
  } else if (btlMenu == 0) {
    // FIGHT across the top, then POKEMON and RUN side by side. Three full-width
    // rows do not fit: the panel is round, and at that depth the chord is only
    // ~250 px. The lower two reuse the move grid's cells, so they inherit its
    // padded hit areas -- which is what made POKEMON hard to press before.
    gfx->fillRoundRect(BTL_GRID_X, BTL_GRID_Y, 328, BTL_CELL_H, 10, UI_BG_DAY);
    gfx->drawRoundRect(BTL_GRID_X, BTL_GRID_Y, 328, BTL_CELL_H, 10, UI_INK);
    gfx->setTextColor(UI_INK);
    uiSetTextSize(2);
    uiSetCursor(CX - uiTextHalfWidth(T(S_FIGHT), 2), BTL_GRID_Y + 14);
    gfx->print(T(S_FIGHT));
    const char *low[2] = { T(S_BTL_SWITCH), T(S_BTL_RUN) };
    for (int i = 0; i < 2; i++) {
      int x = BTL_CELL_X(i + 2), y = BTL_CELL_Y(i + 2);
      gfx->fillRoundRect(x, y, BTL_CELL_W, BTL_CELL_H, 10, UI_TRACK);
      gfx->drawRoundRect(x, y, BTL_CELL_W, BTL_CELL_H, 10, UI_INK);
      gfx->setTextColor(UI_INK);
      uiSetTextSize(2);
      uiSetCursor(x + (BTL_CELL_W - uiTextWidth(low[i], 2)) / 2, y + 14);
      gfx->print(low[i]);
    }
  } else if (btlMenu == 2) {
    drawBtlBack();
    // who to bring on instead; the current one and anything fainted is inert
    for (uint8_t i = 0; i < btlSquadN && i < 4; i++) {
      int x = BTL_CELL_X(i), y = BTL_CELL_Y(i);
      const Combatant &m = (i == btlSquadAt) ? btlYou : btlSquad[i];
      bool usable = (i != btlSquadAt) && !m.fainted();
      gfx->fillRoundRect(x, y, BTL_CELL_W, BTL_CELL_H, 10, usable ? UI_BG_DAY : UI_TRACK);
      gfx->drawRoundRect(x, y, BTL_CELL_W, BTL_CELL_H, 10, usable ? UI_INK : 0x8410);
      gfx->setTextColor(usable ? UI_INK : 0x8410);
      uiDrawLeftFit(btlDisplayName(m), x + 10, y + 4,
                    BTL_CELL_W - 20, 2, 1);
      char hp[20];
      snprintf(hp, sizeof(hp), "%u/%u", m.hp, m.maxHp);
      uiDrawLeftFit(hp, x + 10, y + 25, BTL_CELL_W - 20, 2, 1);
    }
  } else {
    drawBtlBack();
    for (int i = 0; i < MOVE_SLOTS; i++) {
      int x = BTL_CELL_X(i), y = BTL_CELL_Y(i);
      uint8_t mv = btlYou.moves[i];
      bool validMove = mv && mv < MOVE_COUNT;
      gfx->fillRoundRect(x, y, BTL_CELL_W, BTL_CELL_H, 10, validMove ? UI_BG_DAY : UI_TRACK);
      gfx->drawRoundRect(x, y, BTL_CELL_W, BTL_CELL_H, 10, UI_INK);
      if (!validMove) continue;
      gfx->setTextColor(UI_INK);
      uiDrawLeftFit(localizedMoveName(mv), x + 10, y + 4,
                    BTL_CELL_W - 20, 2, 1);
      // Same chip as the move list: in a fight the type IS the decision, and
      // grey 6px text was the least visible thing on the busiest screen.
      int cw = drawTypeChip(x + 10, y + 26, MOVE_TBL[mv].type);
      if ((creatureType1(btlYou.dex)==MOVE_TBL[mv].type||creatureType2(btlYou.dex)==MOVE_TBL[mv].type) &&
          MOVE_TBL[mv].cat != MC_STATUS) {
        uiSetTextSize(1);
        gfx->setTextColor(creatureAccent(btlYou.dex));
        uiSetCursor(x + 10 + cw + 4, y + 30);
        gfx->print("+");
      }
    }
  }
  gfx->flush();
}

// Brings on the flagged replacement and starts its entrance.
static void btlDoSwap() {
  uint32_t now = millis();
  if (btlSwapWho == 1 && btlLink) {
    btlFoeSquad[btlFoeAt] = btlFoe;
    // the next one still standing, not simply the next index
    uint8_t nxt = btlFoeAt;
    while (++nxt < btlFoeSquadN && btlFoeSquad[nxt].fainted()) {}
    if (nxt >= btlFoeSquadN) { btlSwapWho = -1; return; }
    btlFoeAt = nxt;
    btlFoe = btlFoeSquad[btlFoeAt];
    btlHpShown[1] = btlFoe.hp;
    btlSyncSprite(1, btlFoe);
    btlLungeUntil[1] = btlHitUntil[1] = btlFaintUntil[1] = 0;
    btlEnterUntil[1] = now + BTL_ENTER_MS;
    btlSay(T(S_BTL_SENDS), lan.peerName, btlDisplayName(btlFoe));
  } else if (btlSwapWho == 1 && btlRival) {
    btlFoeSquad[btlFoeAt] = btlFoe;
    uint8_t nxt = btlFoeAt;
    while (++nxt < btlFoeSquadN && btlFoeSquad[nxt].fainted()) {}
    if (nxt >= btlFoeSquadN) { btlSwapWho = -1; return; }
    btlFoeAt = nxt;
    btlFoe = btlFoeSquad[btlFoeAt];
    btlHpShown[1] = btlFoe.hp;
    btlSyncSprite(1, btlFoe);
    btlLungeUntil[1] = btlHitUntil[1] = btlFaintUntil[1] = 0;
    btlEnterUntil[1] = now + BTL_ENTER_MS;
    btlSay(T(S_BTL_SENDS), extras.rivalNameKo(), btlDisplayName(btlFoe));
  } else if (btlSwapWho == 1) {
    const Trainer &t = BTL_TRAINERS[btlTrainer];
    btlFoeAt++;
    foeFromSpecies(btlFoe, trainerDexWithArt(t.team[btlFoeAt].dex, t.type), t.team[btlFoeAt].level,
                   btlHard ? HARD_IV : EASY_IV);
    extras.applyWeatherToCombatant(btlFoe, btlWeather);
    btlHpShown[1] = btlFoe.maxHp;
    btlSyncSprite(1, btlFoe);
    btlLungeUntil[1] = btlHitUntil[1] = btlFaintUntil[1] = 0;
    btlEnterUntil[1] = now + BTL_ENTER_MS;
    btlSay(T(S_BTL_SENDS), t.name, btlDisplayName(btlFoe));
  } else if (btlSwapWho == 0) {
    btlSquad[btlSquadAt] = btlYou;     // remember how battered it was
    btlSquadAt++;
    btlYou = btlSquad[btlSquadAt];
    btlHpShown[0] = btlYou.hp;
    btlSyncSprite(0, btlYou);
    btlLungeUntil[0] = btlHitUntil[0] = btlFaintUntil[0] = 0;
    btlEnterUntil[0] = now + BTL_ENTER_MS;
    btlSay(T(S_BTL_GO), btlDisplayName(btlYou));
  }
  btlSwapWho = -1;
}

// Switching spends your turn: the opponent still acts. That is what stops it
// being a free look at the matchup every round.
static void btlSwitchTo(uint8_t i) {
  if (i >= btlSquadN || i == btlSquadAt) return;
  btlSquad[btlSquadAt] = btlYou;
  btlSquadAt = i;
  btlYou = btlSquad[i];
  btlHpShown[0] = btlYou.hp;
  btlSyncSprite(0, btlYou);
  btlLungeUntil[0] = btlHitUntil[0] = btlFaintUntil[0] = 0;
  btlEnterUntil[0] = millis() + BTL_ENTER_MS;
  btlMenu = 0;
  btlSay(T(S_BTL_GO), btlDisplayName(btlYou));
  btlResolve(0);          // move 0 = no attack, so only the foe acts
}

int btlCellIndexAt(int16_t x, int16_t y) {
  for (int i = 0; i < 4; i++)
    if (btlCellHit(i, x, y)) return i;
  return -1;
}

// The way out of the move and switch screens. Without it the only exits were
// choosing something or leaving the fight entirely.
static void drawBtlBack() {
  gfx->fillRoundRect(BTL_BACK_X, BTL_BACK_Y, BTL_BACK_W, BTL_BACK_H, 11, UI_TRACK);
  gfx->drawRoundRect(BTL_BACK_X, BTL_BACK_Y, BTL_BACK_W, BTL_BACK_H, 11, UI_INK);
  gfx->setTextColor(UI_INK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(T(S_BACK), 2), BTL_BACK_Y + 14);
  gfx->print(T(S_BACK));
}

static bool btlBackTap(int16_t x, int16_t y) {
  if (x < BTL_BACK_X || x > BTL_BACK_X + BTL_BACK_W ||
      y < BTL_BACK_Y || y > BTL_BACK_Y + BTL_BACK_H) return false;
  btlMenu = 0;
  sfxPlay(SFX_TAP);
  return true;
}

// Running. A gym leader keeps their badge and the fight simply ends; against
// another device the peer is told, so it does not sit waiting for a move that
// will never come.
static void btlRun() {
  sfxPlay(SFX_DENY);
  if (btlTower) { extras.towerLose(); btlTower = false; towerOpen = true; }
  if (btlBoss) { btlBoss = false; bossOpen = true; }
  if (btlRival) { extras.rivalResult(false, pet); btlRival = false; btlRivalSpecial = RIVSPEC_NONE; rivalOpen = true; }
  btlFreeSprites();
  audioMusic(MUS_NONE);
  if (btlLink) { lanLeave(); btlLink = false; lanOpen = true; }
  battleOpen = false;
  btlMenu = 0;
}

void battleTap(int16_t x, int16_t y) {
  if (btlWinUntil) {          // dismiss the win screen and leave the fight
    btlWinUntil = 0;
    btlFreeSprites();
    audioMusic(MUS_NONE);
    battleOpen = false;
    if (btlLink) { btlLink = false; lanOpen = true; }
    return;
  }
  if (btlMsgCount) {          // a tap clears the narration and returns the menu
    btlMsgCount = 0;
    if (btlOver) {
      btlFreeSprites();
      // A special battle returns directly to its hub rather than the generic
      // win sheet. Stop the victory/battle track at that exact transition;
      // otherwise the boss hub could inherit the battle BGM.
      audioMusic(MUS_NONE);
      btlMenu = 0;
      btlBossPhase2 = false;
      battleOpen = false;
      if (btlTower) { btlTower = false; towerOpen = true; }
      if (btlBoss) { btlBoss = false; bossOpen = true; }
      if (btlRival) { btlRival = false; btlRivalSpecial = RIVSPEC_NONE; rivalOpen = true; }
      // Back to the LAN screen rather than all the way out: that is where a
      // rematch is offered, and re-pairing for every fight would be tedious.
      if (btlLink) { btlLink = false; lanOpen = true; }
      return;
    }
    if (btlSwapWho >= 0) btlDoSwap();   // the replacement arrives on this beat
    return;
  }
  if (btlMenu == 0) {
    if (x >= BTL_GRID_X - BTL_HIT_PAD && x <= BTL_GRID_X + 328 + BTL_HIT_PAD &&
        y >= BTL_HIT_Y0(0) && y <= BTL_HIT_Y1(0)) {
      sfxPlay(SFX_TAP);
      btlMenu = 1;                       // FIGHT
      return;
    }
    if (btlCellHit(2, x, y)) { sfxPlay(SFX_TAP); btlMenu = 2; return; }
    if (btlCellHit(3, x, y)) { btlRun(); return; }
    return;
  }
  if (btlMenu == 2) {
    if (btlBackTap(x, y)) return;
    for (uint8_t i = 0; i < btlSquadN && i < 4; i++) {
      if (!btlCellHit(i, x, y)) continue;
      const Combatant &m = (i == btlSquadAt) ? btlYou : btlSquad[i];
      if (i == btlSquadAt || m.fainted()) { sfxPlay(SFX_DENY); return; }
      sfxPlay(SFX_TAP);
      if (btlLink && !btlLinkHost) {
        // The guest asks; it never switches on its own. A switch rides the same
        // message as a move, so the host spends the turn on it exactly as it
        // would for us.
        lan.sendAct(LINK_ACT_SWITCH_TO(i));
        btlMenu = 0;
        return;
      }
      if (btlLink) {            // host: latched like a move, see btlLinkPoll
        btlMyAct = LINK_ACT_SWITCH_TO(i);
        btlMenu = 0;
        if (!lan.hasPeerAct()) return;
        btlMyAct = 0;
      }
      btlSwitchTo(i);
      return;
    }
    btlMenu = 0;      // anywhere else backs out
    return;
  }
  if (btlBackTap(x, y)) return;
  for (int i = 0; i < MOVE_SLOTS; i++) {
    if (!btlYou.moves[i]) continue;
    if (!btlCellHit(i, x, y)) continue;
    sfxPlay(SFX_TAP);
    btlMenu = 0;
    if (btlLink && !btlLinkHost) {
      lan.sendAct(LINK_ACT_MOVE(i));   // the guest asks; the host decides
      return;
    }
    if (btlLink) {
      // Latched, not discarded. The host used to throw the tap away when the
      // rival had not chosen yet, so you had to keep jabbing at the move until
      // the timing happened to line up.
      btlMyAct = LINK_ACT_MOVE(i);
      if (!lan.hasPeerAct()) return;   // resolved by btlLinkPoll when it lands
      btlMyAct = 0;
    }
    btlResolve(btlYou.moves[i]);
    return;
  }
  btlMenu = 0;        // a tap off the grid goes back to FIGHT/POKEMON
}

// ---------- player card (swipe down) ----------
// Everything here is player-wide and outlives the creature: badges, the daily
// streak, the Pokedex and the party. No player sprite yet -- the SD carries
// PMD creature sprites only, so a trainer portrait needs new art.
// Page 1: who you are -- avatar, badges, totals. Tap the avatar to cycle it;
// the four sprites are hand-drawn (see species.h) because SpriteCollab has no
// trainer art and ripped sprites would be unlicensed.
static void renderPlayerBadges() {
  // the player's name if they have set one, the generic title if not; either
  // way tapping it opens the keyboard
  const char *tn = pet.trainerName[0] ? pet.trainerName : T(S_TRAINER);
  gfx->setTextColor(UI_INK);
  uiSetTextSize(3);
  uiSetCursor(CX - uiTextHalfWidth(tn, 3), 40);
  gfx->print(tn);

  // Pages 1 and 2 are the other regions' ladders: name them, and drop the
  // avatar so the badges have the room. Only page 0 is "you".
  if (playerBadgeRegion != 0) {
    const char *rn = localizedRegionName(gymRegionDexRegion(playerBadgeRegion));
    gfx->setTextColor(UI_INK);
    uiSetTextSize(2);
    uiSetCursor(CX - uiTextHalfWidth(rn, 2), 120);
    gfx->print(rn);
  } else if (gShowAllAvatars) {          // emulator only: every avatar at once
    for (uint8_t i = 0; i < AVATAR_COUNT; i++)
      drawAvatar(i, 60 + (i % 4) * 88, 60 + (i / 4) * 60, 3);
  } else {
    drawAvatar(pet.avatar, CX - AVATAR_PX * 2, 72, 4);
    // the sprite alone is not always obvious at 16x16, so it is named
    const char *an = AVATARS[pet.avatar % AVATAR_COUNT].name;
    gfx->setTextColor(UI_INK);
    uiSetTextSize(1);
    uiSetCursor(CX - uiTextHalfWidth(an, 1), 143);
    gfx->print(an);
    gfx->setTextColor(UI_TRACK);
    uiSetCursor(CX - uiTextHalfWidth(T(S_AVATAR_HINT), 1), 154);
    gfx->print(T(S_AVATAR_HINT));
  }

  // The real badges, 2x4. Unearned ones draw as a faint outline so the shape
  // of what is missing is still visible.
  for (int i = 0; i < TRAINER_GYMS; i++) {
    int bx = 140 + (i % 4) * 62, by = 188 + (i / 4) * 62;
    bool got = pet.hasBadge(playerBadgeRegion, i, false);
    bool hard = pet.hasBadge(playerBadgeRegion, i, true);
    if (hard) {
      // Beaten on hard: a golden halo. Concentric rings, not a filled disc --
      // a disc sat behind the art and read as a gold coin rather than a glow.
      gfx->drawCircle(bx, by, 25, 0xFDE0);
      gfx->drawCircle(bx, by, 24, 0xFEA0);
      gfx->drawCircle(bx, by, 23, 0xFF60);
      gfx->drawCircle(bx, by, 22, 0xFEA0);
      gfx->drawCircle(bx, by, 21, 0xFDE0);
    }
    if (!got) {
      if (modernBadgeRegion(playerBadgeRegion)) drawModernBadge(playerBadgeRegion, i, bx, by, 2, false);
      else gfx->drawCircle(bx, by, 20, UI_TRACK);
      continue;
    }
    const BadgeArt *a = badgeArtFor(playerBadgeRegion, i);
    if (!a) {
      if (modernBadgeRegion(playerBadgeRegion)) drawModernBadge(playerBadgeRegion, i, bx, by, 2, true);
      else {
        gfx->fillCircle(bx, by, 14, typeColor(TRAINER_SETS[playerBadgeRegion % GYM_REGIONS].list[i].type));
        gfx->drawCircle(bx, by, 14, UI_INK);
      }
      continue;
    }
    for (int r = 0; r < BADGE_PX; r++)
      for (int c = 0; c < BADGE_PX; c++) {
        uint8_t v = a->idx[r * BADGE_PX + c];
        if (v == 0xFF) continue;
        gfx->fillRect(bx - BADGE_PX / 2 + c, by - BADGE_PX / 2 + r, 1, 1, a->pal[v]);
      }
  }
  char l[64];
  gfx->setTextColor(UI_INK);
  uiSetTextSize(2);
  snprintf(l, sizeof(l), T(S_STREAK_FMT), pet.streak, pet.bestStreak);
  uiSetCursor(CX - uiTextHalfWidth(l, 2), 286);
  gfx->print(l);
  snprintf(l, sizeof(l), T(S_POKEDEX_FMT), pet.registeredCount(), regionDexCount(REGION_ALL));
  uiSetCursor(CX - uiTextHalfWidth(l, 2), 312);
  gfx->print(l);
  snprintf(l, sizeof(l), T(S_PARTY_FMT), party.count());
  uiSetCursor(CX - uiTextHalfWidth(l, 2), 338);
  gfx->print(l);
}

// Page 2: the medals. They used to sit on the creature's card; they belong with
// the player, since totalMedals accumulates across every pet you raise.
static void renderPlayerMedals() {
  int got = 0;
  for (int i = 0; i < MED_COUNT; i++)
    if (pet.hasMedal(1 << i)) got++;
  char head[24];
  snprintf(head, sizeof(head), T(S_MEDALS_FMT), got, MED_COUNT);
  gfx->setTextColor(UI_INK);
  uiSetTextSize(3);
  uiSetCursor(CX - uiTextHalfWidth(head, 3), 44);
  gfx->print(head);

  for (int i = 0; i < MED_COUNT; i++) {
    int x = 46 + (i % 2) * 190, y = 96 + (i / 2) * 58;
    bool g = pet.hasMedal(1 << i);
    gfx->fillRoundRect(x, y, 180, 48, 10, g ? UI_BAR_OK : UI_TRACK);
    gfx->drawRoundRect(x, y, 180, 48, 10, UI_INK);
    gfx->setTextColor(g ? UI_BG_DAY : UI_INK);
    uiSetTextSize(2);
    uiSetCursor(x + (180 - uiTextWidth(medalLabel(i), 2)) / 2, y + 6);
    gfx->print(medalLabel(i));
    uiSetTextSize(1);
    uiSetCursor(x + (180 - uiTextWidth(medalDesc(i), 1)) / 2, y + 30);
    gfx->print(medalDesc(i));
  }
  char tot[28];
  snprintf(tot, sizeof(tot), T(S_MEDALS_TOTAL_FMT), pet.totalMedals);
  gfx->setTextColor(UI_TRACK);
  uiSetTextSize(1);
  uiSetCursor(CX - uiTextHalfWidth(tot, 1), 344);
  gfx->print(tot);
}

void renderPlayer() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  if (playerPage < GYM_REGIONS) renderPlayerBadges();
  else renderPlayerMedals();

  for (uint8_t i = 0; i < PLAYER_PAGES; i++) {
    int dx = CX - (PLAYER_PAGES - 1) * 13 + i * 26;
    if (i == playerPage) gfx->fillCircle(dx, 366, 5, UI_INK);
    else gfx->drawCircle(dx, 366, 4, UI_INK);
  }
  gfx->setTextColor(UI_TRACK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(T(S_BACK), 2), 392);
  gfx->print(T(S_BACK));
  gfx->flush();
}

// ---------- lightning target (trains SPEED) ----------
// A short reaction game for the round screen: tap the lightning target before
// it vanishes. Consecutive hits build a combo, while an occasional golden
// target is worth two points. Missing a target only resets the combo; there is
// no fiddly movement control, so the game stays quick and readable.
#define SPD_MS 20000UL       // 20-second session
#define SPD_LIFE0 1200       // first target's window, ms
#define SPD_LIFE_MIN 430
#define SPD_R 43             // target radius

void spdSpawn() {
  // Keep the whole target inside the round bezel. Golden targets become a bit
  // more common after the player has settled into a streak.
  int ang = random(360);
  int rad = random(148);
  float a = ang * 3.14159f / 180.0f;
  spdX = CX + (int)(cosf(a) * rad);
  spdY = CY + (int)(sinf(a) * rad);
  spdGold = random(100) < (spdCombo >= 6 ? 20 : 12);
  spdBorn = millis();
}

void startSpeedGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  spdOpen = true;
  spdUntil = millis() + SPD_MS;
  spdOverUntil = 0;
  spdHits = 0;
  spdMisses = 0;
  spdCombo = 0;
  spdBestCombo = 0;
  spdGain = 0;
  spdNewHi = false;
  spdIvReward = XITEM_COUNT;
  spdIvRewardCount = 0;
  spdShinyBerryReward = false;
  spdGold = false;
  spdSpawn();
}

static uint16_t spdLife() {
  int life = SPD_LIFE0 - (int)spdHits * 24;
  if (spdGold) life -= 90;  // bonus target asks for a slightly faster reaction
  return life < SPD_LIFE_MIN ? SPD_LIFE_MIN : life;
}

void spdTap(int16_t x, int16_t y) {
  if (spdOverUntil) { spdOpen = false; digiReact(DIGI_ANIM_TRAIN); return; }
  int dx = x - spdX, dy = y - spdY;
  if (dx * dx + dy * dy <= (SPD_R + 14) * (SPD_R + 14)) {
    spdHits += spdGold ? 2 : 1;
    spdCombo++;
    if (spdCombo > spdBestCombo) spdBestCombo = spdCombo;
    sfxPlay(spdGold ? SFX_MEDAL : SFX_TAP);
    spdSpawn();
  }
}

static void drawLightningBolt(int cx, int cy, int scale, uint16_t color) {
  // Two filled triangles make a crisp bolt without depending on a Unicode glyph.
  gfx->fillTriangle(cx - 4*scale, cy - 14*scale,
                    cx + 7*scale, cy - 14*scale,
                    cx + 1*scale, cy - 1*scale, color);
  gfx->fillTriangle(cx - 1*scale, cy - 1*scale,
                    cx + 7*scale, cy - 1*scale,
                    cx - 7*scale, cy + 15*scale, color);
}

void renderSpeed() {
  uint32_t now = millis();
  drawTrainingBackdrop(currentSceneType(), now, 0.0f, 376);
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;

  if (spdOverUntil) {
    if (now > spdOverUntil) { spdOpen = false; digiReact(DIGI_ANIM_TRAIN); return; }
    char b[32];
    snprintf(b, sizeof(b), "점수 %u", spdHits);
    gfx->setTextColor(ink);
    uiSetTextSize(4);
    uiSetCursor(CX - uiTextHalfWidth(b, 4), 138);
    gfx->print(b);
    char g[24];
    snprintf(g, sizeof(g), T(S_SPD_GAIN_FMT), spdGain);
    gfx->setTextColor(UI_BAR_WARN);
    uiSetTextSize(3);
    uiSetCursor(CX - uiTextHalfWidth(g, 3), 202);
    gfx->print(g);
    char c[28];
    snprintf(c, sizeof(c), "최고 콤보 %u", spdBestCombo);
    gfx->setTextColor(ink);
    uiSetTextSize(2);
    uiSetCursor(CX - uiTextHalfWidth(c, 2), 250);
    gfx->print(c);
    if (spdNewHi && spdHits > 0) {
      gfx->setTextColor(UI_BAR_WARN);
      uiSetCursor(CX - uiTextHalfWidth(T(S_NEW_RECORD), 2), 286);
      gfx->print(T(S_NEW_RECORD));
    } else {
      char r[20];
      snprintf(r, sizeof(r), T(S_RECORD_FMT), pet.spdHi);
      gfx->setTextColor(ink);
      uiSetCursor(CX - uiTextHalfWidth(r, 2), 286);
      gfx->print(r);
    }
    if (spdIvReward < XITEM_COUNT && spdIvRewardCount) {
      char rw[72]; snprintf(rw, sizeof(rw), "훈련 보상: %s x%u", extras.itemNameKo(spdIvReward), (unsigned)spdIvRewardCount);
      gfx->setTextColor(UI_BAR_OK);
      uiDrawCenteredFit(rw, CX, 312, 390, 2, 1);
    }
    if (spdShinyBerryReward) {
      char rw[72]; snprintf(rw, sizeof(rw), "희귀 보상: %s x1", extras.itemNameKo(XITEM_SHINY_BERRY));
      gfx->setTextColor(UI_BAR_WARN);
      uiDrawCenteredFit(rw, CX, 334, 390, 2, 1);
    }
    gfx->flush();
    return;
  }

  if (now >= spdUntil) {
    spdNewHi = (spdHits > pet.spdHi);
    extras.beginBatch();
    spdGain = pet.trainSpeed(spdHits);
    bool rewardReady = spdHits > 0;
    spdIvReward = grantTrainingIvBerry(XITEM_IV_SPE, rewardReady, spdIvRewardCount);
    spdShinyBerryReward = grantTrainingShinyBerry(rewardReady);
    extras.endBatch(false);
    queueTrainingPersist();
    sfxPlay(spdShinyBerryReward || spdIvRewardCount > 1 || spdNewHi ? SFX_MEDAL : SFX_PLAY);
    spdOverUntil = now + 3500;
    gfx->flush();
    return;
  }

  if (now - spdBorn > spdLife()) {
    spdMisses++;
    spdCombo = 0;
    spdSpawn();
  }

  uint32_t age = now - spdBorn;
  int life = spdLife();
  int r = SPD_R - (int)((uint32_t)SPD_R * age / (life ? life : 1) / 3);
  if (r < 18) r = 18;
  uint16_t outer = spdGold ? UI_BAR_WARN : UI_BAR_BAD;
  gfx->fillCircle(spdX, spdY, r + 6, outer);
  gfx->fillCircle(spdX, spdY, r, UI_WHITE);
  gfx->drawCircle(spdX, spdY, r - 3, outer);
  drawLightningBolt(spdX, spdY, max(1, r / 22), outer);

  char b[24];
  snprintf(b, sizeof(b), "점수 %u", spdHits);
  gfx->setTextColor(ink);
  uiSetTextSize(3);
  uiSetCursor(CX - uiTextHalfWidth(b, 3), 26);
  gfx->print(b);

  uint32_t left = (spdUntil > now) ? (spdUntil - now + 999) / 1000 : 0;
  snprintf(b, sizeof(b), "%us", (unsigned)left);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(b, 2), 66);
  gfx->print(b);

  if (spdCombo >= 2) {
    snprintf(b, sizeof(b), "콤보 x%u", spdCombo);
    gfx->setTextColor(spdGold ? UI_BAR_WARN : UI_BAR_OK);
    uiSetTextSize(2);
    uiSetCursor(CX - uiTextHalfWidth(b, 2), 94);
    gfx->print(b);
  }
  if (spdGold) {
    gfx->setTextColor(UI_BAR_WARN);
    uiSetTextSize(1);
    uiSetCursor(CX - uiTextHalfWidth("황금 번개 +2", 1), 116);
    gfx->print("황금 번개 +2");
  }
  gfx->flush();
}

// ---------- team select ----------
// Which creatures come to this fight. It exists because hard mode caps your
// team to the leader's size, so the difference between a sweep and a wipe is
// bringing the right type -- and the squad used to be simply whoever sat first
// in the party.

// candidate n: 0 = live pet, 1..PARTY_SLOTS = party, then BOX_SLOTS box entries.
// LAN remains live+party only because its packet format predates box selection.
uint8_t pickCandidateLimit() {
  return pickTrainer == PICK_LAN ? PARTY_SLOTS : (uint8_t)(PARTY_SLOTS + BOX_SLOTS);
}
bool pickExists(uint8_t n) {
  if (n == 0) return !pet.isEgg() && creatureHasArt(pet.speciesId);
  if (n <= PARTY_SLOTS)
    return !party.slots[n - 1].empty() && speciesHasArt(party.slots[n - 1].dex);
  uint8_t b = (uint8_t)(n - PARTY_SLOTS - 1);
  return b < BOX_SLOTS && !party.box[b].empty() && speciesHasArt(party.box[b].dex);
}
bool pickVisible(uint8_t n) {
  if (!pickExists(n)) return false;
  if (pickTrainer == PICK_LAN) return n <= PARTY_SLOTS;
  return pickSourceTab == 0 ? n <= PARTY_SLOTS : n > PARTY_SLOTS;
}
uint8_t pickChosen() {
  uint8_t c = 0;
  for (uint8_t n = 0; n <= pickCandidateLimit(); n++)
    if (pickExists(n) && (squadMask & (1UL << n))) c++;
  return c;
}
uint8_t pickCandidates() {
  uint8_t c = 0;
  for (uint8_t n = 0; n <= pickCandidateLimit(); n++)
    if (pickVisible(n)) c++;
  return c;
}
// Trims the selection to the first `cap` candidates. The default used to be
// "everything", which with a live pet plus six banked is seven against a cap of
// six -- so the screen opened already invalid.
void pickDefault(uint8_t cap) {
  squadMask = 0;
  uint8_t taken = 0;
  for (uint8_t n = 0; n <= pickCandidateLimit() && taken < cap; n++)
    if (pickExists(n)) { squadMask |= (1UL << n); taken++; }
}

static void drawPickCell(uint8_t n, int x, int y, uint8_t capLvl) {
  bool on = (squadMask & (1UL << n)) != 0;
  int16_t dex; uint16_t lvl; const char *nm; bool shiny;
  if (n == 0) {
    dex = pet.speciesId; lvl = pet.level(); shiny = pet.shiny;
    nm = pet.nick[0] ? pet.nick : creatureName(dex);
  } else if (n <= PARTY_SLOTS) {
    const PartyMon &m = party.slots[n - 1];
    dex = m.dex; lvl = m.level; shiny = m.shiny;
    nm = m.nick[0] ? m.nick : creatureName(dex);
  } else {
    const PartyMon &m = party.box[n - PARTY_SLOTS - 1];
    dex = m.dex; lvl = m.level; shiny = m.shiny;
    nm = m.nick[0] ? m.nick : creatureName(dex);
  }
  if (capLvl && lvl > capLvl) lvl = capLvl;   // show the level it will FIGHT at
  gfx->fillRoundRect(x, y, PICK_CELL_W, PICK_CELL_H, 10, on ? UI_BG_DAY : UI_TRACK);
  gfx->drawRoundRect(x, y, PICK_CELL_W, PICK_CELL_H, 10, on ? UI_INK : 0x8410);
  const uint8_t *th = thumbs.get(dex);
  if (th) drawThumb(th, x - 12, y - 6, 2, !on);
  const char *source = n == 0 ? "현재" : (n <= PARTY_SLOTS ? "파티" : "박스");
  gfx->setTextColor(on ? UI_BAR_OK : 0x8410);
  uiSetTextSize(1);
  uiSetCursor(x + 6, y + PICK_CELL_H - 13);
  gfx->print(source);
  gfx->setTextColor(on ? UI_INK : 0x8410);
  uiSetTextSize(1);
  uiSetCursor(x + 54, y + 14);
  gfx->print(nm);
  char l[16];
  snprintf(l, sizeof(l), "Lv.%u%s", (unsigned)lvl, shiny ? " *" : "");
  uiSetCursor(x + 54, y + 30);
  gfx->print(l);
  // its typing is the whole reason you are on this screen
  uint8_t t1=creatureType1(dex),t2=creatureType2(dex);
  gfx->setTextColor(on ? creatureAccent(dex) : 0x8410);
  uiSetCursor(x + 54, y + 48);
  gfx->print(localizedTypeName(t1));
  if (t2 != T_NONE) {
    uiSetCursor(x + 54, y + 60);
    gfx->print(localizedTypeName(t2));
  }
  if (on) {
    gfx->fillCircle(x + PICK_CELL_W - 16, y + 16, 9, UI_BAR_OK);
    gfx->setTextColor(UI_BG_DAY);
    uiSetCursor(x + PICK_CELL_W - 19, y + 13);
    gfx->print("*");
  }
}

void renderPick() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  uint8_t cap = pickTrainer == PICK_BOSS ? 3 :
                ((pickTrainer == PICK_LAN) ? TRAINER_TEAM_MAX : squadCapForRegion(pickRegion, pickTrainer, pickHard));
  uint8_t top = 0;          // the level cap shown on each cell; 0 = uncapped
  char head[40];
  if (pickTrainer == PICK_BOSS) {
    snprintf(head, sizeof(head), "타입 보스 출전 멤버");
  } else if (pickTrainer == PICK_LAN) {
    snprintf(head, sizeof(head), "%s: %s", T(S_LAN),
             lanWantHost ? T(S_LAN_HOST) : T(S_LAN_JOIN));
  } else {
    const Trainer &t = TRAINER_SETS[pickRegion % GYM_REGIONS].list[pickTrainer];
    for (int k = 0; k < t.count; k++)
      if (t.team[k].level > top) top = t.team[k].level;
    snprintf(head, sizeof(head), "%s  Lv.%u x%u", t.name, top, t.count);
  }
  gfx->setTextColor(UI_INK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(head, 2), 28);
  gfx->print(head);
  char sub[28];
  snprintf(sub, sizeof(sub), T(S_PICK_FMT), pickChosen(), cap);
  uiSetTextSize(1);
  gfx->setTextColor(pickChosen() > cap ? UI_BAR_BAD : UI_TRACK);
  uiSetCursor(CX - uiTextHalfWidth(sub, 1), 54);
  gfx->print(sub);

  if (pickTrainer != PICK_LAN) {
    const char *tabs[2] = { "현재·파티", "박스" };
    for (uint8_t i = 0; i < 2; i++) {
      bool active = pickSourceTab == i;
      int tx = PICK_TAB_X(i);
      gfx->fillRoundRect(tx, PICK_TAB_Y, PICK_TAB_W, PICK_TAB_H, 9,
                         active ? UI_BAR_OK : UI_WHITE);
      gfx->drawRoundRect(tx, PICK_TAB_Y, PICK_TAB_W, PICK_TAB_H, 9, UI_INK);
      gfx->setTextColor(active ? UI_WHITE : UI_INK);
      uiSetTextSize(2);
      uiSetCursor(tx + (PICK_TAB_W - uiTextWidth(tabs[i], 2)) / 2, PICK_TAB_Y + 7);
      gfx->print(tabs[i]);
    }
  }

  uint8_t seen = 0, drawn = 0;
  for (uint8_t n = 0; n <= pickCandidateLimit(); n++) {
    if (!pickVisible(n)) continue;
    if (seen++ < pickPage * PICK_PER_PAGE) continue;
    if (drawn >= PICK_PER_PAGE) break;
    drawPickCell(n, PICK_X(drawn), PICK_Y(drawn), top);
    drawn++;
  }
  uint8_t pages = (pickCandidates() + PICK_PER_PAGE - 1) / PICK_PER_PAGE;
  if (!pages) pages = 1;
  for (uint8_t i = 0; i < pages && pages > 1; i++) {
    int dx = CX - (pages - 1) * 13 + i * 26;
    if (i == pickPage) gfx->fillCircle(dx, 338, 5, UI_INK);
    else gfx->drawCircle(dx, 338, 4, UI_INK);
  }

  bool ok = pickTrainer == PICK_BOSS ? pickChosen() == 3 :
            (pickChosen() > 0 && pickChosen() <= cap);
  gfx->fillRoundRect(PICK_BACK_X, PICK_GO_Y, PICK_BTN_W, PICK_BTN_H, 12, UI_TRACK);
  gfx->drawRoundRect(PICK_BACK_X, PICK_GO_Y, PICK_BTN_W, PICK_BTN_H, 12, UI_INK);
  gfx->setTextColor(UI_INK);
  uiSetTextSize(2);
  uiSetCursor(PICK_BACK_X + (PICK_BTN_W - uiTextWidth(T(S_BACK), 2)) / 2,
                 PICK_GO_Y + 14);
  gfx->print(T(S_BACK));
  gfx->fillRoundRect(PICK_GO_X, PICK_GO_Y, PICK_BTN_W, PICK_BTN_H, 12,
                     ok ? UI_BAR_OK : UI_TRACK);
  gfx->drawRoundRect(PICK_GO_X, PICK_GO_Y, PICK_BTN_W, PICK_BTN_H, 12, UI_INK);
  gfx->setTextColor(ok ? UI_BG_DAY : 0x8410);
  uiSetTextSize(2);
  uiSetCursor(PICK_GO_X + (PICK_BTN_W - uiTextWidth(T(S_FIGHT), 2)) / 2,
                 PICK_GO_Y + 14);
  gfx->print(T(S_FIGHT));
  gfx->flush();
}

void pickTap(int16_t x, int16_t y) {
  if (pickTrainer != PICK_LAN && y >= PICK_TAB_Y && y <= PICK_TAB_Y + PICK_TAB_H) {
    for (uint8_t i = 0; i < 2; i++) {
      int tx = PICK_TAB_X(i);
      if (x < tx || x > tx + PICK_TAB_W) continue;
      pickSourceTab = i;
      pickPage = 0;
      sfxPlay(SFX_TAP);
      return;
    }
  }
  if (y >= PICK_GO_Y && y <= PICK_GO_Y + PICK_BTN_H &&
      x >= PICK_BACK_X && x <= PICK_BACK_X + PICK_BTN_W) {   // BACK
    sfxPlay(SFX_TAP);
    pickOpen = false;
    if (pickTrainer == PICK_LAN) { lanOpen = true; }
    else if (pickTrainer == PICK_BOSS) { bossOpen = true; }
    else { gymRegion = pickRegion; gymOpen = true; }
    return;
  }
  if (y >= PICK_GO_Y && y <= PICK_GO_Y + PICK_BTN_H &&
      x >= PICK_GO_X && x <= PICK_GO_X + PICK_BTN_W) {
    uint8_t cap = pickTrainer == PICK_BOSS ? 3 :
                  ((pickTrainer == PICK_LAN) ? TRAINER_TEAM_MAX : squadCapForRegion(pickRegion, pickTrainer, pickHard));
    if ((pickTrainer == PICK_BOSS && pickChosen() != 3) ||
        (pickTrainer != PICK_BOSS && (pickChosen() == 0 || pickChosen() > cap))) return;
    sfxPlay(SFX_TAP);
    pickOpen = false;
    if (pickTrainer == PICK_BOSS) {
      startBossBattle();
      if (!battleOpen) { pickOpen = true; sfxPlay(SFX_DENY); }
      return;
    }
    if (pickTrainer == PICK_LAN) {
      // The squad is chosen BEFORE the radio comes up, so what gets offered to
      // the peer is what the player picked -- lanOffer() builds lan.mine from
      // squadMask, and the fight is then rebuilt from lan.mine rather than from
      // the party (see startLinkBattle).
      lanOffer(lanWantHost);
      lanOpen = true;
      return;
    }
    startTrainerBattle(pickTrainer, pickHard);
    // If squad construction failed, keep the picker visible instead of falling
    // through to the home screen with every overlay closed.
    if (!battleOpen) { pickOpen = true; sfxPlay(SFX_DENY); }
    return;
  }
  uint8_t seen = 0, drawn = 0;
  for (uint8_t n = 0; n <= pickCandidateLimit(); n++) {
    if (!pickVisible(n)) continue;
    if (seen++ < pickPage * PICK_PER_PAGE) continue;
    if (drawn >= PICK_PER_PAGE) break;
    int cx0 = PICK_X(drawn), cy0 = PICK_Y(drawn);
    drawn++;
    if (x < cx0 || x > cx0 + PICK_CELL_W || y < cy0 || y > cy0 + PICK_CELL_H) continue;
    squadMask ^= (1UL << n);
    sfxPlay(SFX_TAP);
    return;
  }
}

// ---------- LAN battle ----------
// Pairing on a touch-only screen: one device hosts, the other joins, and the
// protocol does the rest. There is no MAC entry because there is no keyboard
// worth typing one on.
void renderLan() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(T(S_LAN), 2), 44);
  gfx->print(T(S_LAN));

  const char *msg = T(S_LAN_PICK);
  switch (lan.state) {
    case LINK_HANDSHAKE:
    case LINK_LISTENING: msg = T(S_LAN_WAIT); break;
    case LINK_SQUADS:    msg = T(S_LAN_WAIT); break;
    case LINK_READY:     msg = T(S_LAN_READY); break;
    case LINK_REFUSED:   msg = T(S_LAN_REFUSED); break;
    case LINK_LOST:      msg = T(S_LAN_GONE); break;
    case LINK_DONE:      msg = lan.youWon ? T(S_BTL_WIN) : T(S_BTL_LOSE); break;
    default: break;
  }
  gfx->setTextColor((lan.state == LINK_REFUSED || lan.state == LINK_LOST)
                      ? UI_BAR_BAD : UI_TRACK);
  uiSetTextSize(1);
  uiSetCursor(CX - uiTextHalfWidth(msg, 1), 76);
  gfx->print(msg);

  if (lan.state == LINK_OFF || lan.state == LINK_REFUSED ||
      lan.state == LINK_LOST) {
    const char *lab[2] = { T(S_LAN_HOST), T(S_LAN_JOIN) };
    for (int i = 0; i < 2; i++) {
      int y = 120 + i * 70;
      gfx->fillRoundRect(90, y, 286, 56, 12, UI_BG_DAY);
      gfx->drawRoundRect(90, y, 286, 56, 12, UI_INK);
      gfx->setTextColor(UI_INK);
      uiSetTextSize(2);
      uiSetCursor(CX - uiTextHalfWidth(lab[i], 2), y + 20);
      gfx->print(lab[i]);
    }
  } else if (lan.state == LINK_READY) {
    char l[40];
    if (lan.peerName[0]) {
      gfx->setTextColor(UI_INK);
      uiSetTextSize(2);
      uiSetCursor(CX - uiTextHalfWidth(lan.peerName, 2), 130);
      gfx->print(lan.peerName);
    }
    snprintf(l, sizeof(l), T(S_LAN_VS), lan.theirsN);
    gfx->setTextColor(UI_INK);
    uiSetTextSize(2);
    uiSetCursor(CX - uiTextHalfWidth(l, 2), 150);
    gfx->print(l);
    gfx->fillRoundRect(120, 220, 226, 56, 12, UI_BAR_OK);
    gfx->drawRoundRect(120, 220, 226, 56, 12, UI_INK);
    gfx->setTextColor(UI_BG_DAY);
    uiSetCursor(CX - uiTextHalfWidth(T(S_FIGHT), 2), 240);
    gfx->print(T(S_FIGHT));
  } else if (lan.state == LINK_DONE) {
    // Both squads are still in hand on both devices, so going again costs one
    // packet -- there is nothing to re-exchange.
    if (lan.peerName[0]) {
      gfx->setTextColor(UI_INK);
      uiSetTextSize(2);
      uiSetCursor(CX - uiTextHalfWidth(lan.peerName, 2), 140);
      gfx->print(lan.peerName);
    }
    gfx->fillRoundRect(120, 220, 226, 56, 12, UI_BG_DAY);
    gfx->drawRoundRect(120, 220, 226, 56, 12, UI_INK);
    gfx->setTextColor(UI_INK);
    uiSetTextSize(2);
    uiSetCursor(CX - uiTextHalfWidth(T(S_LAN_REMATCH), 2), 240);
    gfx->print(T(S_LAN_REMATCH));
  }
  gfx->setTextColor(UI_TRACK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(T(S_BACK), 2), 392);
  gfx->print(T(S_BACK));
  gfx->flush();
}

// Hands our chosen squad to the link, then announces.
// Leaving deliberately: tell the peer so it reports at once instead of sitting
// out the timeout, then free the radio -- it costs real current.
void lanLeave() {
  if (lan.live()) lan.sendBye();
  linkNowEnd();
  lan.state = LINK_OFF;
}

static void lanOffer(bool host) {
  // The squad is built BEFORE the radio is touched. What we advertise has to be
  // exactly what the player just chose in the picker, and that does not depend
  // on whether the radio comes up -- doing it the other way round meant a
  // failed radio skipped the squad entirely and left nothing to inspect.
  lan.begin(host, pet.trainerName);
  snprintf(lan.peerName, sizeof(lan.peerName), "%s", pet.trainerName);
  buildSquad(0, TRAINER_TEAM_MAX, squadMask);
  for (uint8_t i = 0; i < btlSquadN; i++) {
    LinkMon m;
    linkMonFrom(m, btlSquad[i]);
    lan.addMon(m);
  }
  if (!linkNowBegin(&lan)) {          // no radio: say so rather than hanging
    lan.state = LINK_REFUSED;
    return;
  }
  // BOTH sides announce. Which of them ends up hosting is settled by id inside
  // the hello, so the buttons are only a preference -- two players who both tap
  // HOST still get a working fight instead of two authorities, and two who both
  // tap JOIN still get one instead of mutual silence.
  lan.start();
}

void lanTap(int16_t x, int16_t y) {
  if (lan.state == LINK_OFF || lan.state == LINK_REFUSED ||
      lan.state == LINK_LOST) {
    for (int i = 0; i < 2; i++) {
      int by = 120 + i * 70;
      if (x < 90 || x > 376 || y < by || y > by + 56) continue;
      sfxPlay(SFX_TAP);
      lanWantHost = (i == 0);
      lanOpen = false;
      pickTrainer = PICK_LAN;
      pickHard = false;
      pickPage = 0;
      pickSourceTab = 0;
      pickDefault(squadCap(PICK_LAN, false));
      partyOpen = false; boxOpen = false; bossOpen = false; adventureOpen = false; gymOpen = false;
      pickOpen = true;
      return;
    }
  } else if (lan.state == LINK_READY) {
    if (x >= 120 && x <= 346 && y >= 220 && y <= 276) {
      sfxPlay(SFX_TAP);
      lanOpen = false;
      startLinkBattle();
      return;
    }
  } else if (lan.state == LINK_DONE) {
    if (x >= 120 && x <= 346 && y >= 220 && y <= 276) {
      sfxPlay(SFX_TAP);
      lan.sendRematch();      // both sides go back to READY and tap FIGHT
      return;
    }
  }
  if (y > 370) { lanLeave(); lanOpen = false; }   // back
}

// ---------- gym list ----------
void renderGyms() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  // The ladder's own region in the title, since a vertical swipe moves between
  // three of them and "GYMS" alone would not say which you are looking at.
  char title[28];
  snprintf(title, sizeof(title), "%s %s", localizedRegionName(gymRegionDexRegion(gymRegion)),
           T(S_GYMS));
  gfx->setTextColor(UI_INK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(title, 2), 42);
  gfx->print(title);
  // The badge count used to sit here, directly behind the difficulty pill. The
  // region chooser already shows it per region, which is where you are choosing
  // from, so it was both redundant and in the way.
  // difficulty pill: hard caps YOUR team to the leader's size and level, so it
  // is a different ladder with its own badges rather than a damage multiplier
  const char *dif = T(gymHard ? S_HARD : S_EASY);
  int dw = uiTextWidth(dif, 2) + 48;      // wider as well as taller
  if (dw < 120) dw = 120;
  gfx->fillRoundRect(CX - dw / 2, GYMDIF_Y, dw, GYMDIF_H, 12,
                     gymHard ? UI_BAR_BAD : UI_TRACK);
  gfx->setTextColor(gymHard ? UI_BG_DAY : UI_INK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(dif, 2), GYMDIF_Y + 14);
  gfx->print(dif);

  for (int i = 0; i < GYM_ROWS; i++) {
    uint8_t idx = gymPage * GYM_ROWS + i;
    if (idx >= TRAINER_COUNT) break;
    const Trainer &t = TRAINERS[idx];
    int y = GYM_ROW_Y(i);
    bool done = pet.hasBadge(gymRegion, idx, gymHard);
    bool open_ = gymUnlocked(idx, gymHard);
    gfx->fillRoundRect(70, y, 326, 44, 10, done ? UI_TRACK : UI_BG_DAY);
    gfx->drawRoundRect(70, y, 326, 44, 10, open_ ? UI_INK : UI_TRACK);
    gfx->setTextColor(open_ ? UI_INK : UI_TRACK);
    uiSetTextSize(2);
    uiSetCursor(84, y + 8);
    gfx->print(t.name);
    uiSetTextSize(1);
    gfx->setTextColor(UI_TRACK);
    uiSetCursor(84, y + 28);
    gfx->print(open_ ? t.place : T(S_LOCKED));
    // the level of the strongest creature: the honest measure of the wall
    uint8_t top = 0;
    for (int k = 0; k < t.count; k++)
      if (t.team[k].level > top) top = t.team[k].level;
    char lv[16];
    snprintf(lv, sizeof(lv), "Lv.%u x%u", top, t.count);
    gfx->setTextColor(done ? UI_BAR_OK : (open_ ? UI_INK : UI_TRACK));
    uiSetCursor(384 - uiTextWidth(lv, 1), y + 28);
    gfx->print(lv);
    if (done) {
      gfx->setTextColor(UI_BAR_OK);
      uiSetCursor(370, y + 8);
      gfx->print("*");
    }
  }
  uint8_t pages = (TRAINER_COUNT + GYM_ROWS - 1) / GYM_ROWS;
  for (uint8_t i = 0; i < pages; i++) {
    int dx = CX - (pages - 1) * 13 + i * 26;
    if (i == gymPage) gfx->fillCircle(dx, 366, 5, UI_INK);
    else gfx->drawCircle(dx, 366, 4, UI_INK);
  }
  // the other kind of battle lives here too
  gfx->fillRoundRect(148, 380, 170, 32, 9, UI_BG_DAY);
  gfx->drawRoundRect(148, 380, 170, 32, 9, UI_INK);
  gfx->setTextColor(UI_INK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(T(S_LAN), 2), 388);
  gfx->print(T(S_LAN));
  gfx->flush();
}

// The region pill under a waiting egg. Tapping it cycles; Pet::setRegion swaps
// the egg to that region's creature, keeping the rarity it was granted and
// remembering each region's answer so flipping back and forth is not a re-roll.
#define EGGREG_X 133
#define EGGREG_Y 374
#define EGGREG_W 200
#define EGGREG_H 34
// The hit area is BIGGER than the pill, like the BOX button and the battle
// grid, and for the same reason: a 34 px target is under UI_TAP_MIN and a
// finger is not a stylus.
//
// The guard band matters more than the padding. Missing this pill fell through
// to pet.eggTap(), and THREE taps hatch the egg -- so fumbling at the region
// selector hatched the very egg you were trying to re-aim. A near miss now
// does nothing at all, which is the correct answer for a control whose
// neighbour is irreversible.
#define EGGREG_PAD 16
#define EGGREG_GUARD 14

static void drawEggRegion() {
  char l[32];
  if(pet.eggSource>=REGION_COUNT){uint8_t slot=pet.eggSource-REGION_COUNT,ver=slot<5?slot+1:(slot==5?10:slot+5);snprintf(l,sizeof(l),"%s >",digimonDeviceLabel(ver));}
  else snprintf(l, sizeof(l), "%s >", localizedRegionName(pet.region));
  gfx->fillRoundRect(EGGREG_X, EGGREG_Y, EGGREG_W, EGGREG_H, 10, UI_WHITE);
  gfx->drawRoundRect(EGGREG_X, EGGREG_Y, EGGREG_W, EGGREG_H, 10, UI_INK);
  gfx->setTextColor(UI_INK);
  uiSetTextSize(2);
  uiSetCursor(EGGREG_X + (EGGREG_W - uiTextWidth(l, 2)) / 2, EGGREG_Y + 9);
  gfx->print(l);
  gfx->setTextColor(UI_TRACK);
  uiSetTextSize(1);
  uiSetCursor(CX - uiTextHalfWidth(T(S_EGG_REGION), 1), EGGREG_Y + EGGREG_H + 6);
  gfx->print(T(S_EGG_REGION));
}

// True if the tap was on the region pill, so the egg does not also get cracked.
// The egg's region pill: its graphic, and the area that actually accepts a tap.
// Exposed so a test can prove the second is bigger than the first and that a
// near miss does not reach pet.eggTap().
void uiEggPillRect(int *x, int *y, int *w, int *h, bool hitArea) {
  int pad = hitArea ? EGGREG_PAD : 0;
  if (x) *x = EGGREG_X - pad;
  if (y) *y = EGGREG_Y - pad;
  if (w) *w = EGGREG_W + 2 * pad;
  if (h) *h = EGGREG_H + 2 * pad;
}

// 1 = cycled the region, -1 = a near miss that must NOT reach the egg, 0 = not
// ours at all.
static int eggRegionTap(int16_t x, int16_t y) {
  if (!pet.isEgg()) return 0;
  int inset = EGGREG_PAD, guard = EGGREG_PAD + EGGREG_GUARD;
  bool hit = x >= EGGREG_X - inset && x <= EGGREG_X + EGGREG_W + inset &&
             y >= EGGREG_Y - inset && y <= EGGREG_Y + EGGREG_H + inset;
  if (hit) {
    if(pet.eggSource>=REGION_COUNT+10)pet.setRegion(0);
    else if(pet.eggSource>=REGION_COUNT){uint8_t slot=pet.eggSource-REGION_COUNT+1;pet.setDigimonVersion(slot<5?slot+1:(slot==5?10:slot+5));}
    else if(pet.eggSource==REGION_ALL)pet.setDigimonVersion(1);
    else pet.setRegion(nextAvailableRegion(pet.region));
    sfxPlay(SFX_TAP);
    return 1;
  }
  bool near = x >= EGGREG_X - guard && x <= EGGREG_X + EGGREG_W + guard &&
              y >= EGGREG_Y - guard && y <= EGGREG_Y + EGGREG_H + guard;
  if (near) { sfxPlay(SFX_DENY); return -1; }
  return 0;
}

// The region chooser used by the Pokedex and the gym ladder. Each row carries
// its own progress, so the screen answers "where am I up to" as well as "where
// do I want to go".
#define RPICK_X 74
#define RPICK_W 318
#define RPICK_H 62
#define RPICK_Y(i) (108 + (i) * 72)
// The page dots sit BETWEEN the last row and the LAN button, not over it.
// They used to be at y=366, which is inside the LAN button (336..380) and on
// top of its label -- invisible until a region chooser had more than one page,
// so it appeared the moment Sinnoh made GYM_REGIONS 4 and went unnoticed
// because swipe_test drives the paging and never looks at the pixels.
#define RPICK_DOTS_Y 325
#define RPICK_DOT_R 5
// Guards, not decoration: both of these are the collision that shipped, and a
// static_assert is the only check that cannot be forgotten when somebody moves
// a button. The dots must clear the LAN button below and the last row above.
static_assert(RPICK_DOTS_Y + RPICK_DOT_R < LANBTN_Y,
              "the region-chooser page dots overlap the LAN button");
static_assert(RPICK_DOTS_Y - RPICK_DOT_R > RPICK_Y(RPICK_PER_PAGE - 1) + RPICK_H,
              "the region-chooser page dots overlap the last region row");

// How many regions this mode lists. Gyms genuinely only exist for three
// (GYM_REGIONS); the Pokedex and the starter screen list every REAL region,
// which is GAL_REGIONS -- REGION_COUNT minus the ALL pseudo-region.
//
// This used to be GYM_REGIONS for ALL THREE MODES, with the names read out of
// TRAINER_SETS -- a trainer table driving the Pokedex chooser. So the moment
// Sinnoh landed it had no row, while the gallery's vertical swipe cycled it
// happily: built, reachable, and looking absent. That is the exact failure the
// chooser was added to prevent.
uint8_t rpickRegions(uint8_t mode) {
  return (mode == RPICK_FOR_GYMS) ? (uint8_t)GYM_REGIONS :
         (mode == RPICK_FOR_START ? (uint8_t)(GAL_REGIONS+11) : (uint8_t)GAL_REGIONS);
}

uint8_t rpickPageCount(uint8_t mode) {
  uint8_t n = rpickRegions(mode);
  uint8_t p = (uint8_t)((n + RPICK_PER_PAGE - 1) / RPICK_PER_PAGE);
  return p ? p : 1;
}

// The chooser WRAPS rather than closing off the end, unlike the other paged
// screens. It is the root of its own screen, and at first boot there is
// nowhere to go back to at all -- exiting would strand the player before they
// have chosen anything.
// Which chooser is on the panel, or 0xFF for none. THE single answer: the
// swipe handler and swipe_test both ask this rather than each deciding.
uint8_t rpickModeNow() {
  switch (uiCurrentScreen()) {
    case SCR_REGION:  return RPICK_FOR_START;
    case SCR_DEXPICK: return RPICK_FOR_DEX;
    case SCR_GYMPICK: return RPICK_FOR_GYMS;
    default: return 0xFF;
  }
}

static bool rpickSwipe(int dir) {
  uint8_t mode = rpickModeNow();
  if (mode == 0xFF) return false;
  uint8_t pages = rpickPageCount(mode);
  int p = (int)rpickPage + (dir > 0 ? -1 : 1);
  if (p < 0) p = pages - 1;
  if (p >= pages) p = 0;
  rpickPage = (uint8_t)p;
  sfxPlay(SFX_TAP);
  return true;
}

static void renderRegionPick(uint8_t mode) {
  bool forGyms = (mode == RPICK_FOR_GYMS);
  uint8_t nreg = rpickRegions(mode);
  uint8_t pages = rpickPageCount(mode);
  if (rpickPage >= pages) rpickPage = 0;
  uint8_t first = (uint8_t)(rpickPage * RPICK_PER_PAGE);
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  char ttl[40];
  if (mode == RPICK_FOR_START) snprintf(ttl, sizeof(ttl), "%s", T(S_CHOOSE_REGION));
  else if (forGyms) snprintf(ttl, sizeof(ttl), "%s", T(S_GYMS));
  else snprintf(ttl, sizeof(ttl), T(S_POKEDEX_FMT), pet.registeredCount(), regionDexCount(REGION_ALL));
  gfx->setTextColor(UI_INK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(ttl, 2), 48);
  gfx->print(ttl);

  for (uint8_t row = 0; row < RPICK_PER_PAGE; row++) {
    uint8_t i = (uint8_t)(first + row);
    if (i >= nreg) break;
    int y = RPICK_Y(row);
    // The sprite pack is the gate. A region without it is drawn GREYED with a
    // reason rather than dropped from the list: hiding it would say "this
    // region does not exist" when what we mean is "download its pack".
    bool digiChoice = mode==RPICK_FOR_START && i>=GAL_REGIONS;
    bool open = digiChoice || forGyms || regionAvailable(i);
    gfx->fillRoundRect(RPICK_X, y, RPICK_W, RPICK_H, 12, open ? UI_WHITE : UI_BG_DAY);
    gfx->drawRoundRect(RPICK_X, y, RPICK_W, RPICK_H, 12, open ? UI_INK : UI_TRACK);
    char digiNm[32]; if(digiChoice){uint8_t slot=i-GAL_REGIONS,ver=slot<5?slot+1:(slot==5?10:slot+5);snprintf(digiNm,sizeof(digiNm),"%s",digimonDeviceLabel(ver));}
    const char *nm = digiChoice ? digiNm : localizedRegionName(forGyms ? gymRegionDexRegion(i) : i);
    gfx->setTextColor(open ? UI_INK : UI_TRACK);
    uiSetTextSize(3);
    uiSetCursor(RPICK_X + 18, y + 12);
    gfx->print(nm);
    // At first boot there is no subtitle: naming the starter here would give
    // away the next screen, and the counts the other two modes show would all
    // read zero on a new save anyway.
    char sub[28];
    sub[0] = 0;
    if (!open)
      snprintf(sub, sizeof(sub), "%s", T(S_NEED_PACK));
    else if (mode == RPICK_FOR_GYMS)
      snprintf(sub, sizeof(sub), T(S_BADGES_FMT), pet.badgeCountIn(i, gymHard));
    else if (mode == RPICK_FOR_DEX)
      snprintf(sub, sizeof(sub), "%u/%u",
               pet.registeredCountRegion(i),
               (unsigned)regionDexCount(i));
    if (sub[0]) {
      gfx->setTextColor(UI_TRACK);
      uiSetTextSize(2);
      uiSetCursor(RPICK_X + RPICK_W - 18 - uiTextWidth(sub, 2), y + 22);
      gfx->print(sub);
    }
  }
  if (forGyms) {
    gfx->fillRoundRect(LANBTN_X, LANBTN_Y, LANBTN_W, LANBTN_H, 11, UI_BG_DAY);
    gfx->drawRoundRect(LANBTN_X, LANBTN_Y, LANBTN_W, LANBTN_H, 11, UI_INK);
    gfx->setTextColor(UI_INK);
    uiSetTextSize(2);
    uiSetCursor(CX - uiTextHalfWidth(T(S_LAN), 2), LANBTN_Y + 14);
    gfx->print(T(S_LAN));
  }
  if (pages > 1) {                        // dots: which page of regions this is
    int total = pages * 16 - 8;
    for (uint8_t d = 0; d < pages; d++) {
      int cx = CX - total / 2 + d * 16;
      if (d == rpickPage) gfx->fillCircle(cx, RPICK_DOTS_Y, RPICK_DOT_R, UI_INK);
      else gfx->drawCircle(cx, RPICK_DOTS_Y, RPICK_DOT_R, UI_TRACK);
    }
  }
  if (mode != RPICK_FOR_START) {          // first boot has nowhere to go back to
    gfx->setTextColor(UI_TRACK);
    uiSetTextSize(2);
    uiSetCursor(CX - uiTextHalfWidth(T(S_BACK), 2), 392);
    gfx->print(T(S_BACK));
  }
  gfx->flush();
}

// Returns the region tapped, or -1. Takes the mode because the row on screen is
// an offset into the current page, not the region index -- and because a region
// whose sprite pack is missing must not be selectable, which is the whole point
// of the gate. The chooser still SHOWS it, greyed, saying why.
static int regionPickTap(int16_t x, int16_t y, uint8_t mode) {
  if (x < RPICK_X || x > RPICK_X + RPICK_W) return -1;
  uint8_t nreg = rpickRegions(mode);
  for (uint8_t row = 0; row < RPICK_PER_PAGE; row++) {
    uint8_t i = (uint8_t)(rpickPage * RPICK_PER_PAGE + row);
    if (i >= nreg) break;
    if (y >= RPICK_Y(row) && y <= RPICK_Y(row) + RPICK_H) {
      if (mode != RPICK_FOR_GYMS && !(mode==RPICK_FOR_START&&i>=GAL_REGIONS) && !regionAvailable(i)) {
        sfxPlay(SFX_DENY);      // locked: say no out loud rather than do nothing
        return -1;
      }
      return i;
    }
  }
  return -1;
}

// ---------- level-up learn prompt ----------
void renderLearn() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  uint8_t mv = pet.learnOffer();
  char head[80];
  const char *nm = pet.nick[0] ? pet.nick : creatureName(pet.speciesId);
  snprintf(head, sizeof(head), T(S_LEARN_Q), nm);
  gfx->setTextColor(UI_INK);
  uiSetTextSize(1);
  uiSetCursor(CX - uiTextHalfWidth(head, 1), 48);
  gfx->print(head);
  gfx->setTextColor(creatureAccent(pet.speciesId));
  uiSetTextSize(3);
  uiSetCursor(CX - uiTextHalfWidth(localizedMoveName(mv), 3), 66);
  gfx->print(localizedMoveName(mv));

  for (int i = 0; i < MOVE_SLOTS; i++) drawMoveRow(LEARN_ROW_Y(i), pet.moves[i], false, pet.speciesId);

  gfx->fillRoundRect(70, LEARN_SKIP_Y, 326, 44, 12, UI_TRACK);
  gfx->drawRoundRect(70, LEARN_SKIP_Y, 326, 44, 12, UI_INK);
  gfx->setTextColor(UI_INK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(T(S_LEARN_SKIP), 2), LEARN_SKIP_Y + 14);
  gfx->print(T(S_LEARN_SKIP));
  gfx->flush();
}

// pagina 2: medallas con etiqueta descriptiva
void renderCardMedals() {
  int got = 0;
  for (int i = 0; i < MED_COUNT; i++)
    if (pet.hasMedal(1 << i)) got++;
  char head[20];
  snprintf(head, sizeof(head), T(S_MEDALS_FMT), got, MED_COUNT);
  gfx->setTextColor(UI_INK);
  uiSetTextSize(3);
  uiSetCursor(CX - uiTextHalfWidth(head, 3), 48);
  gfx->print(head);

  for (int i = 0; i < MED_COUNT; i++) {
    int x = 28 + (i % 2) * 206, y = 104 + (i / 2) * 54;
    bool g = pet.hasMedal(1 << i);
    gfx->fillRoundRect(x, y, 196, 44, 10, g ? UI_BAR_OK : UI_TRACK);
    if (g) {  // marca de conseguida
      gfx->fillCircle(x + 22, y + 22, 11, UI_BG_DAY);
      gfx->setTextColor(UI_BAR_OK);
      uiSetTextSize(2);
      uiSetCursor(x + 16, y + 13);
      gfx->print("v");
    }
    gfx->setTextColor(g ? UI_BG_DAY : 0x8410);
    uiSetTextSize(2);
    uiSetCursor(x + 44, y + 14);
    gfx->print(medalDesc(i));
  }
}

// pagina 3: progreso (nivel, evolucion, descuidos) — saca a la luz mecanicas
// que antes eran invisibles (cuanto falta para subir/evolucionar y por que)
void renderCardProgress() {
  gfx->setTextColor(UI_INK);
  uiSetTextSize(3);
  uiSetCursor(CX - uiTextHalfWidth(T(S_PROGRESS), 3), 44);
  gfx->print(T(S_PROGRESS));

  // nivel grande
  char lv[10];
  snprintf(lv, sizeof(lv), T(S_LVL_FMT), pet.level());
  uiSetTextSize(5);
  uiSetCursor(CX - uiTextHalfWidth(lv, 5), 86);
  gfx->print(lv);

  // Progress is shown in 1/5-awake-minute units so partial sleeping growth is
  // visible too. Awake adds 5 units/min; sleep adds 1 unit/min.
  uint8_t into = pet.levelClockMinutes() % MINUTES_PER_LEVEL;
  uint8_t sleepRem = pet.sleepLevelRemainder < SLEEP_PROGRESS_QUANTUM
                       ? pet.sleepLevelRemainder : 0;
  const uint8_t finePerLevel = MINUTES_PER_LEVEL * SLEEP_PROGRESS_QUANTUM;
  uint8_t fineInto = into * SLEEP_PROGRESS_QUANTUM + sleepRem;
  int bx = 93, bw = 280, by = 158, bh = 22;
  gfx->fillRoundRect(bx, by, bw, bh, 6, UI_TRACK);
  int fw = (bw - 4) * fineInto / finePerLevel;
  if (fw > 0) gfx->fillRoundRect(bx + 2, by + 2, fw, bh - 4, 5, UI_BAR_OK);
  char nx[26];
  uint8_t remain = pet.sleeping
                     ? (uint8_t)((MINUTES_PER_LEVEL - into) * SLEEP_PROGRESS_QUANTUM - sleepRem)
                     : (uint8_t)(MINUTES_PER_LEVEL - into);
  snprintf(nx, sizeof(nx), T(S_NEXT_LVL_FMT), remain, pet.level() + 1);
  gfx->setTextColor(UI_INK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(nx, 2), by + 32);
  gfx->print(nx);

  // estado de evolucion
  gfx->setTextColor(UI_TRACK);
  uiSetCursor(CX - uiTextHalfWidth(T(S_EVO_LABEL), 2), 230);
  gfx->print(T(S_EVO_LABEL));
  char evoBuf[28];
  const char *evo;
  uint16_t evoCol = UI_INK;
  bool hasEvolution=pet.currentIsDigimon()?digimonHasEvolutionPotential(digimonIndex(pet.speciesId)):dexHasEvolution(pet.speciesId);
  if (!hasEvolution) {
    evo = T(S_FINAL_FORM);
  } else {
    // The SAME sum canEvolveNow() uses -- including the day owed for retiring
    // the previous creature early. A card that left evoPenalty() out would
    // promise an evolution that then does not happen.
    int needed = pet.currentIsDigimon()?digimonEvolutionLevel(digimonIndex(pet.speciesId)):effectiveEvolutionLevel(pet.speciesId, pet.careMistakes, pet.evoPenalty());
    if (pet.level() >= needed) {
      if (pet.currentIsDigimon() ? pet.canEvolveNow() : pet.lowestStat() >= 40) { evo = T(S_EVO_READY); evoCol = UI_BAR_OK; }
      else { evo = T(S_EVO_BLOCKED); evoCol = UI_BAR_BAD; }
    } else {
      snprintf(evoBuf, sizeof(evoBuf), T(S_EVO_IN_FMT), needed - pet.level());
      evo = evoBuf;
    }
  }
  gfx->setTextColor(evoCol);
  uiSetCursor(CX - uiTextHalfWidth(evo, 2), 256);
  gfx->print(evo);

  // Early-retirement delay, said out loud -- otherwise this creature simply
  // evolves later and the player has no way to know why. v3.61.9 keeps this
  // debt bounded so the displayed gate never runs past Lv.100.
  if (pet.evoPenalty()) {
    uiSetTextSize(1);
    gfx->setTextColor(UI_BAR_WARN);
    uiSetCursor(CX - uiTextHalfWidth(T(S_EVO_SLOW), 1), 286);
    gfx->print(T(S_EVO_SLOW));
    uiSetTextSize(2);
  }

  // descuidos (retrasan la evolucion)
  char ms[24];
  snprintf(ms, sizeof(ms), T(S_MISTAKES_FMT), pet.careMistakes);
  gfx->setTextColor(pet.careMistakes > 0 ? UI_BAR_BAD : UI_INK);
  uiSetCursor(CX - uiTextHalfWidth(ms, 2), 312);
  gfx->print(ms);
}

void renderCard() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  if (cardPage == 0) renderCardProfile();
  else if (cardPage == 1) renderCardStats();
  else if (cardPage == 2) renderCardMoves();
  else renderCardProgress();

  if (choiceKind == 1) {
    if (millis() > choiceUntil) choiceKind = 0;
    else {
      drawChoiceDialog();
      gfx->flush();
      return;
    }
  }

  // indicador de paginas + ayuda
  for (int i = 0; i < CARD_PAGES; i++) {
    int dx = CX - (CARD_PAGES - 1) * 13 + i * 26;
    if (i == cardPage) gfx->fillCircle(dx, 374, 5, UI_INK);
    else gfx->drawCircle(dx, 374, 4, UI_INK);
  }
  gfx->setTextColor(UI_TRACK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(T(S_BACK), 2), 398);
  gfx->print(T(S_BACK));
  gfx->flush();
}


// ---------- v3.17 Korean game extras ----------

#define BAG_ROW_X 68
#define BAG_ROW_W 330
#define BAG_ROW_H 48
#define BAG_ROW_Y(i) (126 + (i) * 56)
#define BAG_UI_X_NUDGE 6

static void extraRewardLabel(char *out, size_t n, uint8_t kind, uint8_t id, uint8_t count) {
  if (!out || !n) return;
  if (kind == REWARD_TM) {
    snprintf(out, n, "%s 기술머신 x%u", localizedTypeName(id), count);
  } else if (kind == REWARD_MAP) {
    snprintf(out, n, "보물지도 조각 x%u", count);
  } else {
    snprintf(out, n, "%s x%u", extras.itemNameKo(id), count);
  }
}

static void drawPageNav(uint8_t page, uint8_t pages) {
  // Active page navigator: centered and visually separate from disabled rows.
  const int16_t x = 116 + BAG_UI_X_NUDGE / 2, y = 344, w = 234, h = 40;
  gfx->fillRoundRect(x, y, w, h, 10, UI_WHITE);
  gfx->drawRoundRect(x, y, w, h, 10, RGB565_BLACK);
  gfx->setTextColor(RGB565_BLACK);
  uiSetTextSize(2);
  char p[18];
  snprintf(p, sizeof(p), "<  %u/%u  >", (unsigned)(page + 1), (unsigned)pages);
  uiSetCursor((x + w / 2) - uiTextHalfWidth(p, 2), 355);
  gfx->print(p);
}

static int bagRowBaseX() { return BAG_ROW_X + BAG_UI_X_NUDGE; }
static int bagRowQtyX(const char *cnt) { return bagRowBaseX() + BAG_ROW_W - 18 - uiTextWidth(cnt, 2); }
static int bagRowTextX() { return bagRowBaseX() + 18; }

void renderBag() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  uiSetTextSize(3);
  const char *title = bagTab == 0 ? "육성 아이템" : "기술머신";
  uiSetCursor(CX - uiTextHalfWidth(title, 3) + (gLang == LANG_KO ? 2 : 0), 48);
  gfx->print(title);

  if (tmPendingMove) {
    gfx->setTextColor(UI_INK);
    uiSetTextSize(2);
    char head[72];
    snprintf(head, sizeof(head), "%s을(를) 배운다", localizedMoveName(tmPendingMove));
    uiSetCursor(CX - uiTextHalfWidth(head, 2), 104);
    gfx->print(head);
    gfx->setTextColor(UI_TRACK);
    uiSetTextSize(1);
    uiSetCursor(CX - uiTextHalfWidth("교체할 기술을 선택하세요", 1), 132);
    gfx->print("교체할 기술을 선택하세요");
    for (int i = 0; i < MOVE_SLOTS; i++) {
      int y = 158 + i * 52;
      uint8_t known = pet.moves[i] < MOVE_COUNT ? pet.moves[i] : 0;
      gfx->fillRoundRect(76, y, 314, 44, 10, UI_BG_DAY);
      gfx->drawRoundRect(76, y, 314, 44, 10, typeColor(known ? MOVE_TBL[known].type : T_NORMAL));
      gfx->setTextColor(UI_INK);
      uiSetTextSize(2);
      uiSetCursor(94, y + 12);
      gfx->print(localizedMoveName(known));
    }
    gfx->setTextColor(UI_TRACK);
    uiSetTextSize(2); uiSetCursor(CX - uiTextHalfWidth("취소", 2), 390); gfx->print("취소");
    gfx->flush();
    return;
  }

  if (bagTab == 0) {
    // Earned consumables only. Original unlimited food stays in the quick menu.
    if (bagPage == 0) {
      // Put the four repeat-use IV candies on page 1 so training rewards are
      // immediately reachable when the bag opens.
      const uint8_t ivs[4] = { XITEM_IV_ATK, XITEM_IV_DEF, XITEM_IV_SPE, XITEM_IV_HP };
      for (int i = 0; i < 4; i++) {
        uint8_t id = ivs[i];
        int y = BAG_ROW_Y(i) - 20;
        bool have = extras.itemCount(id) > 0;
        const int rx = bagRowBaseX();
        gfx->fillRoundRect(rx, y, BAG_ROW_W, BAG_ROW_H, 11, have ? UI_WHITE : UI_BG_DAY);
        gfx->drawRoundRect(rx, y, BAG_ROW_W, BAG_ROW_H, 11, have ? UI_INK : UI_TRACK);
        gfx->setTextColor(have ? UI_INK : UI_TRACK);
        uiDrawLeftFit(extras.itemNameKo(id), bagRowTextX(), y + 6, BAG_ROW_W - 86, 2, 1);
        uiDrawLeftFit(extras.itemEffectKo(id), bagRowTextX(), y + 28, BAG_ROW_W - 86, 2, 1);
        char cnt[8]; snprintf(cnt, sizeof(cnt), "x%u", extras.itemCount(id));
        uiSetTextSize(2); uiSetCursor(bagRowQtyX(cnt), y + 13); gfx->print(cnt);
      }
    } else {
      // Page 2 keeps the original next-egg Shiny Charm, utility items and the
      // current-Pokemon Shiny Berry. Page 3 holds ordinary growth items.
      // Existing item IDs 0..10 never move; the new berry is appended.
      const uint8_t utility[4] = { XITEM_SHINY, XITEM_ENERGY, XITEM_GOLD_CROWN, XITEM_SHINY_BERRY };
      const uint8_t growth[4] = { XITEM_ATK, XITEM_DEF, XITEM_SPE, XITEM_VITAL };
      const uint8_t *ids = bagPage == 1 ? utility : growth;
      uint8_t rows = 4;
      for (uint8_t i = 0; i < rows; i++) {
        uint8_t id = ids[i];
        int y = BAG_ROW_Y(i) - 20;
        bool have = extras.itemCount(id) > 0;
        uint16_t edge = (id == XITEM_SHINY || id == XITEM_GOLD_CROWN || id == XITEM_SHINY_BERRY) ? UI_BAR_WARN : UI_BAR_OK;
        const int rx = bagRowBaseX();
        gfx->fillRoundRect(rx, y, BAG_ROW_W, BAG_ROW_H, 11, have ? UI_WHITE : UI_BG_DAY);
        gfx->drawRoundRect(rx, y, BAG_ROW_W, BAG_ROW_H, 11, have ? edge : UI_TRACK);
        gfx->setTextColor(have ? UI_INK : UI_TRACK);
        uiDrawLeftFit(extras.itemNameKo(id), bagRowTextX(), y + 6, BAG_ROW_W - 86, 2, 1);
        uiDrawLeftFit(extras.itemEffectKo(id), bagRowTextX(), y + 28, BAG_ROW_W - 86, 2, 1);
        char cnt[8]; snprintf(cnt, sizeof(cnt), "x%u", extras.itemCount(id));
        uiSetTextSize(2); uiSetCursor(bagRowQtyX(cnt), y + 13); gfx->print(cnt);
      }
      if (bagPage == 1 && extras.shinyBoostArmed()) {
        gfx->setTextColor(UI_BAR_OK); uiSetTextSize(1);
        uiSetCursor(CX - uiTextHalfWidth("다음 알: 반짝부적 적용 중", 1), 82);
        gfx->print("다음 알: 반짝부적 적용 중");
      }
    }
    drawPageNav(bagPage, 3);
  } else {
    uint8_t first = bagPage * 4;
    for (int i = 0; i < 4; i++) {
      uint8_t t = first + i;
      if (t >= TYPE_COUNT) break;
      int y = BAG_ROW_Y(i) - 20;
      bool have = extras.tmCount(t) > 0;
      uint16_t col = typeColor(t);
      const int rx = bagRowBaseX();
      gfx->fillRoundRect(rx, y, BAG_ROW_W, BAG_ROW_H, 11, have ? UI_WHITE : UI_BG_DAY);
      gfx->drawRoundRect(rx, y, BAG_ROW_W, BAG_ROW_H, 11, have ? col : UI_TRACK);
      char nm[64]; snprintf(nm, sizeof(nm), "%s 기술머신", localizedTypeName(t));
      gfx->setTextColor(have ? UI_INK : UI_TRACK);
      uiDrawLeftFit(nm, bagRowTextX(), y + 6, BAG_ROW_W - 86, 2, 1);
      uint8_t mv = extras.bestTmMove(pet, t);
      char tmEffect[80];
      if (mv) snprintf(tmEffect, sizeof(tmEffect), "가르칠 기술: %s", localizedMoveName(mv));
      else snprintf(tmEffect, sizeof(tmEffect), "현재 배울 수 있는 기술 없음");
      uiDrawLeftFit(tmEffect, bagRowTextX(), y + 28, BAG_ROW_W - 86, 2, 1);
      char cnt[8]; snprintf(cnt, sizeof(cnt), "x%u", extras.tmCount(t));
      uiSetTextSize(2); uiSetCursor(bagRowQtyX(cnt), y + 13); gfx->print(cnt);
    }
    drawPageNav(bagPage, 5);
  }

  // Active close control: solid black button, visually distinct from disabled rows.
  gfx->fillRoundRect(CX - 76 + BAG_UI_X_NUDGE / 2, 392, 152, 38, 10, RGB565_BLACK);
  gfx->setTextColor(UI_WHITE); uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth("닫기", 2) + BAG_UI_X_NUDGE / 2, 402); gfx->print("닫기");
  gfx->flush();
}

void bagTap(int16_t x, int16_t y) {
  if (tmPendingMove) {
    for (int i = 0; i < MOVE_SLOTS; i++) {
      int ry = 158 + i * 52;
      if (x < 76 || x > 390 || y < ry || y > ry + 44) continue;
      if (!extras.consumeTm(tmPendingType)) { sfxPlay(SFX_DENY); return; }
      pet.moves[i] = tmPendingMove;
      pet.saveNow();
      tmPendingMove = 0; tmPendingType = T_NONE;
      bagOpen = false;                 // teach once, then return to the pet
      sfxPlay(SFX_MEDAL);
      return;
    }
    if (y > 378) { tmPendingMove = 0; tmPendingType = T_NONE; sfxPlay(SFX_TAP); }
    return;
  }

  for (int i = 0; i < 4; i++) {
    int ry = BAG_ROW_Y(i) - 20;
    const int rx = bagRowBaseX();
    if (x < rx || x > rx + BAG_ROW_W || y < ry || y > ry + BAG_ROW_H) continue;
    if (bagTab == 0) {
      int id = -1;
      if (bagPage == 0) {
        const int ids[4] = { XITEM_IV_ATK, XITEM_IV_DEF, XITEM_IV_SPE, XITEM_IV_HP };
        id = ids[i];
      }
      else if (bagPage == 1) {
        const int ids[4] = { XITEM_SHINY, XITEM_ENERGY, XITEM_GOLD_CROWN, XITEM_SHINY_BERRY };
        id = ids[i];
      } else if (bagPage == 2) {
        const int ids[4] = { XITEM_ATK, XITEM_DEF, XITEM_SPE, XITEM_VITAL };
        id = ids[i];
      }
      if (id < 0 || id >= XITEM_COUNT || !extras.useItem((uint8_t)id, pet)) { sfxPlay(SFX_DENY); return; }
      if (id != XITEM_SHINY) pet.itemEatReaction();
      // IV candies are commonly used several times in a row. Keep page 1 open
      // and refresh its counts/IV effects; only the explicit Close button exits.
      bool ivCandy = id >= XITEM_IV_ATK && id <= XITEM_IV_HP;
      if (!ivCandy) bagOpen = false;
      sfxPlay((id == XITEM_SHINY || id == XITEM_SHINY_BERRY) ? SFX_MEDAL : SFX_EAT);
      return;
    }

    uint8_t type = (uint8_t)(bagPage * 4 + i);
    if (type >= TYPE_COUNT || !extras.tmCount(type) || pet.isEgg()) { sfxPlay(SFX_DENY); return; }
    uint8_t mv = extras.bestTmMove(pet, type);
    if (!mv) { sfxPlay(SFX_DENY); return; }
    int freeSlot = -1;
    for (int k = 0; k < MOVE_SLOTS; k++) if (!pet.moves[k]) { freeSlot = k; break; }
    if (freeSlot >= 0) {
      if (!extras.consumeTm(type)) { sfxPlay(SFX_DENY); return; }
      pet.moves[freeSlot] = mv;
      pet.saveNow();
      bagOpen = false;
      sfxPlay(SFX_MEDAL);
    } else {
      tmPendingType = type; tmPendingMove = mv; sfxPlay(SFX_TAP);
    }
    return;
  }

  if (y >= 344 && y <= 386) {
    uint8_t pages = bagTab == 0 ? 3 : 5;
    if (x < CX) bagPage = (uint8_t)((bagPage + pages - 1) % pages);
    else bagPage = (uint8_t)((bagPage + 1) % pages);
    sfxPlay(SFX_TAP); return;
  }
  if (y > 390) { bagOpen = false; sfxPlay(SFX_TAP); }
}

void renderMissions() {
  extras.ensureDaily(pet);
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  gfx->setTextColor(UI_INK); uiSetTextSize(3);
  uiSetCursor(CX - uiTextHalfWidth("오늘의 미션", 3), 46); gfx->print("오늘의 미션");
  gfx->setTextColor(UI_TRACK); uiSetTextSize(1);
  uiSetCursor(CX - uiTextHalfWidth("완료한 미션을 눌러 보상을 받으세요", 1), 80); gfx->print("완료한 미션을 눌러 보상을 받으세요");

  for (int i = 0; i < 3; i++) {
    int y = 108 + i * 88;
    bool claimed = extras.missionClaimed(i);
    bool done = extras.missionComplete(i);
    uint16_t col = claimed ? UI_TRACK : done ? UI_BAR_OK : UI_WHITE;
    gfx->fillRoundRect(68, y, 330, 76, 13, col);
    gfx->drawRoundRect(68, y, 330, 76, 13, UI_INK);
    gfx->setTextColor(claimed ? UI_WHITE : UI_INK); uiSetTextSize(2);
    uiSetCursor(86, y + 9); gfx->print(extras.missionNameKo(extras.missionKind(i)));
    char pg[20]; snprintf(pg, sizeof(pg), "%u/%u", extras.missionProgress(i), extras.missionGoal(i));
    uiSetCursor(370 - uiTextWidth(pg, 2), y + 9); gfx->print(pg);
    char rw[72]; extraRewardLabel(rw, sizeof(rw), extras.missionRewardKind(i), extras.missionRewardId(i, pet), 1);
    uiSetTextSize(1); uiSetCursor(86, y + 43);
    gfx->print(claimed ? "보상 받음" : "보상: "); if (!claimed) gfx->print(rw);
  }
  gfx->setTextColor(UI_TRACK); uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth("닫기", 2), 402); gfx->print("닫기");
  gfx->flush();
}

void missionTap(int16_t x, int16_t y) {
  for (int i = 0; i < 3; i++) {
    int ry = 108 + i * 88;
    if (x < 68 || x > 398 || y < ry || y > ry + 76) continue;
    if (extras.claimMission(i, pet)) sfxPlay(SFX_MEDAL); else sfxPlay(SFX_DENY);
    return;
  }
  if (y > 384) { missionOpen = false; sfxPlay(SFX_TAP); }
}

void renderRandomEvent() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  gfx->fillRoundRect(62, 92, 342, 280, 20, UI_WHITE);
  gfx->drawRoundRect(62, 92, 342, 280, 20, UI_BAR_WARN);
  gfx->setTextColor(UI_BAR_WARN); uiSetTextSize(3);
  const char *title = extras.eventTitleKo();
  uiSetCursor(CX - uiTextHalfWidth(title, 3), 126); gfx->print(title);
  gfx->setTextColor(UI_INK); uiSetTextSize(2);
  const char *msg = extras.eventTextKo();
  uiSetCursor(CX - uiTextHalfWidth(msg, 2), 184); gfx->print(msg);
  char rw[80]; extraRewardLabel(rw, sizeof(rw), extras.eventRewardKind(), extras.eventRewardId(), extras.eventRewardCount());
  gfx->fillRoundRect(94, 232, 278, 64, 14, UI_BG_DAY);
  gfx->drawRoundRect(94, 232, 278, 64, 14, UI_INK);
  gfx->setTextColor(UI_INK); uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(rw, 2), 254); gfx->print(rw);
  gfx->fillRoundRect(132, 316, 202, 44, 12, UI_BAR_OK);
  gfx->setTextColor(UI_WHITE); uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth("받기", 2), 329); gfx->print("받기");
  gfx->flush();
}


// ---------- v3.23 adventure hub / exploration / boss ----------

static int careSlotDex(uint8_t slot) {
  if (slot >= CARE_SLOT_COUNT) return -1;
  int16_t d = -1;
  if (slot == careSlots.active()) d = pet.speciesId;
  else {
    const CareSnapshot *cs = careSlots.snapshot(slot);
    d = cs ? cs->speciesId : -1;
  }
  return speciesHasArt(d) ? d : -1;
}

static uint8_t careSnapshotLevel(const CareSnapshot &cs, uint32_t nowEpoch) {
  uint32_t prog = cs.levelMinutes == LEVEL_MINUTES_UNSET
                    ? Pet::migrateLegacyLevelMinutes(cs.ageMinutes) : cs.levelMinutes;
  // Estimate inactive-slot growth with the same rates as the real restore path:
  // awake = 1 levelMinute/real minute, sleep = 1 levelMinute/5 real minutes.
  if (!cs.frozen && nowEpoch && cs.parkedEpoch && nowEpoch > cs.parkedEpoch) {
    uint32_t elapsed = (nowEpoch - cs.parkedEpoch) / 60UL;
    if (cs.sleeping) {
      uint8_t rem = cs.sleepLevelRemainder < SLEEP_PROGRESS_QUANTUM
                      ? cs.sleepLevelRemainder : 0;
      prog += (elapsed + rem) / SLEEP_PROGRESS_QUANTUM;
    } else {
      prog += elapsed;
    }
  }
  uint32_t lv = 1 + prog / MINUTES_PER_LEVEL;
  return lv > MAX_LEVEL ? MAX_LEVEL : (uint8_t)lv;
}

static uint16_t careCalcStat(uint8_t base, uint8_t iv, uint8_t lvl, uint8_t tr) {
  return (uint16_t)base + lvl + (uint16_t)iv * lvl / 100 + tr;
}

static bool combatantFromCareSlot(uint8_t slot, Combatant &c, uint32_t nowEpoch) {
  if (slot >= CARE_SLOT_COUNT) return false;
  if (slot == careSlots.active()) {
    if (pet.isEgg() || !creatureHasArt(pet.speciesId)) return false;
    combatantFromPet(c, pet);
    return true;
  }
  const CareSnapshot *ps = careSlots.snapshot(slot);
  if (!ps || !isCreatureId(ps->speciesId) || !creatureHasArt(ps->speciesId)) return false;
  const CareSnapshot &m = *ps;
  uint8_t lvl = careSnapshotLevel(m, nowEpoch);
  uint8_t pers = personalityIdFor(m.speciesId, m.ivAtk, m.ivDef, m.ivSpe, m.ivHp);
  c = Combatant();
  c.dex = m.speciesId;
  c.level = lvl;
  c.maxHp = personalityApply(careCalcStat(creatureBaseHp(m.speciesId), m.ivHp, lvl, (uint8_t)(10 + m.trHp)), pers, PST_HP);
  c.hp = c.maxHp;
  c.base[SI_ATK] = personalityApply(careCalcStat(creatureBaseAtk(m.speciesId), m.ivAtk, lvl, m.trAtk), pers, PST_ATK);
  c.base[SI_DEF] = personalityApply(careCalcStat(creatureBaseDef(m.speciesId), m.ivDef, lvl, m.trDef), pers, PST_DEF);
  c.base[SI_SPA] = c.base[SI_ATK];
  c.base[SI_SPD] = c.base[SI_DEF];
  c.base[SI_SPE] = personalityApply(careCalcStat(creatureBaseSpe(m.speciesId), m.ivSpe, lvl, m.trSpe), pers, PST_SPE);
  memcpy(c.moves, m.moves, sizeof(c.moves));
  c.shiny = m.shiny != 0;
  const char *nm = m.nick[0] ? m.nick : creatureName(m.speciesId);
  snprintf(c.name, sizeof(c.name), "%s", nm);
  return true;
}

static uint8_t bossReadyCount() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < CARE_SLOT_COUNT; i++) if (careSlotDex(i) >= 1) n++;
  return n;
}

static int16_t pickBossDex(uint8_t type) {
  int16_t fallback = 0;
  for (int d = 1; d <= DEX_COUNT; d++) {
    if (DEX_TBL[d].type1 != type || !speciesHasArt(d)) continue;
    uint8_t rg = regionOfDex(d);
    if (rg < REGION_COUNT - 1 && !regionAvailable(rg)) continue;
    fallback = d;
    if (!dexHasEvolution(d) && random(4) == 0) return d;
  }
  return fallback ? fallback : pet.speciesId;
}

void startBossBattle() {
  if (pet.ceremony != CER_NONE) { sfxPlay(SFX_DENY); return; }
  // The boss now uses the same explicit roster selection as gyms. Nothing is
  // withdrawn from or reordered in party/box storage; battle uses copies.
  buildSquad(0, 3, squadMask);
  if (btlSquadN != 3) { sfxPlay(SFX_DENY); return; }
  uint8_t maxLv = 1;
  for (uint8_t i = 0; i < btlSquadN; i++)
    if (btlSquad[i].level > maxLv) maxLv = btlSquad[i].level;
  btlSetCurrentWeather();
  btlApplyWeatherToPlayerSquad();
  btlYou = btlSquad[0];
  btlBossType = extras.bossType(pet);
  int16_t dex = pickBossDex(btlBossType);
  uint8_t lvl = maxLv + 6 > MAX_LEVEL ? MAX_LEVEL : maxLv + 6;
  foeFromSpecies(btlFoe, dex, lvl, 27);
  // One large boss against three companions: sturdier than a normal foe but
  // not three full teams' worth of HP, so switching and type coverage matter.
  btlFoe.maxHp = (uint16_t)min<uint32_t>(65535UL, (uint32_t)btlFoe.maxHp * 210UL / 100UL);
  btlFoe.hp = btlFoe.maxHp;
  for (uint8_t i = 0; i < SI_COUNT; i++)
    btlFoe.base[i] = (uint16_t)min<uint32_t>(65535UL, (uint32_t)btlFoe.base[i] * 112UL / 100UL);
  extras.applyWeatherToCombatant(btlFoe, btlWeather);
  btlTrainer = -1; btlTower = false; btlBoss = true; btlRival = false; btlHard = true;
  btlBossPhase2 = false; btlRivalSpecial = RIVSPEC_NONE;
  btlFoeAt = 0; btlMsgCount = 0; btlOver = false; btlWon = false; btlMenu = 0;
  btlWinUntil = 0; btlSwapWho = -1;
  btlFaintUntil[0] = btlFaintUntil[1] = 0;
  btlEnterUntil[0] = btlEnterUntil[1] = 0;
  btlHpShown[0] = btlYou.maxHp; btlHpShown[1] = btlFoe.maxHp;
  btlSyncSprite(0, btlYou); btlSyncSprite(1, btlFoe);
  audioMusic(MUS_BATTLE);
  btlLungeUntil[0] = btlLungeUntil[1] = 0;
  btlHitUntil[0] = btlHitUntil[1] = 0;
  battleOpen = true; bossOpen = false;
}

void renderAdventure() {
  gfx->fillScreen(RGB565_BLACK); gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  gfx->setTextColor(UI_INK); uiSetTextSize(3);
  uiSetCursor(CX - uiTextHalfWidth("모험", 3), 34); gfx->print("모험");
  char wline[48]; snprintf(wline,sizeof(wline),"현재 날씨: %s",extras.weatherNameKo(extras.weatherId(pet)));
  gfx->setTextColor(UI_TRACK);uiSetTextSize(1);uiSetCursor(CX-uiTextHalfWidth(wline,1),64);gfx->print(wline);
  static const char *const N[5] = { "타입 탐험", "3마리 타입 보스", "배틀 타워", "보물지도", "라이벌 트레이너" };
  for (int i = 0; i < 5; i++) {
    int y = 78 + i * 59;
    uint16_t fill = UI_WHITE;
    if (i == 1) fill = C565(0xff,0xe3,0xd8);
    else if (i == 3) fill = C565(0xff,0xf1,0xc9);
    else if (i == 4) fill = C565(0xe8,0xee,0xff);
    gfx->fillRoundRect(72, y, 322, 49, 13, fill);
    gfx->drawRoundRect(72, y, 322, 49, 13, UI_INK);
    gfx->setTextColor(UI_INK); uiSetTextSize(2); uiSetCursor(90, y + 6); gfx->print(N[i]);
    gfx->setTextColor(UI_TRACK); uiSetTextSize(1); uiSetCursor(90, y + 29);
    if (i == 0) gfx->print("사탕·기술머신·지도 조각 찾기");
    else if (i == 1) gfx->print("육성 슬롯 3마리로 도전");
    else if (i == 2) gfx->print("승리마다 강화 1개 선택");
    else if (i == 3) { char b[40]; snprintf(b,sizeof(b),"지도 조각 %u개 · 3개로 보물찾기",extras.mapPieces()); gfx->print(b); }
    else { char b[48]; snprintf(b,sizeof(b),"%u승 %u패 · 성장 단계 %u",extras.rivalWins(),extras.rivalLosses(),extras.rivalStage(pet)+1); gfx->print(b); }
  }
  gfx->setTextColor(UI_TRACK); uiSetTextSize(2); uiSetCursor(CX-uiTextHalfWidth("닫기",2), 400); gfx->print("닫기");
  gfx->flush();
}

void adventureTap(int16_t x, int16_t y) {
  if (x >= 72 && x <= 394) for (int i = 0; i < 5; i++) {
    int ry = 78 + i * 59;
    if (y < ry || y > ry + 49) continue;
    adventureOpen = false; sfxPlay(SFX_TAP);
    if (i == 0) { exploreOpen = true; explorePage = 0; extras.updateExploration(pet); }
    else if (i == 1) bossOpen = true;
    else if (i == 2) towerOpen = true;
    else if (i == 3) treasureOpen = true;
    else rivalOpen = true;
    return;
  }
  if (y > 384) adventureOpen = false;
}

void renderExplore() {
  extras.updateExploration(pet);
  gfx->fillScreen(RGB565_BLACK); gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  gfx->setTextColor(UI_INK); uiSetTextSize(3);
  uiSetCursor(CX-uiTextHalfWidth("타입 탐험",3), 44); gfx->print("타입 탐험");
  if (extras.explorationReady()) {
    const char *tn = localizedTypeName(extras.explorationType());
    char ttl[48]; snprintf(ttl, sizeof(ttl), "%s 탐험 완료!", tn);
    gfx->setTextColor(typeColor(extras.explorationType())); uiSetTextSize(3);
    uiSetCursor(CX-uiTextHalfWidth(ttl,3), 118); gfx->print(ttl);
    char rw[80]; extraRewardLabel(rw, sizeof(rw), extras.explorationRewardKind(), extras.explorationRewardId(), extras.explorationRewardCount());
    gfx->setTextColor(UI_INK); uiSetTextSize(2); uiSetCursor(CX-uiTextHalfWidth(rw,2), 206); gfx->print(rw);
    gfx->fillRoundRect(104, 274, 258, 66, 16, UI_BAR_OK); gfx->setTextColor(UI_WHITE); uiSetTextSize(3);
    uiSetCursor(CX-uiTextHalfWidth("보상 받기",3), 295); gfx->print("보상 받기");
  } else if (extras.explorationActive()) {
    uint8_t t = extras.explorationType();
    char line[64]; snprintf(line, sizeof(line), "%s 지역 탐험 중", localizedTypeName(t));
    gfx->setTextColor(typeColor(t)); uiSetTextSize(3); uiSetCursor(CX-uiTextHalfWidth(line,3), 126); gfx->print(line);
    char rem[48]; snprintf(rem, sizeof(rem), "남은 시간 약 %u분", extras.explorationMinutesLeft(pet));
    gfx->setTextColor(UI_INK); uiSetTextSize(3); uiSetCursor(CX-uiTextHalfWidth(rem,3), 210); gfx->print(rem);
    char ew[48];snprintf(ew,sizeof(ew),"출발 날씨: %s",extras.weatherNameKo(extras.explorationWeather()));
    gfx->setTextColor(UI_BAR_OK);uiSetTextSize(1);uiSetCursor(CX-uiTextHalfWidth(ew,1),248);gfx->print(ew);
    gfx->setTextColor(UI_TRACK); uiSetTextSize(1);
    uiSetCursor(CX-uiTextHalfWidth("다른 화면을 사용해도 탐험은 계속됩니다",1), 270); gfx->print("다른 화면을 사용해도 탐험은 계속됩니다");
  } else {
    gfx->setTextColor(UI_TRACK); uiSetTextSize(1);
    char wx[72]; uint8_t cw=extras.weatherId(pet); snprintf(wx,sizeof(wx),"날씨 %s · 유리한 타입은 탐험 -3분 / 보상↑",extras.weatherNameKo(cw));
    uiSetCursor(CX-uiTextHalfWidth(wx,1), 76); gfx->print(wx);
    int first = explorePage * 6;
    for (int i = 0; i < 6; i++) {
      int t = first + i; if (t >= TYPE_COUNT) break;
      int y = 96 + i * 46;
      uint16_t col = typeColor((uint8_t)t);
      gfx->fillRoundRect(78, y, 310, 38, 11, lerp565(col, UI_WHITE, 6, 8));
      gfx->drawRoundRect(78, y, 310, 38, 11, col);
      gfx->setTextColor(UI_INK); uiSetTextSize(2); uiSetCursor(98, y + 8); gfx->print(localizedTypeName((uint8_t)t));
      if (!pet.isEgg() && creatureType1(pet.speciesId) == t) { gfx->setTextColor(UI_BAR_OK); uiSetTextSize(1); uiSetCursor(318, y + 12); gfx->print("보너스"); }
    }
    char pg[24]; snprintf(pg,sizeof(pg),"<  %u/3  >", explorePage+1); gfx->setTextColor(UI_TRACK); uiSetTextSize(2);
    uiSetCursor(CX-uiTextHalfWidth(pg,2), 380); gfx->print(pg);
  }
  gfx->setTextColor(UI_TRACK); uiSetTextSize(2); uiSetCursor(CX-uiTextHalfWidth("뒤로",2), 410); gfx->print("뒤로");
  gfx->flush();
}

void exploreTap(int16_t x, int16_t y) {
  extras.updateExploration(pet);
  if (extras.explorationReady()) {
    if (x >= 104 && x <= 362 && y >= 274 && y <= 340) { if (extras.claimExploration(pet)) sfxPlay(SFX_MEDAL); return; }
  } else if (!extras.explorationActive()) {
    if (x >= 78 && x <= 388) for (int i=0;i<6;i++) {
      int t=explorePage*6+i; int ry=96+i*46; if (t>=TYPE_COUNT) break;
      if (y>=ry && y<=ry+38) { if (extras.startExploration((uint8_t)t, pet)) sfxPlay(SFX_TAP); else sfxPlay(SFX_DENY); return; }
    }
    if (y>=358 && y<=398) { explorePage=(uint8_t)((explorePage+1)%3); sfxPlay(SFX_TAP); return; }
  }
  if (y > 396) { exploreOpen=false; adventureOpen=true; }
}

void renderBoss() {
  uint8_t t=extras.bossType(pet);
  uint8_t available=0;
  if (!pet.isEgg() && creatureHasArt(pet.speciesId)) available++;
  for (uint8_t i=0;i<PARTY_SLOTS;i++) if(!party.slots[i].empty()&&speciesHasArt(party.slots[i].dex)) available++;
  for (uint8_t i=0;i<BOX_SLOTS;i++) if(!party.box[i].empty()&&speciesHasArt(party.box[i].dex)) available++;
  gfx->fillScreen(RGB565_BLACK); gfx->fillCircle(CX,CY,231,UI_BG_DAY);
  gfx->setTextColor(UI_INK); uiSetTextSize(3); uiSetCursor(CX-uiTextHalfWidth("3마리 타입 보스",3),44); gfx->print("3마리 타입 보스");
  gfx->fillRoundRect(72,96,322,92,18,lerp565(typeColor(t),UI_WHITE,6,8)); gfx->drawRoundRect(72,96,322,92,18,typeColor(t));
  char bn[48]; snprintf(bn,sizeof(bn),"오늘의 보스: %s",localizedTypeName(t)); gfx->setTextColor(typeColor(t)); uiSetTextSize(3); uiSetCursor(CX-uiTextHalfWidth(bn,3),118); gfx->print(bn);
  gfx->setTextColor(UI_INK); uiSetTextSize(1); const char* done=extras.bossDefeated(t)?"오늘 타입은 격파 기록 있음":"첫 격파는 타입 기술머신 확정"; uiSetCursor(CX-uiTextHalfWidth(done,1),154); gfx->print(done);
  char bp[96];snprintf(bp,sizeof(bp),"HP 50%%↓ 2페이즈: %s",bossPhaseEffectKo(t));gfx->setTextColor(UI_BAR_BAD);uiSetCursor(CX-uiTextHalfWidth(bp,1),174);gfx->print(bp);
  char bw[48];snprintf(bw,sizeof(bw),"현재 날씨: %s",extras.weatherNameKo(extras.weatherId(pet)));gfx->setTextColor(UI_TRACK);uiSetCursor(CX-uiTextHalfWidth(bw,1),192);gfx->print(bw);
  gfx->fillRoundRect(72,214,322,62,12,UI_WHITE);gfx->drawRoundRect(72,214,322,62,12,UI_INK);
  char pool[64];snprintf(pool,sizeof(pool),"현재·파티·박스에서 선택 가능: %u마리",available);gfx->setTextColor(UI_INK);uiSetTextSize(2);uiSetCursor(CX-uiTextHalfWidth(pool,2),226);gfx->print(pool);
  gfx->setTextColor(UI_TRACK);uiSetTextSize(1);uiSetCursor(CX-uiTextHalfWidth("도전을 누른 뒤 출전할 3마리를 고르세요",1),254);gfx->print("도전을 누른 뒤 출전할 3마리를 고르세요");
  gfx->fillRoundRect(104,302,258,62,16,available>=3?UI_BAR_BAD:UI_TRACK);gfx->setTextColor(UI_WHITE);uiSetTextSize(3);const char* go=available>=3?"멤버 선택":"3마리 필요";uiSetCursor(CX-uiTextHalfWidth(go,3),321);gfx->print(go);
  gfx->setTextColor(UI_TRACK);uiSetTextSize(2);uiSetCursor(CX-uiTextHalfWidth("뒤로",2),405);gfx->print("뒤로");gfx->flush();
}

void bossTap(int16_t x,int16_t y){
  if(x>=104&&x<=362&&y>=302&&y<=364){
    uint8_t available=0;
    if(!pet.isEgg()&&creatureHasArt(pet.speciesId))available++;
    for(uint8_t i=0;i<PARTY_SLOTS;i++)if(!party.slots[i].empty()&&speciesHasArt(party.slots[i].dex))available++;
    for(uint8_t i=0;i<BOX_SLOTS;i++)if(!party.box[i].empty()&&speciesHasArt(party.box[i].dex))available++;
    if(available<3){sfxPlay(SFX_DENY);return;}
    pickTrainer=PICK_BOSS;pickPage=0;pickSourceTab=0;pickDefault(3);
    partyOpen=false;boxOpen=false;gymOpen=false;adventureOpen=false;bossOpen=false;pickOpen=true;
    sfxPlay(SFX_TAP);return;
  }
  if(y>390){bossOpen=false;adventureOpen=true;}
}

void startTowerBattle() {
  if (pet.isEgg() || pet.ceremony != CER_NONE) { sfxPlay(SFX_DENY); return; }
  int16_t dex = 0;
  for (int tries = 0; tries < 160; tries++) {
    int16_t d = (int16_t)random(1, DEX_COUNT + 1);
    if (!speciesHasArt(d)) continue;
    uint8_t rg = regionOfDex(d);
    if (rg < REGION_COUNT - 1 && !regionAvailable(rg)) continue;
    dex = d; break;
  }
  if (!dex) dex = pet.speciesId;
  uint16_t lvl = pet.level() + extras.towerStreak() * 2 / 3;
  if (lvl > MAX_LEVEL) lvl = MAX_LEVEL;
  startBattle(dex, (uint8_t)lvl);
  if (!battleOpen) return;
  // render() gives towerOpen priority over battleOpen. Leaving this true meant
  // the battle DID start internally but the tower hub stayed painted on top,
  // while taps were already routed to battleTap(). Close the hub only after a
  // successful battle start so failures still leave the player on the tower.
  towerOpen = false;
  for (uint8_t i = 0; i < btlSquadN; i++) extras.applyTowerBuffs(btlSquad[i]);
  btlYou = btlSquad[btlSquadAt];
  btlHpShown[0] = btlYou.maxHp;
  btlTrainer = -1;
  btlTower = true;
  btlBoss = false;
  btlRival = false;
  btlHard = extras.towerStreak() >= 4;
}

void renderTower() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  gfx->setTextColor(UI_INK); uiSetTextSize(3);
  uiSetCursor(CX - uiTextHalfWidth("배틀 타워", 3), 48); gfx->print("배틀 타워");
  char cur[48], best[48];
  snprintf(cur, sizeof(cur), "현재 %u연승", extras.towerStreak());
  snprintf(best, sizeof(best), "최고 %u연승", extras.towerBest());
  gfx->setTextColor(UI_BAR_BAD); uiSetTextSize(3);
  uiSetCursor(CX - uiTextHalfWidth(cur, 3), 92); gfx->print(cur);
  gfx->setTextColor(UI_INK); uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(best, 2), 128); gfx->print(best);

  if (extras.towerBuffPending()) {
    gfx->setTextColor(UI_BAR_WARN); uiSetTextSize(2);
    uiSetCursor(CX-uiTextHalfWidth("승리 보너스 1개를 선택하세요",2),162); gfx->print("승리 보너스 1개를 선택하세요");
    for (uint8_t i=0;i<3;i++) {
      uint8_t id=extras.towerBuffChoice(i); int y=198+i*58;
      gfx->fillRoundRect(76,y,314,50,13,lerp565(typeColor((id*3)%TYPE_COUNT),UI_WHITE,7,8));
      gfx->drawRoundRect(76,y,314,50,13,UI_INK);
      gfx->setTextColor(UI_INK);uiSetTextSize(2);uiSetCursor(92,y+6);gfx->print(extras.towerBuffNameKo(id));
      char ef[64];snprintf(ef,sizeof(ef),"%s  Lv.%u",extras.towerBuffEffectKo(id),extras.towerBuffLevel(id));
      gfx->setTextColor(UI_TRACK);uiSetTextSize(1);uiSetCursor(92,y+29);gfx->print(ef);
    }
  } else {
    gfx->setTextColor(UI_TRACK); uiSetTextSize(1);
    uiSetCursor(CX-uiTextHalfWidth("연승 중 선택한 강화는 패배할 때까지 유지",1),160); gfx->print("연승 중 선택한 강화는 패배할 때까지 유지");
    static const char *const K[5]={"공","방","속","체","균"};
    for(uint8_t i=0;i<TBUFF_COUNT;i++){
      int x=72+i*65; char b[12];snprintf(b,sizeof(b),"%s%u",K[i],extras.towerBuffLevel(i));
      gfx->fillRoundRect(x,190,56,34,9,UI_WHITE);gfx->drawRoundRect(x,190,56,34,9,UI_TRACK);
      gfx->setTextColor(UI_INK);uiSetTextSize(1);uiSetCursor(x+10,201);gfx->print(b);
    }
    gfx->fillRoundRect(96, 266, 274, 68, 16, UI_BAR_BAD);
    gfx->drawRoundRect(96, 266, 274, 68, 16, UI_INK);
    gfx->setTextColor(UI_WHITE); uiSetTextSize(3);
    const char *go = extras.towerStreak() ? "다음 층 도전" : "도전 시작";
    uiSetCursor(CX - uiTextHalfWidth(go, 3), 288); gfx->print(go);
    gfx->setTextColor(UI_TRACK);uiSetTextSize(1);
    uiSetCursor(CX-uiTextHalfWidth("3연승 아이템 · 5연승 타입 기술머신",1),350);gfx->print("3연승 아이템 · 5연승 타입 기술머신");
  }
  gfx->setTextColor(UI_TRACK); uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth("뒤로", 2), 402); gfx->print("뒤로");
  gfx->flush();
}

void towerTap(int16_t x, int16_t y) {
  if (extras.towerBuffPending()) {
    if (x>=76 && x<=390) for(uint8_t i=0;i<3;i++){int ry=198+i*58;if(y>=ry&&y<=ry+50){if(extras.chooseTowerBuff(i))sfxPlay(SFX_MEDAL);else sfxPlay(SFX_DENY);return;}}
  } else if (x >= 96 && x <= 370 && y >= 266 && y <= 334) {
    sfxPlay(SFX_TAP); startTowerBattle(); return;
  }
  if (y > 382) { towerOpen = false; adventureOpen = true; sfxPlay(SFX_TAP); }
}

// ---------- v3.24 treasure map ----------

void renderTreasure() {
  gfx->fillScreen(RGB565_BLACK); gfx->fillCircle(CX,CY,231,UI_BG_DAY);
  gfx->setTextColor(UI_INK); uiSetTextSize(3); uiSetCursor(CX-uiTextHalfWidth("보물지도",3),44); gfx->print("보물지도");
  char pc[40]; snprintf(pc,sizeof(pc),"지도 조각 %u개",extras.mapPieces());
  gfx->setTextColor(UI_BAR_WARN); uiSetTextSize(3); uiSetCursor(CX-uiTextHalfWidth(pc,3),88); gfx->print(pc);

  if (extras.treasureRewardReady()) {
    gfx->setTextColor(UI_INK); uiSetTextSize(2); uiSetCursor(CX-uiTextHalfWidth("숨겨진 보물을 찾았다!",2),154); gfx->print("숨겨진 보물을 찾았다!");
    char rw[80]; extraRewardLabel(rw,sizeof(rw),extras.treasureRewardKind(),extras.treasureRewardId(),extras.treasureRewardCount());
    gfx->setTextColor(UI_BAR_OK); uiSetTextSize(3); uiSetCursor(CX-uiTextHalfWidth(rw,3),208); gfx->print(rw);
    gfx->fillRoundRect(106,286,254,64,16,UI_BAR_OK); gfx->setTextColor(UI_WHITE); uiSetTextSize(3);
    uiSetCursor(CX-uiTextHalfWidth("보상 받기",3),307); gfx->print("보상 받기");
  } else if (extras.treasureChoosing()) {
    gfx->setTextColor(UI_INK); uiSetTextSize(2); uiSetCursor(CX-uiTextHalfWidth("보물상자 하나를 선택하세요",2),142); gfx->print("보물상자 하나를 선택하세요");
    static const char *const N[3] = { "성장", "반짝", "기술" };
    static const uint16_t C[3] = { C565(0xd9,0xb7,0x74), C565(0xf2,0xb8,0xdd), C565(0xa9,0xc8,0xf0) };
    for (int i=0;i<3;i++) {
      int x=54+i*122;
      gfx->fillRoundRect(x,190,108,110,16,C[i]); gfx->drawRoundRect(x,190,108,110,16,UI_INK);
      gfx->fillRect(x+20,214,68,42,C565(0x8b,0x5d,0x2e)); gfx->fillRect(x+16,206,76,14,C565(0xba,0x82,0x45));
      gfx->fillCircle(x+54,235,5,UI_BAR_WARN);
      gfx->setTextColor(UI_INK); uiSetTextSize(2); uiSetCursor(x+(108-uiTextWidth(N[i],2))/2,266); gfx->print(N[i]);
    }
    gfx->setTextColor(UI_TRACK); uiSetTextSize(1); uiSetCursor(CX-uiTextHalfWidth("성장=사탕 · 반짝=부적 · 기술=TM",1),326); gfx->print("성장=사탕 · 반짝=부적 · 기술=TM");
  } else if (extras.mapComplete()) {
    gfx->setTextColor(UI_INK); uiSetTextSize(2); uiSetCursor(CX-uiTextHalfWidth("조각 3개가 하나의 지도로 이어졌다!",2),160); gfx->print("조각 3개가 하나의 지도로 이어졌다!");
    gfx->fillRoundRect(94,244,278,72,17,UI_BAR_WARN); gfx->drawRoundRect(94,244,278,72,17,UI_INK);
    gfx->setTextColor(UI_INK); uiSetTextSize(3); uiSetCursor(CX-uiTextHalfWidth("보물찾기 시작",3),268); gfx->print("보물찾기 시작");
    gfx->setTextColor(UI_TRACK); uiSetTextSize(1); uiSetCursor(CX-uiTextHalfWidth("시작하면 지도 조각 3개를 사용합니다",1),338); gfx->print("시작하면 지도 조각 3개를 사용합니다");
  } else {
    gfx->setTextColor(UI_INK); uiSetTextSize(2); uiSetCursor(CX-uiTextHalfWidth("지도 조각 3개를 모아야 합니다",2),160); gfx->print("지도 조각 3개를 모아야 합니다");
    gfx->setTextColor(UI_TRACK); uiSetTextSize(1);
    uiSetCursor(CX-uiTextHalfWidth("타입 탐험 · 타입 보스 · 라이벌",1),214); gfx->print("타입 탐험 · 타입 보스 · 라이벌");
    uiSetCursor(CX-uiTextHalfWidth("배틀타워 4연승마다 조각 1개",1),238); gfx->print("배틀타워 4연승마다 조각 1개");
    uiSetCursor(CX-uiTextHalfWidth("랜덤 이벤트에서도 발견할 수 있습니다",1),262); gfx->print("랜덤 이벤트에서도 발견할 수 있습니다");
  }
  gfx->setTextColor(UI_TRACK); uiSetTextSize(2); uiSetCursor(CX-uiTextHalfWidth("뒤로",2),404); gfx->print("뒤로");
  gfx->flush();
}

void treasureTap(int16_t x, int16_t y) {
  if (extras.treasureRewardReady()) {
    if (x>=106&&x<=360&&y>=286&&y<=350) { if(extras.claimTreasure()) sfxPlay(SFX_MEDAL); else sfxPlay(SFX_DENY); return; }
  } else if (extras.treasureChoosing()) {
    for(uint8_t i=0;i<3;i++){int bx=54+i*122;if(x>=bx&&x<=bx+108&&y>=190&&y<=300){if(extras.chooseTreasureChest(i,pet))sfxPlay(SFX_MEDAL);else sfxPlay(SFX_DENY);return;}}
  } else if (extras.mapComplete()) {
    if(x>=94&&x<=372&&y>=244&&y<=316){if(extras.startTreasureHunt())sfxPlay(SFX_TAP);else sfxPlay(SFX_DENY);return;}
  }
  if(y>386){treasureOpen=false;adventureOpen=true;sfxPlay(SFX_TAP);}
}

// ---------- v3.24 persistent rival ----------

static int16_t rivalDexFor(uint8_t slot, uint8_t stage) {
  static const int16_t LINE[3][3] = { {1,2,3}, {4,5,6}, {7,8,9} };
  uint8_t tier = stage >= 6 ? 2 : (stage >= 3 ? 1 : 0);
  if (slot >= 3) slot = 0;
  int16_t d = LINE[slot][tier];
  if (d >= 1 && d <= DEX_COUNT && speciesHasArt(d)) return d;
  // Defensive fallback for sprite packs that omit one species.
  for (int i=0;i<DEX_COUNT;i++) {
    int16_t q = (int16_t)(1 + ((d + i - 1) % DEX_COUNT));
    if (speciesHasArt(q)) return q;
  }
  return pet.speciesId;
}

void startRivalBattle() {
  if (!extras.rivalReady(pet) || pet.ceremony != CER_NONE) { sfxPlay(SFX_DENY); return; }
  if (extras.rivalHasSpecial() && extras.rivalSpecial() != RIVSPEC_TREASURE_RACE) { sfxPlay(SFX_DENY); return; }
  uint32_t e = rtcEpoch(); if(!e)e=pet.lastSeenEpoch;
  btlSquadN=0; btlSquadAt=0; btlPetIn=false;
  uint8_t maxLv=1;
  for(uint8_t i=0;i<CARE_SLOT_COUNT;i++){
    Combatant c; if(!combatantFromCareSlot(i,c,e)) continue;
    btlSquad[btlSquadN++]=c;
    if(i==careSlots.active()) btlPetIn=true;
    if(c.level>maxLv)maxLv=c.level;
    if(btlSquadN>=3)break;
  }
  if(!btlSquadN){sfxPlay(SFX_DENY);return;}
  btlSetCurrentWeather();
  btlApplyWeatherToPlayerSquad();
  btlYou=btlSquad[0];
  uint8_t stage=extras.rivalStage(pet);
  uint8_t foeN=btlSquadN>3?3:btlSquadN;
  btlFoeSquadN=0; btlFoeAt=0;
  uint8_t lvl=(uint8_t)min<int>(MAX_LEVEL,(int)maxLv+1+(int)stage/3);
  uint8_t iv=(uint8_t)min<int>(31,18+stage);
  for(uint8_t i=0;i<foeN;i++){
    foeFromSpecies(btlFoeSquad[btlFoeSquadN],rivalDexFor(i,stage),lvl+(i==2&&lvl<MAX_LEVEL?1:0),iv);
    extras.applyWeatherToCombatant(btlFoeSquad[btlFoeSquadN], btlWeather);
    btlFoeSquadN++;
  }
  btlFoe=btlFoeSquad[0];
  btlTrainer=-1; btlTower=false; btlBoss=false; btlRival=true; btlHard=stage>=2;
  btlBossPhase2=false; btlRivalSpecial=extras.rivalSpecial();
  btlMsgCount=0;btlOver=false;btlWon=false;btlMenu=0;btlWinUntil=0;btlSwapWho=-1;
  btlFaintUntil[0]=btlFaintUntil[1]=0;btlEnterUntil[0]=btlEnterUntil[1]=0;
  btlHpShown[0]=btlYou.maxHp;btlHpShown[1]=btlFoe.maxHp;
  btlSyncSprite(0,btlYou);btlSyncSprite(1,btlFoe);
  audioMusic(MUS_BATTLE);btlLungeUntil[0]=btlLungeUntil[1]=0;btlHitUntil[0]=btlHitUntil[1]=0;
  battleOpen=true;rivalOpen=false;
}

void renderRival() {
  extras.ensureRivalSpecial(pet);
  uint8_t stage=extras.rivalStage(pet); uint8_t ready=bossReadyCount(); uint16_t wait=extras.rivalMinutesLeft(pet);
  gfx->fillScreen(RGB565_BLACK);gfx->fillCircle(CX,CY,231,UI_BG_DAY);
  gfx->setTextColor(UI_INK);uiSetTextSize(3);uiSetCursor(CX-uiTextHalfWidth("라이벌 트레이너",3),40);gfx->print("라이벌 트레이너");
  gfx->fillRoundRect(72,84,322,86,18,C565(0xe8,0xee,0xff));gfx->drawRoundRect(72,84,322,86,18,C565(0x65,0x7c,0xc5));
  gfx->setTextColor(C565(0x45,0x5e,0xb0));uiSetTextSize(3);uiSetCursor(92,98);gfx->print(extras.rivalNameKo());
  char rec[72];snprintf(rec,sizeof(rec),"내 기록 %u승 %u패 · 성장 단계 %u",extras.rivalWins(),extras.rivalLosses(),stage+1);
  gfx->setTextColor(UI_INK);uiSetTextSize(1);uiSetCursor(92,142);gfx->print(rec);
  char wl[48];snprintf(wl,sizeof(wl),"날씨: %s",extras.weatherNameKo(extras.weatherId(pet)));gfx->setTextColor(UI_TRACK);uiSetCursor(314-uiTextWidth(wl,1),158);gfx->print(wl);

  if (extras.rivalHasSpecial() && !wait) {
    uint8_t sp=extras.rivalSpecial();
    gfx->fillRoundRect(70,188,326,150,18,C565(0xff,0xf5,0xd9));gfx->drawRoundRect(70,188,326,150,18,UI_BAR_WARN);
    const char *ttl=extras.rivalSpecialTitleKo();gfx->setTextColor(UI_BAR_WARN);uiSetTextSize(3);uiSetCursor(CX-uiTextHalfWidth(ttl,3),202);gfx->print(ttl);
    const char *tx=extras.rivalSpecialTextKo();gfx->setTextColor(UI_INK);uiSetTextSize(1);uiSetCursor(CX-uiTextHalfWidth(tx,1),246);gfx->print(tx);
    if (sp==RIVSPEC_TRADE) {
      char tr[96];snprintf(tr,sizeof(tr),"내 %s TM 1개 → %s TM 1개",localizedTypeName(extras.rivalTradeWantType()),localizedTypeName(extras.rivalTradeGiveType()));
      gfx->setTextColor(UI_INK);uiSetTextSize(1);uiSetCursor(CX-uiTextHalfWidth(tr,1),270);gfx->print(tr);
      char own[48];snprintf(own,sizeof(own),"보유 %u개",extras.tmCount(extras.rivalTradeWantType()));gfx->setTextColor(UI_TRACK);uiSetCursor(CX-uiTextHalfWidth(own,1),288);gfx->print(own);
    } else if (sp==RIVSPEC_TREASURE_RACE) {
      gfx->setTextColor(UI_BAR_OK);uiSetTextSize(1);uiSetCursor(CX-uiTextHalfWidth("승리하면 지도 조각 +1 추가 보상",1),274);gfx->print("승리하면 지도 조각 +1 추가 보상");
    }
    gfx->fillRoundRect(92,350,190,48,13,sp==RIVSPEC_TRADE&&extras.tmCount(extras.rivalTradeWantType())==0?UI_TRACK:UI_BAR_OK);
    gfx->setTextColor(UI_WHITE);uiSetTextSize(2);const char *ok=sp==RIVSPEC_GIFT?"선물 받기":sp==RIVSPEC_TRADE?"교환하기":"경쟁 시작";uiSetCursor(187-uiTextHalfWidth(ok,2),362);gfx->print(ok);
    gfx->fillRoundRect(292,350,82,48,13,UI_TRACK);gfx->setTextColor(UI_WHITE);uiSetTextSize(1);uiSetCursor(333-uiTextHalfWidth("나중에",1),366);gfx->print("나중에");
  } else {
    gfx->setTextColor(UI_TRACK);uiSetTextSize(1);uiSetCursor(78,188);gfx->print("예상 라이벌 팀");
    for(uint8_t i=0;i<3;i++){
      int x=70+i*110;int d=rivalDexFor(i,stage);uint16_t col=typeColor(DEX_TBL[d].type1);
      gfx->fillRoundRect(x,206,96,58,12,UI_WHITE);gfx->drawRoundRect(x,206,96,58,12,col);
      gfx->setTextColor(RGB565_BLACK);uiSetTextSize(1);uiSetCursor(x+8,218);gfx->print(localizedSpeciesName(d));
      char sl[16];snprintf(sl,sizeof(sl),"내 슬롯 %u",i+1);gfx->setTextColor(UI_TRACK);uiSetCursor(x+8,240);gfx->print(sl);
    }
    bool can=ready>0 && !wait;
    gfx->fillRoundRect(104,294,258,62,16,can?C565(0x5e,0x76,0xc8):UI_TRACK);gfx->setTextColor(UI_WHITE);uiSetTextSize(3);
    char go[48]; if(wait)snprintf(go,sizeof(go),"약 %u분 후 재대결",wait); else snprintf(go,sizeof(go),"라이벌 도전");
    uiSetCursor(CX-uiTextHalfWidth(go,3),313);gfx->print(go);
    gfx->setTextColor(UI_TRACK);uiSetTextSize(1);const char *tip=ready>=3?"육성 슬롯 3마리가 모두 출전합니다":"현재 준비된 육성 슬롯만 출전합니다";
    uiSetCursor(CX-uiTextHalfWidth(tip,1),368);gfx->print(tip);
  }
  gfx->setTextColor(UI_TRACK);uiSetTextSize(2);uiSetCursor(CX-uiTextHalfWidth("뒤로",2),410);gfx->print("뒤로");gfx->flush();
}

void rivalTap(int16_t x,int16_t y){
  extras.ensureRivalSpecial(pet);
  if(extras.rivalHasSpecial() && extras.rivalReady(pet)){
    if(x>=92&&x<=282&&y>=350&&y<=398){
      uint8_t sp=extras.rivalSpecial();
      bool ok=false;
      if(sp==RIVSPEC_GIFT)ok=extras.claimRivalGift();
      else if(sp==RIVSPEC_TRADE)ok=extras.acceptRivalTrade();
      else if(sp==RIVSPEC_TREASURE_RACE){sfxPlay(SFX_TAP);startRivalBattle();return;}
      sfxPlay(ok?SFX_MEDAL:SFX_DENY);return;
    }
    if(x>=292&&x<=374&&y>=350&&y<=398){extras.dismissRivalSpecial();sfxPlay(SFX_TAP);return;}
  } else if(x>=104&&x<=362&&y>=294&&y<=356){
    if(extras.rivalReady(pet)&&bossReadyCount()>0){sfxPlay(SFX_TAP);startRivalBattle();}else sfxPlay(SFX_DENY);return;
  }
  if(y>398){rivalOpen=false;adventureOpen=true;sfxPlay(SFX_TAP);}
}

// ---------- Digital Monster COLOR companion ----------
// DGI1 has three frames. DGI2 adds fifteen 48x48 animation frames while this
// streaming loader keeps RAM use to one frame. Both older DGI1 sizes remain.
static const uint8_t DIGI_FRAME_MAX = 48;
static uint16_t digiPixels[DIGI_FRAME_MAX * DIGI_FRAME_MAX];
static uint8_t digiSpriteW = 0, digiSpriteH = 0;
static int16_t digiLoaded = -1;
static int8_t digiLoadedFrame = -1;
static uint8_t digiFrameCount = 0;
static bool digiAnimated = false;
static uint16_t digiTransparent = 0;
static bool loadDigiSprite(uint16_t id,uint8_t wantedFrame) {
  if (digiLoaded == id && digiLoadedFrame == wantedFrame) return true;
  digiLoaded = -1; digiSpriteW = digiSpriteH = 0;
  char path[32]; snprintf(path,sizeof(path),"/digimon/d%03u.dgi",id);
  File f=SD_MMC.open(path,FILE_READ);
  if(!f)return false;
  uint8_t h[10]; if(f.read(h,sizeof(h))!=(int)sizeof(h)){f.close();return false;}
  bool dgi1=!memcmp(h,"DGI1",4),dgi2=!memcmp(h,"DGI2",4),dgi3=!memcmp(h,"DGI3",4);
  if((!dgi1&&!dgi2&&!dgi3)||(dgi1&&h[7]!=3)||(dgi2&&h[7]!=15)||(dgi3&&h[7]!=3&&h[7]!=15)||h[4]!=(uint8_t)id){f.close();return false;}
  uint8_t w=h[8],hh=h[9];
  if(!((w==16&&hh==16)||(w==48&&hh==48))){f.close();return false;}
  uint8_t frame=wantedFrame%h[7];
  size_t frameBytes=(size_t)w*hh*sizeof(uint16_t);
  size_t expected=10+(size_t)h[7]*frameBytes;
  if(f.size()!=expected||!f.seek(10+(size_t)frame*frameBytes)||f.read((uint8_t*)digiPixels,frameBytes)!=(int)frameBytes){f.close();return false;}
  f.close();digiSpriteW=w;digiSpriteH=hh;digiFrameCount=h[7];digiAnimated=dgi2||dgi3;digiTransparent=dgi3?0x0001:0x0000;digiLoaded=id;digiLoadedFrame=frame;return true;
}
static bool drawDigiFrameCentered(uint16_t spriteId,uint8_t frame,int centerX,
                                  int groundY,int scale,bool flip,bool silhouette){
  if(!loadDigiSprite(spriteId,frame))return false;
  int sc=scale>0?scale:(digiSpriteW==16?8:2);
  int drawW=digiSpriteW*sc,drawH=digiSpriteH*sc,x=centerX-drawW/2,y=groundY-drawH;
  for(int py=0;py<digiSpriteH;py++)for(int px=0;px<digiSpriteW;px++){
    int sx=flip?digiSpriteW-1-px:px;uint16_t c=digiPixels[py*digiSpriteW+sx];
    if(c!=digiTransparent)canvasFillRectFast(x+px*sc,y+py*sc,sc,sc,silhouette?UI_WHITE:c);
  }
  return true;
}
static bool loadBattleDigi(uint8_t who,uint16_t id){
  if(who>1)return false;if(btlDigiFor[who]==id)return true;char path[32];snprintf(path,sizeof(path),"/digimon/d%03u.dgi",id);
  File f=SD_MMC.open(path,FILE_READ);if(!f)return false;uint8_t h[10];if(f.read(h,10)!=10){f.close();return false;}
  bool old=!memcmp(h,"DGI1",4)||!memcmp(h,"DGI2",4),v3=!memcmp(h,"DGI3",4);size_t bytes=48*48*2;
  if((!old&&!v3)||h[4]!=(uint8_t)id||h[8]!=48||h[9]!=48||f.size()!=10+(size_t)h[7]*bytes||f.read((uint8_t*)btlDigiPixels[who],bytes)!=(int)bytes){f.close();return false;}
  f.close();btlDigiFor[who]=id;btlDigiTransparent[who]=v3?0x0001:0;return true;
}
static const char *digiStageName(uint8_t s){static const char*const n[]={"유년기 I","유년기 II","성장기","성숙기","완전체","궁극체"};return s<6?n[s]:"?";}
static uint16_t digiLocalIndex(uint16_t id){uint16_t n=0;if(id>=DIGI_SPECIES_COUNT)return 0;for(uint16_t i=0;i<id;i++)if(DIGI_SPECIES[i].version==DIGI_SPECIES[id].version)n++;return n+1;}
static void digiDexNumber(uint16_t id,char*out,size_t n){
  if(id==DIGI_OMNIMON_ALTER_S)snprintf(out,n,"DF-001");
  else if(id==DIGI_CHAOSMON)snprintf(out,n,"DF-002");
  else if(id==DIGI_MILLENNIUMMON)snprintf(out,n,"DF-003");
  else if(id==DIGI_CHAOSDRAMON)snprintf(out,n,"DX-001");
  else if(DIGI_SPECIES[id].version>=10)snprintf(out,n,"P%u-%02u",DIGI_SPECIES[id].version-10,digiLocalIndex(id));
  else snprintf(out,n,"D%02u-%02u",DIGI_SPECIES[id].version,digiLocalIndex(id));
}
static const char*digiDexRule(uint16_t id){
  if(id==DIGI_OMNIMON_ALTER_S)return "블리츠그레이몬 Lv.55 + 크레스가루루몬 Lv.55";
  if(id==DIGI_CHAOSMON)return "반쵸레오몬 Lv.55 + 다크드라몬 Lv.55";
  if(id==DIGI_MILLENNIUMMON)return "키메라몬 Lv.55 + 무겐드라몬 Lv.55";
  if(id==DIGI_CHAOSDRAMON)return "무겐드라몬 Lv.60 + 공격 80 + 방어 60";
  if(isPendulumFusionSpecies(id)){
    uint8_t v=DIGI_SPECIES[id].version;const char*n=DIGI_SPECIES[id].name;
    if(!strcmp(n,"Omegamon"))return "워그레이몬 + 메탈가루몬 각 Lv.55 (P0/P5 교차 가능)";
    if(!strcmp(n,"Mastemon"))return "엔젤우몬 + 레이디데블몬 각 Lv.55 (버전 교차)";
    if(!strcmp(n,"Proximamon"))return "시리우스몬 + 아크투루스몬 각 Lv.55";
    if(!strcmp(n,"Mitamamon"))return "마린엔젤몬(P2) + 호우오우몬(P4) 각 Lv.55";
    if(!strcmp(n,"Aegisdramon"))return "플레시오몬 + 메탈시드라몬 각 Lv.55";
    if(!strcmp(n,"Chaosdramon"))return "무겐드라몬 + 하이안드로몬 각 Lv.55";
    if(!strcmp(n,"Voltobautamon"))return "묘티스몬 + 피에몬 각 Lv.55";
    if(!strcmp(n,"Cernumon"))return "그리포몬/피노키몬/히드라몬 중 2종 Lv.55";
    if(!strcmp(n,"Tlalocmon"))return "샤벨레오몬 + 엘도라디몬/메탈에테몬 Lv.55";
    (void)v;
  }
  return "레벨과 네 종류 훈련 기록에 따라 진화 분기";
}
static void drawDigiEgg(){
  const int sc=10,ox=CX-40,oy=132;
  static const uint8_t rows[10]={0x18,0x3C,0x7E,0xFF,0xDB,0xA5,0xBD,0x7E,0x3C,0x18};
  for(int py=0;py<10;py++)for(int px=0;px<8;px++)if(rows[py]&(1<<(7-px)))canvasFillRectFast(ox+px*sc,oy+py*sc,sc,sc,(py==4||py==6)?UI_BAR_OK:UI_WHITE);
  gfx->drawRoundRect(ox-4,oy-4,88,108,18,UI_INK);
}
static void drawDigiSprite() {
  uint32_t now=millis(),phase=now%16000;uint8_t frame=0;int16_t drift=0;bool flip=false;
  PetMood mood=pet.mood();
  // Real pet state always wins over the ambient loop. Frame assignments are
  // the ones approved in the preview: joy 1/2, training 1/3, eat/touch 0/2,
  // sleep 11/12, unmet-care discomfort 13/14. Walking uses the neutral pose
  // and actual screen motion/flip instead of pretending a joy frame is a step.
  if(mood==MOOD_SLEEPING)frame=11+(now/850)%2;
  else if(mood==MOOD_EATING)frame=((now/260)&1)?2:0;
  else if(mood==MOOD_SAD)frame=13+(now/650)%2;
  else if((int32_t)(digiActionUntil-now)>0){
    if(digiActionKind==DIGI_ANIM_TRAIN)frame=((now/240)&1)?3:1;
    else frame=((now/260)&1)?2:0;
    drift=((now/130)&1)?0:-3;
  }
  else if(phase<6500)frame=0;
  else if(phase<11500){
    int p=(phase-6500)/45;
    bool movingRight=p<=55;
    // Converted Digital Monster COLOR art faces left in its source frame.
    // Mirror only while travelling right; use the original while travelling
    // left, so the Digimon always looks toward its movement direction.
    flip=movingRight;
    drift=movingRight?p:110-p;
    frame=0;
  }
  else if(phase<14500){frame=((now/300)&1)?2:1;drift=((now/150)&1)?0:-2;}
  else frame=0;
  uint16_t spriteId=pet.currentIsDigimon()?digimonIndex(pet.speciesId):digiPet.speciesId;
  if(!loadDigiSprite(spriteId,frame)){
    gfx->fillRoundRect(153,116,160,142,18,UI_TRACK);gfx->setTextColor(UI_INK);uiDrawCenteredFit("SD 도트 없음",CX,174,150,2,1);return;
  }
  if(!digiAnimated){frame=(now/420)%digiFrameCount;if(!loadDigiSprite(spriteId,frame))return;drift=0;flip=false;}
  int sc=digiSpriteW==16?8:2;
  int drawW=digiSpriteW*sc,drawH=digiSpriteH*sc,x=CX-drawW/2+drift,y=PET_GROUND-drawH;
  for(int py=0;py<digiSpriteH;py++)for(int px=0;px<digiSpriteW;px++){
    int sx=flip?digiSpriteW-1-px:px;uint16_t c=digiPixels[py*digiSpriteW+sx];
    if(c!=digiTransparent)canvasFillRectFast(x+px*sc,y+py*sc,sc,sc,c);
  }
}
void renderDigi(){
  gfx->fillScreen(RGB565_BLACK);gfx->fillCircle(CX,CY,231,0xDFFF);
  gfx->setTextColor(UI_INK);uiDrawCenteredFit("디지털 몬스터 COLOR",CX,30,410,3,2);
  for(int v=1;v<=5;v++){int x=48+(v-1)*76;bool on=digiPet.enabled&&digiPet.version==v;gfx->fillRoundRect(x,72,66,34,9,on?UI_BAR_OK:UI_WHITE);gfx->drawRoundRect(x,72,66,34,9,UI_INK);char b[8];snprintf(b,sizeof(b),"Ver.%d",v);uiDrawCenteredFit(b,x+33,82,60,1,1);}
  if(!digiPet.enabled){gfx->setTextColor(UI_INK);uiDrawCenteredFit("버전을 선택해 디지타마를 받으세요",CX,196,410,2,1);}
  else if(digiPet.egg){
    char title[32];snprintf(title,sizeof(title),"Ver.%u 디지타마",digiPet.version);gfx->setTextColor(UI_INK);uiDrawCenteredFit(title,CX,112,350,2,1);drawDigiEgg();
    char hatch[32];snprintf(hatch,sizeof(hatch),"부화 진행 %u/3",digiPet.eggTaps);uiDrawCenteredFit(hatch,CX,254,300,2,1);uiDrawCenteredFit("알을 터치하세요",CX,282,300,1,1);
    gfx->fillRoundRect(118,390,230,44,12,UI_BAR_OK);gfx->drawRoundRect(118,390,230,44,12,UI_INK);uiDrawCenteredFit("디지몬 도감",CX,402,218,2,1);
  }
  else{
    char title[64];snprintf(title,sizeof(title),"%s  Lv.%u",digimonNameKo(digiPet.speciesId),digiPet.level());gfx->setTextColor(typeColor(digiPet.type1()));uiDrawCenteredFit(title,CX,110,420,2,1);drawDigiSprite();
    char sub[48];snprintf(sub,sizeof(sub),"%s  %s/%s",digiStageName(digiPet.species().stage),typeName(digiPet.type1()),digiPet.type2()==T_NONE?"-":typeName(digiPet.type2()));gfx->setTextColor(UI_INK);uiDrawCenteredFit(sub,CX,264,430,1,1);
    static const char*const lab[]={"공격","방어","스피드","체력"};
    for(int i=0;i<4;i++){int x=38+i*99;gfx->fillRoundRect(x,292,92,62,10,UI_WHITE);gfx->drawRoundRect(x,292,92,62,10,typeColor(digiPet.type1()));char s[24];snprintf(s,sizeof(s),"%s %u",lab[i],digiPet.stat((DigiTrain)i));uiDrawCenteredFit(s,x+46,302,86,1,1);snprintf(s,sizeof(s),"훈련 %u",digiPet.training[i]);uiDrawCenteredFit(s,x+46,326,86,1,1);}
    char mv[96];snprintf(mv,sizeof(mv),"%s / %s / %s / %s",MOVE_TBL[digiPet.moves[0]].name,MOVE_TBL[digiPet.moves[1]].name,MOVE_TBL[digiPet.moves[2]].name,MOVE_TBL[digiPet.moves[3]].name);gfx->setTextColor(UI_INK);uiDrawCenteredFit(mv,CX,364,430,1,1);
    gfx->fillRoundRect(36,390,104,44,12,UI_WHITE);gfx->drawRoundRect(36,390,104,44,12,UI_INK);uiDrawCenteredFit("도감",88,402,94,2,1);
    gfx->fillRoundRect(151,390,265,44,12,digiPet.canEvolve()?UI_BAR_OK:UI_TRACK);gfx->drawRoundRect(151,390,265,44,12,UI_INK);uiDrawCenteredFit(digiPet.canEvolve()?"진화하기":"진화 조건 미달",283,402,250,2,1);
  }
  gfx->setTextColor(UI_INK);uiDrawCenteredFit("닫기",CX,443,100,2,1);gfx->flush();
}
void digiTap(int16_t x,int16_t y){
  if(y>=72&&y<=106){for(int v=1;v<=5;v++){int bx=48+(v-1)*76;if(x>=bx&&x<=bx+66){digiPet.startEgg(v);digiLoaded=-1;digiLoadedFrame=-1;sfxPlay(SFX_TAP);return;}}}
  if(!digiPet.enabled){if(y>426)digiOpen=false;return;}
  if(digiPet.egg){
    if(x>=145&&x<=321&&y>=120&&y<=246){sfxPlay(digiPet.tapEgg()?SFX_MEDAL:SFX_TAP);digiLoaded=-1;digiLoadedFrame=-1;return;}
    if(y>=390&&y<=434){digiDexOpen=true;digiDexPage=0;digiDexDetail=-1;digiOpen=false;sfxPlay(SFX_TAP);return;}
    if(y>434)digiOpen=false;return;
  }
  if(y>=292&&y<=354){int i=(x-38)/99;if(i>=0&&i<4){digiPet.train((DigiTrain)i);digiActionUntil=millis()+1500;sfxPlay(SFX_TAP);return;}}
  if(y>=390&&y<=434&&x<=140){digiDexOpen=true;digiDexPage=0;digiDexDetail=-1;digiOpen=false;sfxPlay(SFX_TAP);return;}
  if(y>=390&&y<=434&&x>=151){sfxPlay(digiPet.evolve()?SFX_MEDAL:SFX_DENY);digiLoaded=-1;digiLoadedFrame=-1;return;}
  if(y>434)digiOpen=false;
}

void renderDigiDex(){
  gfx->fillScreen(RGB565_BLACK);gfx->fillCircle(CX,CY,231,0xDFFF);gfx->setTextColor(UI_INK);
  char head[48];snprintf(head,sizeof(head),"디지몬 도감 %u/%u",pet.digiRegisteredCount(),DIGI_SPECIES_COUNT);uiDrawCenteredFit(head,CX,24,410,3,2);
  if(digiDexDetail>=0&&digiDexDetail<DIGI_SPECIES_COUNT){
    uint16_t id=(uint16_t)digiDexDetail;char no[16];digiDexNumber(id,no,sizeof(no));bool seen=pet.isDigiRegistered(id);
    gfx->fillRoundRect(36,72,394,318,18,UI_WHITE);gfx->drawRoundRect(36,72,394,318,18,UI_INK);
    uiDrawCenteredFit(no,CX,92,350,2,1);uiDrawCenteredFit((seen||isDigiExtraSpecies(id))?digimonNameKo(id):"???",CX,132,350,3,2);
    uiDrawCenteredFit(isDigiFusionSpecies(id)?"융합체":(id==DIGI_CHAOSDRAMON?"상위 진화체":digiStageName(DIGI_SPECIES[id].stage)),CX,180,350,2,1);
    gfx->setTextColor(UI_TRACK);uiDrawCenteredFit("진화 조건",CX,228,330,2,1);gfx->setTextColor(UI_INK);uiDrawCenteredFit(digiDexRule(id),CX,264,350,2,1);
    if(isDigiExtraSpecies(id)){char lv[64];snprintf(lv,sizeof(lv),"현재 최고기록 %u",pet.digiBest[id]);uiDrawCenteredFit(lv,CX,324,330,1,1);}
    uiDrawCenteredFit("목록으로",CX,410,140,2,1);gfx->flush();return;
  }
  const uint8_t per=8;uint8_t pages=(uint8_t)((DIGI_SPECIES_COUNT+per-1)/per);
  for(uint8_t row=0;row<per;row++){
    uint16_t id=(uint16_t)digiDexPage*per+row;if(id>=DIGI_SPECIES_COUNT)break;int y=66+row*43;bool seen=pet.isDigiRegistered(id);
    gfx->fillRoundRect(52,y,362,36,8,seen?UI_WHITE:UI_TRACK);gfx->drawRoundRect(52,y,362,36,8,seen?typeColor(digiPet.type1()):UI_INK);
    char no[16];digiDexNumber(id,no,sizeof(no));uiDrawLeftFit(no,64,y+10,76,1,1);
    uiDrawLeftFit((seen||isDigiExtraSpecies(id))?digimonNameKo(id):"???",145,y+8,158,2,1);uiDrawLeftFit(seen?(isDigiFusionSpecies(id)?"융합체":digiStageName(DIGI_SPECIES[id].stage)):"미등록",310,y+10,94,1,1);
  }
  char page[32];snprintf(page,sizeof(page),"%u/%u  좌우 넘김",digiDexPage+1,pages);uiDrawCenteredFit(page,CX,418,260,1,1);uiDrawCenteredFit("닫기",CX,443,100,2,1);gfx->flush();
}
void digiDexTap(int16_t x,int16_t y){(void)x;if(digiDexDetail>=0){if(y>390){digiDexDetail=-1;sfxPlay(SFX_TAP);}return;}if(y>=66&&y<410){uint8_t row=(y-66)/43;uint16_t id=(uint16_t)digiDexPage*8+row;if(id<DIGI_SPECIES_COUNT){digiDexDetail=id;sfxPlay(SFX_TAP);return;}}if(y>426){digiDexOpen=false;digiOpen=false;sfxPlay(SFX_TAP);}}

// ---------- menu overlay ----------

// Row labels are built fresh each frame because two of them carry live counts.
static void menuRowLabel(int i, char *out, size_t n) {
  switch (i) {
    case 0: snprintf(out, n, "상태·능력치"); break;
    case 1: snprintf(out, n, "도감 %u/%u", pet.registeredCount(), regionDexCount(REGION_ALL)); break;
    case 2: snprintf(out, n, "오늘의 미션"); break;
    case 3: snprintf(out, n, "모험·탐험"); break;
    case 4: snprintf(out, n, "설정·시계"); break;
    case 5: snprintf(out, n, "디지몬 도감"); break;
    case 6: snprintf(out, n, "좋은 이별"); break;
    default: snprintf(out, n, "닫기"); break;
  }
}

void drawMenu() {
  // dim the game behind the panel so the overlay reads as modal, and so it is
  // obvious that tapping the darkened area is a way out
  for (int y = 0; y < 466; y += 2)
    gfx->drawFastHLine(0, y, 466, gNight ? 0x0000 : 0x2104);

  gfx->fillRoundRect(MENU_X, MENU_Y, MENU_W, MENU_H, 18, UI_WHITE);
  gfx->drawRoundRect(MENU_X, MENU_Y, MENU_W, MENU_H, 18, UI_INK);

  for (int i = 0; i < MENU_ROWS; i++) {
    int y = MENU_ROW_Y(i);
    bool close = (i == MENU_ROWS - 1);
    bool dead = (i == 6 && !pet.canRetireNow());   // an egg or a companion
    gfx->fillRoundRect(MENU_X + 18, y, MENU_W - 36, MENU_ROW_H, 12,
                       close || dead ? UI_TRACK : UI_BG_DAY);
    gfx->drawRoundRect(MENU_X + 18, y, MENU_W - 36, MENU_ROW_H, 12, UI_INK);
    char lbl[28];
    menuRowLabel(i, lbl, sizeof(lbl));
    gfx->setTextColor(UI_INK);
    // Menu labels are the primary navigation: prefer size 3 and shrink only
    // when a live count or translation cannot fit the padded row.
    uint8_t mts = uiFitTextSize(lbl, 3, 2, MENU_W - 64);
    uiDrawCenteredFit(lbl, CX, y + (mts == 3 ? 6 : 10),
                      MENU_W - 64, 3, 2);
  }
}

// ---------- 체력 훈련: 심장 고리 타이밍 ----------

static int vitalityRingRadius(uint32_t now) {
  uint32_t t = (now - hpRoundStarted) % 1800UL;
  if (t > 900) t = 1800 - t;
  return 42 + (int)(t * 88UL / 900UL);
}

static void finishVitality() {
  if (hpOverUntil) return;
  extras.beginBatch();
  hpGain = pet.trainVitality(hpScore);
  bool rewardReady = hpRound >= HP_ROUNDS;
  hpIvReward = grantTrainingIvBerry(XITEM_IV_HP, rewardReady, hpIvRewardCount);
  hpShinyBerryReward = grantTrainingShinyBerry(rewardReady);
  extras.endBatch(false);
  queueTrainingPersist();
  sfxPlay(hpShinyBerryReward || hpIvRewardCount > 1 ? SFX_MEDAL : SFX_LEVEL);
  hpOverUntil = millis() + 3500UL;
}

void startVitalityGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  hpOpen = true;
  hpRoundStarted = millis();
  hpFeedbackUntil = hpOverUntil = 0;
  hpRound = hpFeedback = hpGain = 0;
  hpScore = 0;
  hpIvReward = XITEM_COUNT;
  hpIvRewardCount = 0;
  hpShinyBerryReward = false;
}

void leaveVitality() {
  if (hpOpen && !hpOverUntil) finishVitality();
  hpOpen = false;
  digiReact(DIGI_ANIM_TRAIN);
}

void vitalityTap(int16_t x, int16_t y) {
  (void)x; (void)y;
  if (hpOverUntil) { hpOpen = false; digiReact(DIGI_ANIM_TRAIN); return; }
  uint32_t now = millis();
  if (hpFeedbackUntil && (int32_t)(hpFeedbackUntil - now) > 0) return;
  int r = vitalityRingRadius(now);
  int dist = abs(r - 92);
  hpFeedback = dist <= 7 ? 3 : (dist <= 17 ? 2 : (dist <= 30 ? 1 : 4));
  if (hpFeedback < 4) hpScore += hpFeedback;
  sfxPlay(hpFeedback == 3 ? SFX_MEDAL : hpFeedback == 2 ? SFX_LEVEL : hpFeedback == 1 ? SFX_PLAY : SFX_DENY);
  hpRound++;
  hpFeedbackUntil = now + 480UL;
  hpRoundStarted = now;
  if (hpRound >= HP_ROUNDS) finishVitality();
}

void renderVitality() {
  uint32_t now = millis();
  drawGaugeBackdrop(currentSceneType());
  if (hpOverUntil) {
    if (now > hpOverUntil) { hpOpen = false; digiReact(DIGI_ANIM_TRAIN); return; }
    gfx->fillRoundRect(62, 82, 342, 300, 24, UI_WHITE);
    gfx->drawRoundRect(62, 82, 342, 300, 24, UI_INK);
    gfx->setTextColor(UI_INK); uiSetTextSize(3);
    uiSetCursor(CX-uiTextHalfWidth("체력 훈련 종료!",3),108); gfx->print("체력 훈련 종료!");
    char b[72]; snprintf(b,sizeof(b),"점수 %u / 36",(unsigned)hpScore);
    uiSetTextSize(4); uiSetCursor(CX-uiTextHalfWidth(b,4),164); gfx->print(b);
    snprintf(b,sizeof(b),"체력 +%u",(unsigned)hpGain);
    gfx->setTextColor(UI_BAR_OK); uiSetTextSize(3); uiSetCursor(CX-uiTextHalfWidth(b,3),224); gfx->print(b);
    if (hpIvReward < XITEM_COUNT && hpIvRewardCount) {
      snprintf(b,sizeof(b),"훈련 보상: %s x%u",extras.itemNameKo(hpIvReward),(unsigned)hpIvRewardCount);
      uiDrawCenteredFit(b,CX,274,318,2,1);
    }
    if (hpShinyBerryReward) {
      snprintf(b,sizeof(b),"희귀 보상: %s x1",extras.itemNameKo(XITEM_SHINY_BERRY));
      gfx->setTextColor(UI_BAR_WARN); uiDrawCenteredFit(b,CX,304,318,2,1);
    }
    gfx->setTextColor(UI_INK); uiDrawCenteredFit("터치해서 돌아가기",CX,344,300,2,1);
    gfx->flush(); return;
  }
  if (hpFeedbackUntil && (int32_t)(now - hpFeedbackUntil) >= 0) {
    hpFeedbackUntil = 0;
    hpFeedback = 0;
    hpRoundStarted = now;
  }
  int r = vitalityRingRadius(now);
  gfx->setTextColor(UI_INK); uiSetTextSize(3);
  uiSetCursor(CX-uiTextHalfWidth("체력 훈련",3),54); gfx->print("체력 훈련");
  gfx->fillCircle(CX,CY,101,UI_BAR_OK);
  gfx->fillCircle(CX,CY,81,UI_BG_DAY);
  gfx->drawCircle(CX,CY,r,UI_BAR_BAD);
  gfx->drawCircle(CX,CY,r+2,UI_BAR_BAD);
  gfx->fillCircle(CX-20,CY-8,28,UI_BAR_BAD); gfx->fillCircle(CX+20,CY-8,28,UI_BAR_BAD);
  gfx->fillTriangle(CX-47,CY,CX+47,CY,CX,CY+55,UI_BAR_BAD);
  char b[32]; snprintf(b,sizeof(b),"%u/%u  점수 %u",(unsigned)hpRound,(unsigned)HP_ROUNDS,(unsigned)hpScore);
  gfx->setTextColor(UI_INK); uiSetTextSize(2); uiSetCursor(CX-uiTextHalfWidth(b,2),350); gfx->print(b);
  const char *hint = hpFeedback == 3 ? "PERFECT!" : hpFeedback == 2 ? "GOOD!" : hpFeedback == 1 ? "OK!" : hpFeedback == 4 ? "MISS" : "초록 고리에 맞춰 터치!";
  uiDrawCenteredFit(hint,CX,388,360,2,1);
  gfx->flush();
}

// ---------- training submenu (5th icon) ----------

// Bars here show progress toward the IV-capped ceiling, not a raw stat: 100%
// means this individual cannot train the stat any higher, which is the whole
// point of trMaxFor() gating training by IV.
static uint8_t trainPct(uint8_t cur, uint8_t cap) {
  return cap ? (uint8_t)((uint16_t)cur * 100 / cap) : 0;
}

void renderTrain() {
  for (int y = 0; y < 466; y += 2)
    gfx->drawFastHLine(0, y, 466, gNight ? 0x0000 : 0x2104);

  gfx->fillRoundRect(TRAIN_X, TRAIN_Y, TRAIN_W, TRAIN_H, 18, UI_WHITE);
  gfx->drawRoundRect(TRAIN_X, TRAIN_Y, TRAIN_W, TRAIN_H, 18, UI_INK);

  gfx->setTextColor(UI_INK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(T(S_TRAIN), 2), TRAIN_Y + 20);
  gfx->print(T(S_TRAIN));

  const char *lbl[4] = { T(S_TR_ATK), T(S_TR_SPE), T(S_TR_DEF), "체력 훈련" };
  uint8_t cur[4] = { pet.trAtk, pet.trSpe, pet.trDef, pet.trHp };
  uint8_t cap[4] = { pet.trMaxAtk(), pet.trMaxSpe(), pet.trMaxDef(), pet.trMaxHp() };

  for (int i = 0; i < 4; i++) {
    int y = TRAIN_ROW_Y(i);
    bool passive = false;      // every row opens a game now, DEF included
    gfx->fillRoundRect(TRAIN_X + 18, y, TRAIN_W - 36, TRAIN_ROW_H, 12,
                       passive ? UI_TRACK : UI_BG_DAY);
    gfx->drawRoundRect(TRAIN_X + 18, y, TRAIN_W - 36, TRAIN_ROW_H, 12, UI_INK);
    gfx->setTextColor(UI_INK);
    uiSetTextSize(2);
    uiSetCursor(TRAIN_X + 32, y + 10);
    gfx->print(lbl[i]);

    uint8_t pct = trainPct(cur[i], cap[i]);
    int bx = TRAIN_X + 32, bw = TRAIN_W - 64, bh = 12, by = y + 34;
    gfx->fillRoundRect(bx, by, bw, bh, 4, UI_TRACK);
    int fw = (bw - 4) * pct / 100;
    if (fw > 0)
      gfx->fillRoundRect(bx + 2, by + 2, fw, bh - 4, 3, pct >= 100 ? UI_BAR_OK : UI_BAR_WARN);
  }

  gfx->setTextColor(UI_INK);
  uiDrawCenteredFit(T(S_TR_DEF_HINT), CX, TRAIN_Y + TRAIN_H - 25,
                    TRAIN_W - 48, 2, 1);
  gfx->flush();   // without this the panel never updates and the screen freezes
}

// ---------- the box ----------
// Storage past the six that fight. A creature is moved by picking a party slot
// and then a box slot, which swaps them -- so one gesture covers deposit,
// withdraw and exchange rather than needing three.
void renderBox() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  char head[32];
  snprintf(head, sizeof(head), T(S_BOX_FMT), party.boxCount(), BOX_SLOTS);
  gfx->setTextColor(UI_INK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(head, 2), 40);
  gfx->print(head);
  if (boxSwapFrom) {
    const PartyMon &p = party.slots[boxSwapFrom - 1];
    char sub[80];
    snprintf(sub, sizeof(sub), T(S_BOX_SWAP),
             p.empty() ? "-" : (p.nick[0] ? p.nick : creatureName(p.dex)));
    gfx->setTextColor(UI_BAR_WARN);
    uiDrawCenteredFit(sub, CX, 62, 390, 2, 1);
  }
  for (uint8_t i = 0; i < BOX_PER_PAGE; i++) {
    uint8_t idx = boxPage * BOX_PER_PAGE + i;
    if (idx >= BOX_SLOTS) break;
    const PartyMon &m = party.box[idx];
    int x = PARTY_GRID_X + (i % 2) * (PARTY_CELL_W + 10);
    int y = 88 + (i / 2) * (PARTY_CELL_H + 8);
    gfx->fillRoundRect(x, y, PARTY_CELL_W, PARTY_CELL_H, 10,
                       m.empty() ? UI_TRACK : UI_WHITE);
    gfx->drawRoundRect(x, y, PARTY_CELL_W, PARTY_CELL_H, 10, UI_INK);
    if (m.empty()) {
      gfx->setTextColor(0x8410);
      uiDrawCenteredFit(T(S_PARTY_EMPTY), x + PARTY_CELL_W / 2,
                        y + PARTY_CELL_H / 2 - 8, PARTY_CELL_W - 16, 2, 1);
      continue;
    }
    const uint8_t *th = thumbs.get(m.dex);
    if (th) drawThumb(th, x - 14, y - 4, 2, false);
    gfx->setTextColor(UI_INK);
    const char *boxName = m.nick[0] ? m.nick : creatureName(m.dex);
    uiDrawLeftFit(boxName, x + 52, y + 10, PARTY_CELL_W - 60, 2, 1);
    char l[16];
    snprintf(l, sizeof(l), "Lv.%u%s", (unsigned)m.level, m.shiny ? " *" : "");
    uiDrawLeftFit(l, x + 52, y + 34, PARTY_CELL_W - 60, 2, 1);
  }
  uint8_t pages = BOX_SLOTS / BOX_PER_PAGE;
  for (uint8_t i = 0; i < pages; i++) {
    int dx = CX - (pages - 1) * 13 + i * 26;
    if (i == boxPage) gfx->fillCircle(dx, 366, 5, UI_INK);
    else gfx->drawCircle(dx, 366, 4, UI_INK);
  }
  gfx->setTextColor(UI_TRACK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(T(S_BACK), 2), 392);
  gfx->print(T(S_BACK));
  gfx->flush();
}

void boxTap(int16_t x, int16_t y) {
  // The sheet is checked first and is modal in the same way the party's is.
  if (boxDetail) {
    if (releaseConfirm) { monSheetConfirmTap(x, y, true); return; }
    if (monSheetBtn(x, y, true)) {          // TO PARTY
      int free = party.firstFree();
      if (free < 0) { sfxPlay(SFX_DENY); return; }
      party.swapPartyBox((uint8_t)free, boxDetail - 1);
      boxDetail = 0;
      boxSel = 0;
      sfxPlay(SFX_MEDAL);
      return;
    }
    if (monSheetBtn(x, y, false)) {         // RELEASE -- ask first, always
      releaseConfirm = true;
      sfxPlay(SFX_TAP);
      return;
    }
    boxDetail = 0;                          // anywhere else backs out
    releaseConfirm = false;
    sfxPlay(SFX_TAP);
    return;
  }
  for (uint8_t i = 0; i < BOX_PER_PAGE; i++) {
    uint8_t idx = boxPage * BOX_PER_PAGE + i;
    if (idx >= BOX_SLOTS) break;
    int cx0 = PARTY_GRID_X + (i % 2) * (PARTY_CELL_W + 10);
    int cy0 = 88 + (i / 2) * (PARTY_CELL_H + 8);
    if (x < cx0 || x > cx0 + PARTY_CELL_W || y < cy0 || y > cy0 + PARTY_CELL_H) continue;
    if (boxSwapFrom) {           // a party slot is waiting: complete the trade
      if (party.slots[boxSwapFrom - 1].empty() && party.box[idx].empty()) {
        sfxPlay(SFX_DENY);
        return;
      }
      party.swapPartyBox(boxSwapFrom - 1, idx);
      boxSwapFrom = 0;
      boxSel = 0;
      sfxPlay(SFX_MEDAL);
      return;
    }
    // Otherwise open its SHEET. It used to go straight to the party, which is a
    // lot to happen from one tap and left nowhere to put RELEASE; the sheet
    // offers TO PARTY explicitly and shows what you are about to move.
    if (party.box[idx].empty()) { sfxPlay(SFX_DENY); return; }
    if (party.firstFree() < 0) {
      boxSel = idx + 1;          // party is full: go choose who steps out
      boxOpen = false;
      sfxPlay(SFX_TAP);
      return;
    }
    boxDetail = idx + 1;
    releaseConfirm = false;
    sfxPlay(SFX_TAP);
    return;
  }
  boxOpen = false;               // anywhere else backs out
  boxSwapFrom = 0;
  boxDetail = 0;
  releaseConfirm = false;
}

// ---------- party ----------

void drawPartySlot(int i, int x, int y) {
  const PartyMon &m = party.slots[i];
  gfx->fillRoundRect(x, y, PARTY_CELL_W, PARTY_CELL_H, 10,
                     m.empty() ? UI_TRACK : UI_WHITE);
  gfx->drawRoundRect(x, y, PARTY_CELL_W, PARTY_CELL_H, 10, UI_INK);
  if (m.empty()) {
    gfx->setTextColor(0x8410);
    uiSetTextSize(2);
    uiSetCursor(x + (PARTY_CELL_W - uiTextWidth(T(S_PARTY_EMPTY), 2)) / 2,
                   y + PARTY_CELL_H / 2 - 8);
    gfx->print(T(S_PARTY_EMPTY));
    return;
  }
  const uint8_t *th = thumbs.get(m.dex);
  if (th) drawThumb(th, x - 6, y - 3, 1, false);
  const char *nm = m.nick[0] ? m.nick : creatureName(m.dex);
  gfx->setTextColor(RGB565_BLACK);
  uiSetTextSize(1);
  uiSetCursor(x + 62, y + 18);
  gfx->print(nm);
  if (m.shiny) {
    gfx->setTextColor(UI_BAR_WARN);
    uiSetCursor(x + 62 + uiTextWidth(nm, 1) + 3, y + 18);
    gfx->print("*");
  }
  char lv[12];
  snprintf(lv, sizeof(lv), T(S_LVL_FMT), (unsigned)m.level);
  gfx->setTextColor(UI_INK);
  uiSetTextSize(2);
  uiSetCursor(x + 62, y + 36);
  gfx->print(lv);
}

void renderParty() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);

  char head[24];
  snprintf(head, sizeof(head), T(S_PARTY_FMT), party.count());
  gfx->setTextColor(UI_INK);
  uiSetTextSize(3);
  uiSetCursor(CX - uiTextHalfWidth(head, 3), 42);
  gfx->print(head);

  // the box lives behind this button; it also shows how full it is, so the
  // player knows there is anything in there without opening it
  if (!partyPick) {
    char bl[24];
    snprintf(bl, sizeof(bl), T(S_BOX_FMT), party.boxCount(), BOX_SLOTS);
    bool armed = boxSwapFrom != 0;
    gfx->fillRoundRect(BOXBTN_X, BOXBTN_Y, BOXBTN_W, BOXBTN_H, 10,
                       armed ? UI_BAR_WARN : UI_BG_DAY);
    gfx->drawRoundRect(BOXBTN_X, BOXBTN_Y, BOXBTN_W, BOXBTN_H, 10, UI_INK);
    gfx->setTextColor(UI_INK);
    uiSetTextSize(2);
    uiSetCursor(CX - uiTextHalfWidth(bl, 2), BOXBTN_Y + 12);
    gfx->print(bl);
  }

  if (boxSel) {
    const PartyMon &b = party.box[boxSel - 1];
    char sw[80];
    snprintf(sw, sizeof(sw), T(S_BOX_SWAP),
             b.nick[0] ? b.nick : creatureName(b.dex));
    gfx->setTextColor(UI_BAR_WARN);
    uiSetTextSize(1);
    uiSetCursor(CX - uiTextHalfWidth(sw, 1), 72);
    gfx->print(sw);
  }

  // when a newcomer is waiting, say so instead of the usual hint
  if (partyPick) {
    gfx->setTextColor(UI_BAR_BAD);
    uiSetTextSize(1);
    uiSetCursor(CX - uiTextHalfWidth(T(S_PARTY_FULL), 1), 74);
    gfx->print(T(S_PARTY_FULL));
  }

  for (int i = 0; i < PARTY_SLOTS; i++) {
    int x = PARTY_GRID_X + (i % 2) * (PARTY_CELL_W + 10);
    int y = PARTY_GRID_Y + (i / 2) * (PARTY_CELL_H + 8);
    drawPartySlot(i, x, y);
  }

  // exit: an explicit button, always in the same place
  const char *ex = partyPick ? T(S_PARTY_LETGO) : T(S_CLOSE);
  gfx->fillRoundRect(PARTYCLOSE_X, PARTYCLOSE_Y, PARTYCLOSE_W, PARTYCLOSE_H, 12,
                     partyPick ? UI_BAR_BAD : UI_TRACK);
  gfx->drawRoundRect(PARTYCLOSE_X, PARTYCLOSE_Y, PARTYCLOSE_W, PARTYCLOSE_H, 12, UI_INK);
  gfx->setTextColor(partyPick ? UI_WHITE : UI_INK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(ex, 2), PARTYCLOSE_Y + 14);
  gfx->print(ex);
  gfx->flush();
}

// ---------- teclado para renombrar ----------

static const char KB_KEYS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-";  // 28 + DEL + OK = 30
#define KB_COLS 6
#define KB_X 40
#define KB_Y 150
#define KB_W 64
#define KB_H 52

// The keyboard is shared, so it has to be told what it is naming. It used to
// hardcode pet.rename() on commit, which is why a second caller needed this.
void openKeyboardFor(uint8_t target) {
  kbTarget = target;
  kbOpen = true;
  const char *cur = (target == KB_TRAINER) ? pet.trainerName : pet.nick;
  strncpy(nameBuf, cur, sizeof(nameBuf) - 1);
  nameBuf[sizeof(nameBuf) - 1] = 0;
  nameLen = strlen(nameBuf);
}
void openKeyboard() { openKeyboardFor(KB_PET); }

void renderKeyboard() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(T(S_NAME), 2), 56);
  gfx->print(T(S_NAME));
  // buffer actual
  gfx->fillRoundRect(83, 84, 300, 40, 8, UI_WHITE);
  gfx->drawRoundRect(83, 84, 300, 40, 8, UI_INK);
  uiSetTextSize(3);
  uiSetCursor(95, 94);
  gfx->print(nameLen ? nameBuf : "_");

  for (int i = 0; i < 30; i++) {
    int x = KB_X + (i % KB_COLS) * KB_W, y = KB_Y + (i / KB_COLS) * KB_H;
    bool special = (i >= 28);
    gfx->fillRoundRect(x, y, KB_W - 6, KB_H - 6, 6, special ? UI_BAR_WARN : UI_WHITE);
    gfx->drawRoundRect(x, y, KB_W - 6, KB_H - 6, 6, UI_INK);
    gfx->setTextColor(UI_INK);
    uiSetTextSize(2);
    if (i < 28) {
      uiSetCursor(x + KB_W / 2 - 9, y + KB_H / 2 - 10);
      gfx->print(KB_KEYS[i]);
    } else {
      const char *lab = (i == 28) ? "<-" : "OK";
      uiSetCursor(x + KB_W / 2 - 15, y + KB_H / 2 - 10);
      gfx->print(lab);
    }
  }
  gfx->flush();
}

void keyboardTap(int16_t x, int16_t y) {
  int col = (x - KB_X) / KB_W, row = (y - KB_Y) / KB_H;
  if (col < 0 || col >= KB_COLS || row < 0 || row >= 5) return;
  int i = row * KB_COLS + col;
  if (i >= 30) return;
  if (i == 28) {  // borrar
    if (nameLen) nameBuf[--nameLen] = 0;
  } else if (i == 29) {  // OK
    if (kbTarget == KB_TRAINER) pet.renameTrainer(nameBuf);
    else pet.rename(nameBuf);
    kbOpen = false;
  } else if (nameLen < sizeof(nameBuf) - 1) {
    nameBuf[nameLen++] = KB_KEYS[i];
    nameBuf[nameLen] = 0;
  }
}

// ---------- galeria pokedex ----------

#define GAL_X 73
#define GAL_Y 84
#define GAL_CELL 80
// v3.57.3 Pokedex Search button in detail view.
#define HUNT_BTN_X 123
#define HUNT_BTN_Y 334
#define HUNT_BTN_W 220
#define HUNT_BTN_H 34

// dibuja una miniatura centrada en su celda; sil=true la pinta en tinta
void drawThumb(const uint8_t *b, int x, int y, int s, bool sil) {
  uint8_t w = b[0], h = b[1], n = b[2];
  const uint8_t *pal = b + 3;
  const uint8_t *d = pal + n * 2;
  int ox = x + (GAL_CELL - w * s) / 2;
  int oy = y + (GAL_CELL - h * s) / 2;
  for (int r = 0; r < h; r++) {
    const uint8_t *row = d + r * w;
    int c = 0;
    while (c < w) {
      uint8_t idx = row[c];
      int run = 1;
      while (c + run < w && row[c + run] == idx) run++;
      if (idx != 0xFF) {
        uint16_t col = sil ? INK_K : (uint16_t)(pal[idx * 2] | (pal[idx * 2 + 1] << 8));
        gfx->fillRect(ox + c * s, oy + r * s, run * s, s, col);
      }
      c += run;
    }
  }
}

void renderGallery() {
  if (galleryDetail) {  // vista detalle: se redibuja siempre (animada)
    gfx->fillScreen(RGB565_BLACK);
    gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
    const DexEntry &d = DEX_TBL[galleryDetail];
    bool reg = pet.isRegistered(galleryDetail);
    char head[48];
    snprintf(head, sizeof(head), "N.%03d %s%s", galleryDetail,
             pet.isShinyRegistered(galleryDetail) ? "*" : "",
             reg ? localizedSpeciesName(galleryDetail) : "???");
    gfx->setTextColor(RGB565_BLACK);
    int glen = (int)utf8GlyphCount(head);
    int gts = (glen <= 13) ? 3 : 2;  // auto-encoge nombres largos (no caben a t3)
    uiSetTextSize(gts);
    uiSetCursor(CX - uiTextHalfWidth(head, gts), gts == 3 ? 56 : 60);
    gfx->print(head);
    if (galleryPmd.loaded) {
      // animado y a color si esta registrado; silueta estatica si no (estilo "?")
      drawPmdActM(galleryPmd, PMD_IDLE, CX, 300, reg ? millis() : 0, true, !reg, 6);
    } else {
      const uint8_t *t = thumbs.get(galleryDetail);
      if (t) drawThumb(t, CX - GAL_CELL, 135, 4, !reg);
    }
    // Pokedex Search: selectable even for an unseen silhouette, so it can
    // actually help fill the dex. Unavailable-art entries stay disabled.
    bool canHunt = pet.huntCanTarget(galleryDetail);
    bool huntingThis = pet.huntTargetDex() == galleryDetail;
    uint16_t hb = huntingThis ? C565(0xff, 0xe5, 0x92) : UI_WHITE;
    uint16_t he = (canHunt || huntingThis) ? UI_INK : UI_TRACK;
    gfx->fillRoundRect(HUNT_BTN_X, HUNT_BTN_Y, HUNT_BTN_W, HUNT_BTN_H, 10, hb);
    gfx->drawRoundRect(HUNT_BTN_X, HUNT_BTN_Y, HUNT_BTN_W, HUNT_BTN_H, 10, he);
    char hs[40];
    if (huntingThis && !canHunt) snprintf(hs, sizeof(hs), "찾기 해제 - 지역팩 확인");
    else if (!canHunt) snprintf(hs, sizeof(hs), "탐색 불가");
    else if (huntingThis && pet.huntMisses() >= 5) snprintf(hs, sizeof(hs), "찾는 중 - 다음 확정");
    else if (huntingThis) snprintf(hs, sizeof(hs), "찾는 중 %u/6 - 누르면 해제", pet.huntMisses());
    else snprintf(hs, sizeof(hs), pet.huntTargetDex() ? "찾기 대상으로 변경" : "찾기 대상으로 지정");
    gfx->setTextColor(he);
    uiSetTextSize(1);
    uiSetCursor(CX - uiTextHalfWidth(hs, 1), HUNT_BTN_Y + 12);
    gfx->print(hs);

    gfx->setTextColor(UI_INK);
    uiSetTextSize(2);
    char rs[48];
    if (reg) snprintf(rs, sizeof(rs), "연구도 Lv.%u/5  (%u/15)", extras.researchLevel(galleryDetail), extras.researchScore(galleryDetail));
    else snprintf(rs, sizeof(rs), "연구도 ???");
    uiSetCursor(CX - uiTextHalfWidth(rs, 2), 378);
    gfx->print(rs);
    gfx->setTextColor(UI_TRACK);
    uiSetCursor(CX - uiTextHalfWidth(T(S_DETAIL_BACK), 2), 418);
    gfx->print(T(S_DETAIL_BACK));
    // First frame (or any hunt-state change) sends the complete detail page.
    // Subsequent registered-Pokemon frames only transfer the animated sprite
    // band; header/buttons/research text are static. An unseen silhouette is
    // completely static, so after its first full present no panel traffic is
    // needed until the user taps.
    if (gGalleryPanelDex != galleryDetail) {
      gfx->flush();
      gGalleryPanelDex = galleryDetail;
    } else if (reg && galleryPmd.loaded) {
      canvasPresentBand(32, 300);
    }
    return;
  }

  if (!galleryDirty) return;  // la rejilla es estatica
  galleryDirty = false;
  gGalleryPanelDex = -1;

  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  // the region's own name and its own tally: "how much of Johto have I seen"
  // is the question you are actually asking here
  char head[32];
  const RegionInfo &grg = REGIONS[galleryRegion % GAL_REGIONS];
  snprintf(head, sizeof(head), "%s %u/%u", localizedRegionName(galleryRegion % GAL_REGIONS),
           pet.registeredCountRegion(galleryRegion % GAL_REGIONS), (unsigned)GAL_SPAN);
  gfx->setTextColor(UI_INK);
  uiSetTextSize(3);
  uiSetCursor(CX - uiTextHalfWidth(head, 3), 36);
  gfx->print(head);

  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      uint16_t ord = (uint16_t)galleryPage * GAL_PER_PAGE + r * 4 + c;
      int16_t dex = regionDexAt(galleryRegion % GAL_REGIONS, ord);
      if (dex < 1) break;
      int x = GAL_X + c * GAL_CELL, y = GAL_Y + r * GAL_CELL;
      const uint8_t *t = thumbs.get(dex);
      if (t) {
        drawThumb(t, x, y, 2, !pet.isRegistered(dex));
        if (pet.isShinyRegistered(dex)) {
          gfx->setTextColor(UI_BAR_WARN);
          uiSetTextSize(2);
          uiSetCursor(x + 62, y + 4);
          gfx->print("*");
        }
      } else {
        char num[6];
        snprintf(num, sizeof(num), "%d", dex);
        gfx->setTextColor(UI_TRACK);
        uiSetTextSize(2);
        uiSetCursor(x + 24, y + 32);
        gfx->print(num);
      }
    }
  }
  // A page number, not a row of dots. 25 dots do not fit across the bottom of
  // a round panel -- the chord at that height is only ~228 px -- and counting
  // them to find where you are is worse than reading the number.
  char pg[12];
  snprintf(pg, sizeof(pg), "%d/%d", galleryPage + 1, (int)GAL_PAGES);
  gfx->setTextColor(UI_TRACK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(pg, 2), 428);
  gfx->print(pg);
  gfx->flush();
}

void galleryTap(int16_t x, int16_t y) {
  if (galleryDetail) {
    if (x >= HUNT_BTN_X && x <= HUNT_BTN_X + HUNT_BTN_W &&
        y >= HUNT_BTN_Y && y <= HUNT_BTN_Y + HUNT_BTN_H) {
      if (pet.huntTargetDex() == galleryDetail || pet.huntCanTarget(galleryDetail)) {
        pet.toggleHuntTarget(galleryDetail);
        gGalleryPanelDex = -1;
        sfxPlay(SFX_TAP);
      } else {
        sfxPlay(SFX_DENY);
      }
      return;
    }
    // Any other tap keeps the original detail-view behavior: back to grid.
    galleryDetail = 0;
    galleryPmd.unload();
    galleryDirty = true;
    return;
  }
  if (y < 72) {  // tocar la cabecera = salir
    galleryOpen = false;
    galleryPmd.unload();
    return;
  }
  int c = (x - GAL_X) / GAL_CELL, r = (y - GAL_Y) / GAL_CELL;
  if (c < 0 || c > 3 || r < 0 || r > 3) return;
  uint16_t ord = (uint16_t)galleryPage * GAL_PER_PAGE + r * 4 + c;
  int16_t dex = regionDexAt(galleryRegion % GAL_REGIONS, ord);
  if (dex < 1 || dex > DEX_COUNT) return;
  galleryDetail = dex;
  galleryPmd.load(dex, pet.isShinyRegistered(dex));
}

void drawBattery() {
  int pc = batPercent();
  if (pc < 0) return;  // no battery connected
  uint16_t mv = batVoltageMv();
  bool charging = batCharging();

  // v3.57.2: show both a stable percentage and the real cell voltage.  The
  // icon/percentage group is centered in the top chord of the round display.
  int w = 24, h = 11;
  int x = CX - 32, y = 10;
  uint16_t col = charging ? UI_BAR_OK
                 : (pc >= 35) ? inkColor()
                 : (pc >= 15) ? UI_BAR_WARN
                              : UI_BAR_BAD;

  gfx->drawRoundRect(x, y, w, h, 2, col);
  gfx->fillRect(x + w, y + 3, 3, 5, col);
  int fw = (w - 4) * pc / 100;
  if (fw > 0) gfx->fillRect(x + 2, y + 2, fw, h - 4, col);

  if (charging) {
    uint16_t bolt = C565(0xff, 0xd9, 0x4a);
    int bx = x + w / 2;
    gfx->fillTriangle(bx + 3, y + 1, bx - 4, y + 6, bx + 1, y + 6, bolt);
    gfx->fillTriangle(bx - 1, y + 5, bx + 4, y + 5, bx - 3, y + 10, bolt);
  }

  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", pc);
  gfx->setTextColor(col);
  uiSetTextSize(1);
  uiSetCursor(x + w + 8, y + 1);
  gfx->print(pct);

  if (mv) {
    char volts[10];
    snprintf(volts, sizeof(volts), "%u.%02uV", mv / 1000U, (mv % 1000U) / 10U);
    gfx->setTextColor(inkColor());
    uiSetTextSize(1);
    uiSetCursor(CX - uiTextHalfWidth(volts, 1), 26);
    gfx->print(volts);
  }
}

void drawHeader(const char *name, uint16_t nameColor, const char *msg) {
  drawBattery();

  // v3.56: remove the capsule/background behind the Pokemon name entirely.
  // Keep the simulator-tuned text position from v3.55 and draw only the text.
  constexpr int HEADER_NAME_Y_NUDGE = 5;
  constexpr int HEADER_TEXT_LIFT = -2;

  gfx->setTextColor(nameColor);
  uiDrawCenteredFit(name, CX, 52 + HEADER_NAME_Y_NUDGE - HEADER_TEXT_LIFT,
                    390, 3, 2);

  gfx->setTextColor(inkColor());
  // Status is read constantly during play. Prefer the larger scale, but keep
  // long Korean status messages on one line instead of clipping them.
  uiDrawCenteredFit(msg, CX, 88, 410, 3, 2);
}

static void drawDigiCeremony(){
  uint32_t now=millis();float t=pet.ceremonyT();bool panic=pet.ceremony==CER_RUNAWAY;
  uint16_t id=digimonIndex(pet.speciesId);int x=CX;uint8_t frame=0;bool flip=false,silhouette=false;
  if(panic){
    for(int i=0;i<46;i++){
      int rx=(i*47+now/3)%466,ry=(i*91+now/2)%470;
      gfx->drawLine(rx,ry,rx-3,ry+12,C565(0x6a,0x84,0xb0));
    }
    if(t<0.30f){frame=13+(now/280)%2;x=CX+(int)(4*sinf(now*0.04f));}
    else{x=CX-(int)(((t-0.30f)/0.70f)*(CX+120));frame=0;silhouette=t>0.6f&&((now/160)%2==0);}
    drawDigiFrameCentered(id,frame,x,PET_GROUND,0,false,silhouette);
    return;
  }
  int gcy=PET_GROUND-96;
  for(int k=0;k<4;k++){
    int r=60+k*34+(int)(10*sinf(now*0.02f));
    gfx->drawCircle(CX,gcy,r,C565(0xff,0xdf,0x8a));
  }
  for(int i=0;i<16;i++){
    int px=(i*71+28)%466,py=410-(int)((now/8+i*70)%360);if(py<30)continue;
    if(i%4==0)drawMap(SPR_HEART,32,px-8,py-8,1,false);
    else gfx->fillRect(px,py,4,4,(i%2)?C565(0xff,0xe7,0x9f):C565(0xff,0x9a,0xc0));
  }
  if(t<0.45f)frame=((now/300)&1)?2:1;
  else{x=CX+(int)(((t-0.45f)/0.55f)*(CX+140));frame=0;flip=true;}
  drawDigiFrameCentered(id,frame,x,PET_GROUND,0,flip,false);
  if(pet.showHeart())drawMap(SPR_HEART,32,x+50,PET_GROUND-190,2,false);
}

// animacion de la ceremonia (10s): despedida = reverencia con corazones y se
// aleja caminando; escapada = se asusta y sale corriendo. Sustituye al idle.
void drawCeremony() {
  if(pet.currentIsDigimon()){drawDigiCeremony();return;}
  if (!pmd.loaded) { drawPet(); return; }  // respaldo si no hay sprite PMD
  uint32_t now = millis();
  float t = pet.ceremonyT();               // 0..1 a lo largo de los 10s
  bool panic = (pet.ceremony == CER_RUNAWAY);
  int x = CX, y = PET_GROUND;
  uint8_t act = PMD_IDLE;

  if (panic) {
    // final triste: penumbra azulada + lluvia
    for (int i = 0; i < 46; i++) {
      int rx = (i * 47 + now / 3) % 466;
      int ry = (i * 91 + now / 2) % 470;
      gfx->drawLine(rx, ry, rx - 3, ry + 12, C565(0x6a, 0x84, 0xb0));
    }
    bool fade = false;
    if (t < 0.30f) {                       // cabizbajo, temblando
      act = pmd.has(PMD_HURT) ? PMD_HURT : PMD_IDLE;
      x = CX + (int)(4 * sinf(now * 0.04f));
    } else {                               // se aleja despacio y se desvanece
      act = pmd.has(PMD_WALKL) ? PMD_WALKL : PMD_IDLE;
      x = CX - (int)(((t - 0.30f) / 0.70f) * (CX + 120));
      fade = (t > 0.6f) && ((now / 160) % 2 == 0);  // parpadea hacia la silueta
    }
    drawPmdAct(act, x, y, now, true, fade, 5);  // fade=silueta: se difumina al irse
    // lagrima cayendo del bicho
    if (t < 0.55f) {
      int ty = y - 150 + (int)((now / 6) % 40);
      gfx->fillRect(x + 6, ty, 3, 6, C565(0x9a, 0xc4, 0xe8));
    }
    return;
  }

  // despedida epica: halo dorado pulsante + chispas y corazones que ascienden
  int gcy = PET_GROUND - 96;
  for (int k = 0; k < 4; k++) {
    int r = 60 + k * 34 + (int)(10 * sinf(now * 0.02f));
    gfx->drawCircle(CX, gcy, r, C565(0xff, 0xdf, 0x8a));
  }
  for (int i = 0; i < 16; i++) {
    int px = (i * 71 + 28) % 466;
    int py = 410 - (int)((now / 8 + i * 70) % 360);   // suben y reaparecen abajo
    if (py < 30) continue;
    if (i % 4 == 0) drawMap(SPR_HEART, 32, px - 8, py - 8, 1, false);  // corazoncito
    else gfx->fillRect(px, py, 4, 4, (i % 2) ? C565(0xff, 0xe7, 0x9f) : C565(0xff, 0x9a, 0xc0));
  }

  if (t < 0.45f) {                         // reverencia / pose de despedida
    act = pmd.has(PMD_POSE) ? PMD_POSE : (pmd.has(PMD_NOD) ? PMD_NOD : PMD_IDLE);
  } else {                                 // se aleja por la derecha
    act = pmd.has(PMD_WALKR) ? PMD_WALKR : PMD_IDLE;
    x = CX + (int)(((t - 0.45f) / 0.55f) * (CX + 140));
  }
  drawPmdAct(act, x, y, now, true, false, 5);
  if (pet.showHeart())                     // corazon grande siguiendo al bicho
    drawMap(SPR_HEART, 32, x + 50, y - 190, 2, false);
}

// dialogo de decision (2 botones apilados): evolucionar/mantener o despedirse/quedaros
// The two confirm buttons' hit areas, so a test can hold them to UI_TAP_MIN and
// prove they do not overlap without restating the numbers.
void uiConfirmRects(int *b1Top, int *b1Bot, int *b2Top, int *b2Bot) {
  *b1Top = CONFIRM_B1_Y;
  *b1Bot = CONFIRM_B1_Y + CONFIRM_BTN_H;
  *b2Top = CONFIRM_B2_Y;
  *b2Bot = CONFIRM_B2_Y + CONFIRM_BTN_H;
}

// Draws the panel and its two buttons. sub1/sub2 are optional lines between the
// question and the buttons -- what the choice COSTS, which for anything
// irreversible has to be on screen before the tap, not after it.
void drawConfirmPanel(const char *q, const char *sub1, const char *sub2,
                      uint16_t subCol, const char *o1, uint16_t c1, uint16_t t1,
                      const char *o2, uint16_t c2, uint16_t t2) {
  gfx->fillRoundRect(CONFIRM_X, CONFIRM_Y, CONFIRM_W, CONFIRM_H, 16, UI_WHITE);
  gfx->drawRoundRect(CONFIRM_X, CONFIRM_Y, CONFIRM_W, CONFIRM_H, 16, UI_INK);
  gfx->setTextColor(UI_INK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(q, 2), 176);
  gfx->print(q);
  if (sub1 || sub2) {
    uiSetTextSize(1);
    gfx->setTextColor(subCol);
    if (sub1) {
      uiSetCursor(CX - uiTextHalfWidth(sub1, 1), sub2 ? 188 : 194);
      gfx->print(sub1);
    }
    if (sub2) {
      uiSetCursor(CX - uiTextHalfWidth(sub2, 1), 197);
      gfx->print(sub2);
    }
    uiSetTextSize(2);
    gfx->setTextColor(UI_INK);
  }
  gfx->fillRoundRect(CONFIRM_BTN_X, CONFIRM_B1_Y, CONFIRM_BTN_W, CONFIRM_BTN_H, 12, c1);
  gfx->setTextColor(t1);
  uiSetCursor(CX - uiTextHalfWidth(o1, 2), CONFIRM_B1_Y + 18);
  gfx->print(o1);
  gfx->fillRoundRect(CONFIRM_BTN_X, CONFIRM_B2_Y, CONFIRM_BTN_W, CONFIRM_BTN_H, 12, c2);
  gfx->setTextColor(t2);
  uiSetCursor(CX - uiTextHalfWidth(o2, 2), CONFIRM_B2_Y + 18);
  gfx->print(o2);
}

void drawChoiceDialog() {
  const char *q, *o1, *o2;
  const char *sub1 = nullptr, *sub2 = nullptr;
  uint16_t c1, c2, t1, t2;
  if (choiceKind == 1) {  // evolucion
    q = T(S_EVO_Q); o1 = T(S_EVO_TAP); o2 = T(S_EVO_KEEP);
    c1 = UI_BAR_BAD; t1 = UI_WHITE; c2 = UI_TRACK; t2 = UI_INK;
  } else if (choiceKind == 3) {   // voluntary ending, opened only from menu
    q = T(S_FAR_Q);
    o1 = T(S_FAR_GO); o2 = T(S_FAR_STAY);
    c1 = UI_BAR_WARN; t1 = UI_INK; c2 = UI_BAR_OK; t2 = UI_WHITE;
  } else {                // despedida
    q = T(S_FAR_Q); o1 = T(S_FAR_GO); o2 = T(S_FAR_STAY);
    c1 = UI_BAR_WARN; t1 = UI_INK; c2 = UI_BAR_OK; t2 = UI_WHITE;
  }
  drawConfirmPanel(q, sub1, sub2, UI_BAR_BAD, o1, c1, t1, o2, c2, t2);
}

// boton-CTA rojo y grande para evolucionar (pulsa para llamar la atencion)
void drawEvolveButton() {
  uint32_t now = millis();
  int p = (int)(5 * sinf(now * 0.006f));  // late: -5..5
  int x = EVO_BTN_X - p, y = EVO_BTN_Y - p, w = EVO_BTN_W + 2 * p, h = EVO_BTN_H + 2 * p;
  gfx->fillRoundRect(x, y, w, h, 18, UI_BAR_BAD);
  gfx->drawRoundRect(x, y, w, h, 18, UI_WHITE);
  gfx->drawRoundRect(x + 2, y + 2, w - 4, h - 4, 16, UI_WHITE);
  gfx->setTextColor(UI_WHITE);
  uiSetTextSize(3);
  const char *t = T(S_EVO_TAP);
  uiSetCursor(CX - uiTextHalfWidth(t, 3), y + h / 2 - 11);
  gfx->print(t);
}

// boton-CTA dorado de despedida: "<nombre> quiere decirte algo..."
void drawFarewellButton() {
  uint32_t now = millis();
  int p = (int)(4 * sinf(now * 0.005f));
  int x = FAR_BTN_X - p, y = FAR_BTN_Y - p, w = FAR_BTN_W + 2 * p, h = FAR_BTN_H + 2 * p;
  gfx->fillRoundRect(x, y, w, h, 16, UI_BAR_WARN);
  gfx->drawRoundRect(x, y, w, h, 16, UI_INK);
  char buf[96];
  const char *nm = pet.nick[0] ? pet.nick : creatureName(pet.speciesId);
  snprintf(buf, sizeof(buf), T(S_FAREWELL_BTN), nm);
  gfx->setTextColor(UI_INK);
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(buf, 2), y + h / 2 - 8);
  gfx->print(buf);
}

// boton-CTA sombrio de escapada por abandono: "<nombre> se siente abandonado..."
// (final triste: azul-gris oscuro, latido lento y apagado)
void drawRunawayButton() {
  uint32_t now = millis();
  int p = (int)(3 * sinf(now * 0.003f));
  int x = FAR_BTN_X - p, y = FAR_BTN_Y - p, w = FAR_BTN_W + 2 * p, h = FAR_BTN_H + 2 * p;
  gfx->fillRoundRect(x, y, w, h, 16, C565(0x3a, 0x44, 0x5a));
  gfx->drawRoundRect(x, y, w, h, 16, C565(0x70, 0x80, 0x98));
  char buf[96];
  const char *nm = pet.nick[0] ? pet.nick : creatureName(pet.speciesId);
  snprintf(buf, sizeof(buf), T(S_RUNAWAY_BTN), nm);
  gfx->setTextColor(C565(0xc8, 0xd2, 0xe0));
  uiSetTextSize(2);
  uiSetCursor(CX - uiTextHalfWidth(buf, 2), y + h / 2 - 8);
  gfx->print(buf);
}

// animacion epica de evolucion: halo radial + rayos giratorios + parpadeo del
// sprite acelerando + chispas que salen disparadas + fogonazo final
void drawEvolveFX(uint32_t now) {
  float t = pet.evolveT();          // 0..1
  int cx = CX, cy = PET_GROUND - 96;

  // halo radial que crece y pulsa
  int halo = 36 + (int)(t * 150) + (int)(8 * sinf(now * 0.02f));
  for (int k = 0; k < 4; k++) {
    int r = halo - k * 7;
    if (r > 0) gfx->drawCircle(cx, cy, r, UI_WHITE);
  }
  // rayos giratorios desde el centro del bicho
  float base = now * 0.004f;
  for (int i = 0; i < 12; i++) {
    float a = base + i * (float)(PI / 6);
    int len = 90 + (int)(70 * (0.5f + 0.5f * sinf(now * 0.012f + i)));
    gfx->drawLine(cx, cy, cx + (int)(cosf(a) * len), cy + (int)(sinf(a) * len), UI_WHITE);
  }
  // parpadeo entre la forma ANTERIOR y la NUEVA (siluetas), acelerando; al
  // final (t>0.9) se queda fija en la nueva para el fogonazo de revelado
  int period = 60 + (int)(220 * (1.0f - t));
  bool showOld = t < 0.9f && evoPmd.loaded && ((now / period) % 2) == 0;
  if (showOld) drawPmdActM(evoPmd, PMD_IDLE, cx, PET_GROUND, 0, true, true, 5);
  else drawPmdAct(PMD_IDLE, cx, PET_GROUND, 0, true, true, 5);
  // chispas que salen disparadas
  for (int i = 0; i < 10; i++) {
    float a = i * (float)(PI / 5) + t * 4.0f;
    int d = (int)((now / 14 + i * 33) % 200);
    int sx = cx + (int)(cosf(a) * d), sy = cy + (int)(sinf(a) * d);
    gfx->fillRect(sx - 2, sy - 2, 5, 5, (i & 1) ? C565(0xff, 0xe0, 0x70) : UI_WHITE);
  }
  // fogonazo final antes de revelar la forma nueva
  if (t > 0.9f) gfx->fillCircle(cx, cy, (int)(300 * (t - 0.9f) / 0.1f), UI_WHITE);
}

// Digimon use the same halo/rays/sparks/timing as Pokemon evolution, while
// their old and new DGI3 frame 0 sprites alternate on the shared ground line.
static bool drawDigiEvolutionForm(uint16_t spriteId, bool silhouette) {
  return drawDigiFrameCentered(spriteId,0,CX,PET_GROUND,0,false,silhouette);
}

static void drawDigiEvolveFX(uint32_t now) {
  float t=pet.evolveT();
  int cx=CX,cy=PET_GROUND-96;
  int halo=36+(int)(t*150)+(int)(8*sinf(now*0.02f));
  for(int k=0;k<4;k++){int r=halo-k*7;if(r>0)gfx->drawCircle(cx,cy,r,UI_WHITE);}
  float base=now*0.004f;
  for(int i=0;i<12;i++){
    float a=base+i*(float)(PI/6);
    int len=90+(int)(70*(0.5f+0.5f*sinf(now*0.012f+i)));
    gfx->drawLine(cx,cy,cx+(int)(cosf(a)*len),cy+(int)(sinf(a)*len),UI_WHITE);
  }
  uint16_t current=digimonIndex(pet.speciesId);
  uint16_t previous=isDigimonId(pet.prevSpeciesId)?digimonIndex(pet.prevSpeciesId):current;
  int period=60+(int)(220*(1.0f-t));
  bool showOld=t<0.9f&&((now/period)%2)==0;
  bool silhouette=t<0.82f;
  if(!drawDigiEvolutionForm(showOld?previous:current,silhouette)&&showOld)
    drawDigiEvolutionForm(current,silhouette);
  for(int i=0;i<10;i++){
    float a=i*(float)(PI/5)+t*4.0f;int d=(int)((now/14+i*33)%200);
    int sx=cx+(int)(cosf(a)*d),sy=cy+(int)(sinf(a)*d);
    gfx->fillRect(sx-2,sy-2,5,5,(i&1)?C565(0xff,0xe0,0x70):UI_WHITE);
  }
  if(t>0.9f)gfx->fillCircle(cx,cy,(int)(300*(t-0.9f)/0.1f),UI_WHITE);
}

void drawPet() {
  if (pet.currentIsDigimon()) {
    if (pet.evolving()) drawDigiEvolveFX(millis());
    else drawDigiSprite();
    return;
  }
  if (pmd.loaded) {
    drawPetPMD();
    return;
  }
  if (mon.loaded) {
    drawPetSD();
    return;
  }
  int fi = flashIdxForDex(pet.speciesId);
  if (fi < 0) {
    // sin SD y sin sprite de flash: aviso claro de que faltan sprites
    gfx->setTextColor(inkColor());
    uiSetTextSize(6);
    uiSetCursor(CX - 18, PET_CY - 80);
    gfx->print("?");
    uiSetTextSize(2);
    const char *l1 = T(S_NO_SPRITES);
    uiSetCursor(CX - uiTextHalfWidth(l1, 2), PET_CY - 4);
    gfx->print(l1);
    const char *l2 = T(S_LOAD_SPRITES);
    uiSetCursor(CX - uiTextHalfWidth(l2, 2), PET_CY + 20);
    gfx->print(l2);
    return;
  }
  const Species &sp = SPECIES[fi];
  int s = sp.scale;
  int x = CX - 16 * s;
  int y = PET_CY - 16 * s;

  // animacion de evolucion: alterna la silueta de la forma anterior y la nueva
  if (pet.evolving()) {
    bool flash = (millis() / 300) % 2;
    int16_t showDex = (flash && pet.prevSpeciesId >= 0) ? pet.prevSpeciesId : pet.speciesId;
    int sfi = flashIdxForDex(showDex);
    if (sfi >= 0) {
      const Species &show = SPECIES[sfi];
      drawMap(show.sprite, SPRITE_H, CX - 16 * show.scale, PET_CY - 16 * show.scale, show.scale, flash);
    }
    return;
  }

  PetMood m = pet.mood();
  if (m == MOOD_HAPPY && (millis() / 500) % 2) y -= 6;  // saltito

  drawMap(sp.sprite, SPRITE_H, x, y, s, false);

  // expresiones superpuestas usando las anclas de la especie
  bool blink = (millis() % 3500 < 300);
  if (m == MOOD_SLEEPING || blink) {
    overlayEye(sp, x, y, s, sp.eyeColL);
    overlayEye(sp, x, y, s, sp.eyeColR);
  }
  if (m == MOOD_EATING) overlayMouth(sp, x, y, s, true);
  else if (m == MOOD_SAD) overlayMouth(sp, x, y, s, false);

  if (pet.showHeart()) drawMap(SPR_HEART, 32, x + 20 * s, y - 2 * s, 2, false);
}

// ---------- escena de bano ----------

void startBath() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony || bathUntil) return;
  bathUntil = millis() + 3000;
  bathPending = true;
  int cx = (int)beh.x;
  for (auto &b : bubbles) {
    b.x = cx - 70 + random(140);
    b.y = PET_GROUND - random(150);
    b.r = 8 + random(16);
    b.ph = random(64);
  }
}

void drawBath() {
  uint32_t now = millis();
  if (now > bathUntil) {
    bathUntil = 0;
    if (bathPending) {
      bathPending = false;
      pet.clean();
      // pose de alegria al quedar limpio
      if (pmd.has(PMD_POSE)) {
        beh.mode = 2;
        beh.act = PMD_POSE;
        beh.t0 = now;
        beh.until = now + pmdActTotalMs(pmd.acts[PMD_POSE]) * 2;
      }
    }
    return;
  }
  uint32_t left = bathUntil - now;
  if (left > 800) {
    // espuma: pompas meciendose y subiendo poco a poco
    float t = now / 220.0f;
    for (auto &b : bubbles) {
      int bx = b.x + (int)(sinf(t + b.ph) * 6);
      int by = b.y - (int)((3000 - left) / 90);
      gfx->fillCircle(bx, by, b.r, UI_WHITE);
      gfx->drawCircle(bx, by, b.r, 0x7E3D);
      gfx->fillCircle(bx - b.r / 3, by - b.r / 3, b.r / 4, UI_BG_DAY);
    }
  } else {
    // las pompas revientan: destellos
    for (int i = 0; i < 8; i++) {
      auto &b = bubbles[i];
      int sx = b.x + (i % 3) * 6 - 6, sy = b.y - 18;
      uint16_t col = (i % 2) ? UI_BAR_WARN : UI_WHITE;
      gfx->fillRect(sx - 6, sy - 1, 13, 3, col);
      gfx->fillRect(sx - 1, sy - 6, 3, 13, col);
    }
  }
}

// ---------- mascota PMD: comportamiento ----------

static uint16_t pmdSafeFrameMs(const PmdAct &a, uint8_t i) {
  // A few community PMD packs contain 0-ms frame timings. The old selector did
  // `while (t >= 0)` forever when such an action (notably HOP) was used. Treat
  // zero as a normal short animation frame instead of trusting malformed data.
  if (i >= a.frames) return 80;
  return a.ms[i] ? a.ms[i] : 80;
}

uint32_t pmdActTotalMs(const PmdAct &a) {
  // v3.61.2: parsed once at load time. Re-summing up to 24 durations for every
  // screen frame was small on the original catalog but wasteful with several
  // simultaneous PMD actors in battle/gallery.
  if (a.totalMs) return a.totalMs;
  uint32_t t = 0;
  for (uint8_t i = 0; i < a.frames; i++) t += pmdSafeFrameMs(a, i);
  return t ? t : 100;
}

uint8_t pmdFrameAt(const PmdAct &a, uint32_t t, bool loop) {
  if (!a.frames) return 0;
  uint32_t total = pmdActTotalMs(a);
  if (!loop && t >= total) return a.frames - 1;
  t %= total;
  // Bounded by frame count: even corrupt timing data can never spin forever.
  for (uint8_t i = 0; i < a.frames; i++) {
    uint16_t ms = pmdSafeFrameMs(a, i);
    if (t < ms) return i;
    t -= ms;
  }
  return a.frames - 1;
}

static uint8_t pmdVisibleW(const PmdAct &a) {
  return (a.visR > a.visL) ? (uint8_t)(a.visR - a.visL) : a.w;
}

static uint8_t pmdVisibleH(const PmdAct &a) {
  return (a.base > a.visT) ? (uint8_t)(a.base - a.visT) : a.h;
}

// Draw a PMD action anchored by its visible feet. v3.57 deliberately scales
// from non-transparent content rather than the raw PMD canvas. This normalizes
// padded large-bodied sheets (Dragonite, etc.) toward the same on-screen body
// size as a well-packed Venusaur sheet without maintaining species-by-species
// exceptions.
void drawPmdActM(PmdMon &m, uint8_t actId, int cx, int groundY, uint32_t t, bool loop, bool sil, uint8_t maxS) {
  const PmdAct &a = m.acts[actId];
  if (!a.frames) return;

  const PmdAct &idle = m.acts[PMD_IDLE].frames ? m.acts[PMD_IDLE] : a;
  uint8_t idleVisH = pmdVisibleH(idle);
  uint8_t idleVisW = pmdVisibleW(idle);

  // About 170 visible pixels is the established Venusaur-like home-screen
  // target. The important change is that the divisor is the *visible body*,
  // not the padded canvas. Keep floor-based integer zoom so a 1-step jump does
  // not suddenly make a bulky species much larger than Venusaur.
  uint8_t sBase = idleVisH ? (uint8_t)(170u / idleVisH) : maxS;
  if (sBase < 2) sBase = 2;
  if (sBase > maxS) sBase = maxS;

  // Keep very wide bodies inside the usable scene. Battle calls use maxS=4,
  // so their width cap is tighter; home/gallery/evolution keep the larger cap.
  uint16_t idleWidthCap = (maxS <= 4) ? 176 : 224;
  while (sBase > 2 && (uint16_t)idleVisW * sBase > idleWidthCap) sBase--;

  // Integer zoom has coarse steps. If the floor result is still visibly tiny,
  // allow one extra step only when it stays within a Venusaur-like safe band.
  if ((uint16_t)idleVisH * sBase < 145 && sBase < maxS) {
    uint8_t next = sBase + 1;
    if ((uint16_t)idleVisH * next <= 190 &&
        (uint16_t)idleVisW * next <= idleWidthCap) sBase = next;
  }

  uint8_t s = sBase;
  uint8_t actVisH = pmdVisibleH(a);
  uint8_t actVisW = pmdVisibleW(a);
  uint16_t actionHeightCap = (maxS <= 4) ? 205 : 250;
  uint16_t actionWidthCap  = (maxS <= 4) ? 188 : 270;
  while (s > 2 && ((uint16_t)actVisH * s > actionHeightCap ||
                   (uint16_t)actVisW * s > actionWidthCap)) s--;

  uint8_t fi = pmdFrameAt(a, t, loop);
  const uint8_t *fr = a.data + (uint32_t)fi * a.w * a.h;

  // Center the visible body rather than the padded canvas. Vertical anchoring
  // continues to use the lowest visible row so Eat/Hurt/Attack do not float.
  int visCenter2 = (int)a.visL + (int)(a.visR ? a.visR : a.w);
  int x0 = cx - visCenter2 * s / 2;
  int y0 = groundY - (a.base ? a.base : a.h) * s;
  // Scan only the cached visible union. Some PMDCollab sheets have enormous
  // transparent margins; walking those empty pixels every frame was pure CPU
  // work and became noticeable after the full catalog expansion.
  // v3.61.3: per-frame bounds come directly from TPK3 for generated catalog
  // sprites (legacy TPK2 computes them once while loading). Walking/attack
  // sheets often move within a larger action canvas, so scanning the exact
  // current frame removes additional transparent work without changing pixels.
  const int r0 = (a.frameT[fi] < a.h) ? a.frameT[fi] : ((a.visT < a.h) ? a.visT : 0);
  const int r1 = (a.frameB[fi] > r0 && a.frameB[fi] <= a.h) ? a.frameB[fi] :
                 ((a.base > r0 && a.base <= a.h) ? a.base : a.h);
  const int c0 = (a.frameL[fi] < a.w) ? a.frameL[fi] : ((a.visL < a.w) ? a.visL : 0);
  const int c1 = (a.frameR[fi] > c0 && a.frameR[fi] <= a.w) ? a.frameR[fi] :
                 ((a.visR > c0 && a.visR <= a.w) ? a.visR : a.w);
  for (int r = r0; r < r1; r++) {
    const uint8_t *row = fr + r * a.w;
    int c = c0;
    while (c < c1) {
      uint8_t idx = row[c];
      int run = 1;
      while (c + run < c1 && row[c + run] == idx) run++;
      if (idx != 0xFF)
        canvasFillRectFast(x0 + c * s, y0 + r * s, run * s, s, sil ? INK_K : m.pal[idx]);
      c += run;
    }
  }
}
void drawPmdAct(uint8_t actId, int cx, int groundY, uint32_t t, bool loop, bool sil, uint8_t maxS) {
  drawPmdActM(pmd, actId, cx, groundY, t, loop, sil, maxS);
}

// elige el siguiente capricho del bicho cuando esta contento
void behNext() {
  uint32_t now = millis();
  beh.t0 = now;
  int r = random(100);
  if (r < 35 && (pmd.has(PMD_WALKL) || pmd.has(PMD_WALKR))) {
    beh.mode = 1;  // paseo
    beh.targetX = 150 + random(176);
    beh.until = now + 15000;
  } else if (r < 60) {
    // gesto aleatorio entre los disponibles
    // (Hop fuera: salta demasiado alto; Sit fuera: mira hacia atras)
    static const uint8_t flair[] = { PMD_POSE, PMD_NOD, PMD_BREATH };
    uint8_t pick[3], n = 0;
    for (uint8_t f : flair)
      if (pmd.has(f)) pick[n++] = f;
    if (n) {
      beh.mode = 2;
      beh.act = pick[random(n)];
      beh.until = now + pmdActTotalMs(pmd.acts[beh.act]);
      return;
    }
    beh.mode = 0;
    beh.until = now + 2000 + random(3000);
  } else {
    beh.mode = 0;  // mirar al frente
    beh.until = now + 2000 + random(3000);
  }
}

void drawPetPMD() {
  uint32_t now = millis();

  if (pet.evolving()) {
    drawEvolveFX(now);
    return;
  }
  if (evoPmd.loaded) evoPmd.unload();  // termino la evolucion: libera la forma anterior

  PetMood m = pet.mood();
  uint8_t act;
  bool loop = true;
  if (m == MOOD_SLEEPING && pmd.has(PMD_SLEEP)) {
    act = PMD_SLEEP;
    beh.mode = 0;
  } else if (m == MOOD_EATING && pmd.has(PMD_EAT)) {
    act = PMD_EAT;
    beh.t0 = 0;
  } else if (m == MOOD_SAD && pmd.has(PMD_HURT)) {
    act = PMD_HURT;
  } else {
    // contento: el planificador decide (idle / paseo / gesto)
    if (now > beh.until) behNext();
    if (beh.mode == 1) {
      float d = beh.targetX - beh.x;
      if (fabsf(d) < 4) {
        behNext();
        act = PMD_IDLE;
      } else {
        beh.x += (d > 0 ? 3.0f : -3.0f);
        act = (d > 0) ? PMD_WALKR : PMD_WALKL;
      }
    } else {
      act = (beh.mode == 2) ? beh.act : PMD_IDLE;
      loop = false;
    }
    if (!pmd.has(act)) act = PMD_IDLE;
  }

  drawPmdAct(act, (int)beh.x, PET_GROUND, now - beh.t0, loop || act == PMD_IDLE, false, 5);

  if (pet.showHeart()) drawMap(SPR_HEART, 32, (int)beh.x + 50, PET_GROUND - 190, 2, false);
}

// sprite animado desde la SD: zoom entero por pixel, frames a su ritmo
void drawPetSD() {
  int s = mon.scale;
  int w = mon.w * s, h = mon.h * s;
  int x = CX - w / 2;
  int y = PET_CY - h / 2;

  bool sil = false;
  if (pet.evolving()) {
    sil = (millis() / 300) % 2;
  } else if (pet.mood() == MOOD_HAPPY && (millis() / 500) % 2) {
    y -= 6;  // saltito
  }

  uint16_t fm = mon.frameMs ? mon.frameMs : 100;
  uint16_t fi = pet.sleeping ? 0 : (millis() / fm) % mon.frames;
  const uint8_t *fr = mon.data + (uint32_t)fi * mon.w * mon.h;
  for (int r = 0; r < mon.h; r++) {
    const uint8_t *row = fr + r * mon.w;
    for (int c = 0; c < mon.w; c++) {
      uint8_t idx = row[c];
      if (idx == 0xFF) continue;
      gfx->fillRect(x + c * s, y + r * s, s, s, sil ? INK_K : mon.pal[idx]);
    }
  }

  // emotes en vez de expresiones (los sprites importados no tienen anclas)
  if (pet.showHeart()) drawMap(SPR_HEART, 32, x + w - 30, y - 50, 2, false);
}

// ojo cerrado: borra el ojo 3x4 y dibuja el parpado
void overlayEye(const Species &sp, int x, int y, int s, int col) {
  gfx->fillRect(x + col * s, y + sp.eyeRow * s, 3 * s, 4 * s, sp.bodyColor);
  gfx->fillRect(x + col * s, y + (sp.eyeRow + 2) * s, 3 * s, s, INK_K);
}

// borra la sonrisa base y pinta boca abierta (comer) o ceno (triste)
void overlayMouth(const Species &sp, int x, int y, int s, bool open) {
  int mc = sp.mouthCol, mr = sp.mouthRow;
  gfx->fillRect(x + (mc - 3) * s, y + mr * s, 7 * s, 2 * s, sp.bodyColor);
  if (open) {
    gfx->fillRect(x + (mc - 2) * s, y + mr * s, 5 * s, 2 * s, INK_K);
  } else {
    gfx->fillRect(x + (mc - 2) * s, y + mr * s, 5 * s, s, INK_K);
    gfx->fillRect(x + (mc - 3) * s, y + (mr + 1) * s, s, s, INK_K);
    gfx->fillRect(x + (mc + 3) * s, y + (mr + 1) * s, s, s, INK_K);
  }
}

void drawPoops() {
  for (int i = 0; i < pet.poops; i++) {
    drawMap(SPR_POOP, 32, 36 + i * 46, 244, 2, false);
  }
}

void drawBars() {
  drawBar(78, 318, T(S_BAR_FOOD), pet.fullness);
  drawBar(244, 318, T(S_BAR_JOY), pet.joy);
  drawBar(78, 346, T(S_BAR_ENE), pet.energy);
  drawBar(244, 346, T(S_BAR_HYG), pet.hygiene);
}

void drawBar(int x, int y, const char *label, uint8_t val) {
  gfx->setTextColor(inkColor());
  uiSetTextSize(2);
  uiSetCursor(x, y);
  gfx->print(label);
  int bx = x + 48, bw = 100, bh = 15;  // +48: deja sitio a etiquetas de 4 letras (EN)
  uint16_t fill = (val >= 50) ? UI_BAR_OK : (val >= 25) ? UI_BAR_WARN : UI_BAR_BAD;
  gfx->fillRoundRect(bx, y, bw, bh, 4, UI_TRACK);
  int fw = (bw - 4) * val / 100;
  if (fw > 0) gfx->fillRoundRect(bx + 2, y + 2, fw, bh - 4, 3, fill);
}

void drawButtons() {
  for (int i = 0; i < BTN_COUNT; i++) {
    bool off = uiButtonDisabled(i);   // durmiendo solo funciona LUZ
    int bx = buttons[i].cx - BTN_HALF, by = buttons[i].cy - BTN_HALF;
    if (!pet.sleeping) gfx->fillRoundRect(bx, by, 2 * BTN_HALF, 2 * BTN_HALF, 14, UI_WHITE);
    gfx->drawRoundRect(bx, by, 2 * BTN_HALF, 2 * BTN_HALF, 14, inkColor());
    if (!off) drawMap(buttons[i].icon, 16, buttons[i].cx - 16, buttons[i].cy - 16, 2, false);
  }
}

const char *eggMsg() {
  switch (pet.eggCracks()) {
    case 0: return T(S_EGG_TOUCH);
    case 1: return T(S_EGG_MOVES);
    default: return T(S_EGG_ALMOST);
  }
}

const char *statusMsg() {
  if (pet.evolving()) return T(S_EVOLVING);
  if (bathUntil) return "첨벙첨벙!";
  if (pet.sleeping) return "Zzz...";
  if (pet.eating()) return T(S_EATING);
  if (pet.showHeart()) return T(S_LIKES);
  if (pet.fullness < 25) return T(S_HUNGRY);
  if (pet.hygiene < 25) return T(S_NEEDS_BATH);
  if (pet.energy < 25) return T(S_EXHAUSTED);
  if (pet.joy < 25) return T(S_SAD);
  if (pet.weight > 60) return T(S_CHUBBY);
  if (pet.shiny && pet.ageMinutes < 15) return T(S_IS_SHINY);
  return T(S_HAPPY);
}

// dibuja un mapa de n x n pixeles escalado; silhouette=true lo pinta en tinta
// An 8bpp indexed avatar, same shape as the badge art: 0xFF is transparent.
void drawAvatar(uint8_t which, int x, int y, int s) {
  const AvatarArt &a = AVATARS[which % AVATAR_COUNT];
  for (int r = 0; r < AVATAR_PX; r++)
    for (int c = 0; c < AVATAR_PX; c++) {
      uint8_t v = a.idx[r * AVATAR_PX + c];
      if (v == 0xFF) continue;
      canvasFillRectFast(x + c * s, y + r * s, s, s, a.pal[v]);
    }
}

void drawMap(const char *const *map, int n, int x, int y, int s, bool silhouette) {
  for (int r = 0; r < n; r++) {
    for (int c = 0; c < n; c++) {
      char ch = map[r][c];
      if (ch == '.') continue;
      canvasFillRectFast(x + c * s, y + r * s, s, s, silhouette ? INK_K : spriteColor(ch));
    }
  }
}
