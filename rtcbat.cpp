#include "rtcbat.h"
#include "pin_config.h"  // define XPOWERS_CHIP_AXP2101
#include <Wire.h>
#include <time.h>
#include <SensorPCF85063.hpp>
#include <XPowersLib.h>

static SensorPCF85063 rtc;
static XPowersPMU pmu;
static bool rtcOk = false;
static bool pmuOk = false;

bool rtcBegin() {
  rtcOk = rtc.begin(Wire, IIC_SDA, IIC_SCL);
  if (!rtcOk) Serial.println("PCF85063 no detectado");
  return rtcOk;
}

uint32_t rtcEpoch() {
  if (!rtcOk) return 0;
  RTC_DateTime t = rtc.getDateTime();
  if (t.getYear() < 2025 || t.getYear() > 2120) return 0;  // sin hora valida
  struct tm tmv = {};
  tmv.tm_year = t.getYear() - 1900;
  tmv.tm_mon = t.getMonth() - 1;
  tmv.tm_mday = t.getDay();
  tmv.tm_hour = t.getHour();
  tmv.tm_min = t.getMinute();
  tmv.tm_sec = t.getSecond();
  time_t e = mktime(&tmv);  // TZ por defecto = UTC, consistente con gmtime_r
  return e > 0 ? (uint32_t)e : 0;
}

void rtcSetEpoch(uint32_t e) {
  if (!rtcOk) return;
  time_t tt = e;
  struct tm tmv;
  gmtime_r(&tt, &tmv);
  rtc.setDateTime(RTC_DateTime(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                               tmv.tm_hour, tmv.tm_min, tmv.tm_sec));
}

bool batBegin() {
  pmuOk = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (!pmuOk) {
    Serial.println("AXP2101 no detectado");
    return false;
  }

  // v3.57.2: explicitly enable the AXP2101 channels used by the battery UI.
  // The board can boot with these ADC/detection bits in different states after
  // a deep discharge or PMU reset, so do not rely on power-on defaults.
  pmu.enableBattDetection();
  pmu.enableBattVoltageMeasure();
  return true;
}

// Enciende la alimentacion de la AMOLED. En la Waveshare 1.75 el panel (OLED VDD)
// cuelga del rail BLDO1 a 3.3V del AXP2101. El firmware daba por hecho que estaba
// encendido; si el PMU se resetea (drenaje total), BLDO1 queda OFF y la pantalla
// se ve negra aunque el resto funcione. Hay que llamarla ANTES de gfx->begin().
void pmuEnablePanel() {
  if (!pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    Serial.println("AXP2101 no detectado (pmuEnablePanel)");
    return;
  }
  pmu.setBLDO1Voltage(3300);   // OLED VDD
  pmu.enableBLDO1();
}

// Battery telemetry is intentionally cached: the AXP2101 shares I2C with the
// touch controller and RTC, so polling it every frame only adds bus traffic.
//
// AXP2101 fuel-gauge % is useful, but can jump after a load change or USB plug
// event. We therefore expose the real cell voltage too and apply light
// hysteresis to the displayed percentage. Voltage is also used as a sanity
// fallback when the gauge value is unavailable or clearly implausible.
static uint32_t powerCacheT = 0;
static int cachedPct = -1;
static int cachedRawPct = -1;
static uint16_t cachedMv = 0;
static bool cachedCharging = false, cachedUsb = true;
static bool cachedBattery = false;

struct VoltPctPoint { uint16_t mv; uint8_t pct; };
static const VoltPctPoint LIION_CURVE[] = {
  {4200,100}, {4150,95}, {4100,90}, {4050,83}, {4000,76},
  {3950,69},  {3900,61}, {3850,53}, {3800,45}, {3750,36},
  {3700,27},  {3650,18}, {3600,10}, {3500,4},  {3400,0}
};

static int voltagePercent(uint16_t mv) {
  if (mv >= LIION_CURVE[0].mv) return 100;
  const size_t n = sizeof(LIION_CURVE) / sizeof(LIION_CURVE[0]);
  for (size_t i = 1; i < n; ++i) {
    if (mv >= LIION_CURVE[i].mv) {
      uint16_t hiMv = LIION_CURVE[i - 1].mv, loMv = LIION_CURVE[i].mv;
      int hiP = LIION_CURVE[i - 1].pct, loP = LIION_CURVE[i].pct;
      return loP + (int32_t)(mv - loMv) * (hiP - loP) / (hiMv - loMv);
    }
  }
  return 0;
}

static void refreshPower() {
  uint32_t now = millis();
  if (powerCacheT && now - powerCacheT < 3000) return;
  powerCacheT = now ? now : 1;

  if (!pmuOk) {
    cachedPct = cachedRawPct = -1;
    cachedMv = 0;
    cachedCharging = false;
    cachedUsb = true;
    cachedBattery = false;
    return;
  }

  bool battery = pmu.isBatteryConnect();
  bool charging = pmu.isCharging();
  bool usb = pmu.isVbusIn();
  if (!battery) {
    cachedPct = cachedRawPct = -1;
    cachedMv = 0;
    cachedCharging = charging;
    cachedUsb = usb;
    cachedBattery = false;
    return;
  }

  uint16_t mv = pmu.getBattVoltage();
  int rawPct = (int)pmu.getBatteryPercent();
  if (rawPct < 0 || rawPct > 100) rawPct = -1;
  if (mv < 2800 || mv > 4400) mv = 0;  // reject impossible/transient ADC reads

  bool stateChanged = !cachedBattery || usb != cachedUsb || charging != cachedCharging;

  // 1/4 EMA on voltage, but reset immediately when power state changes.
  if (mv) {
    if (!cachedMv || stateChanged) cachedMv = mv;
    else cachedMv = (uint16_t)(((uint32_t)cachedMv * 3U + mv + 2U) / 4U);
  }

  int vPct = cachedMv ? voltagePercent(cachedMv) : -1;
  int target = rawPct;
  if (target < 0) target = vPct;

  // While discharging, a badly uncalibrated fuel gauge can claim a percentage
  // far away from the cell voltage. Keep the gauge as the main source, but
  // bound only extreme mismatches. During charging, voltage is artificially
  // elevated so we trust the PMU gauge instead.
  if (!usb && !charging && target >= 0 && vPct >= 0) {
    int lo = vPct - 15; if (lo < 0) lo = 0;
    int hi = vPct + 15; if (hi > 100) hi = 100;
    if (target < lo) target = lo;
    if (target > hi) target = hi;
  }

  if (target >= 0) {
    if (cachedPct < 0 || stateChanged) {
      cachedPct = target;
    } else {
      int delta = target - cachedPct;
      // Ignore 1% chatter. On battery power, the display never rises merely
      // because the load dropped; a real increase requires USB/charging.
      if (!usb && !charging && delta > 0) delta = 0;
      if (delta > 2) delta = 2;
      if (delta < -2) delta = -2;
      if (delta > 1 || delta < -1) cachedPct += delta;
    }
    if (cachedPct < 0) cachedPct = 0;
    if (cachedPct > 100) cachedPct = 100;
  }

  cachedRawPct = rawPct;
  cachedCharging = charging;
  cachedUsb = usb;
  cachedBattery = true;
}

int batPercent() { refreshPower(); return cachedPct; }
uint16_t batVoltageMv() { refreshPower(); return cachedMv; }
bool batCharging() { refreshPower(); return cachedCharging; }
bool usbPresent() { refreshPower(); return cachedUsb; }

void pwrSetup() {
  if (!pmuOk) return;
  pmu.setPowerKeyPressOffTime(XPOWERS_POWEROFF_4S);
  pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
  pmu.clearIrqStatus();
}

bool pwrShortPressed() {
  if (!pmuOk) return false;
  pmu.getIrqStatus();
  bool hit = pmu.isPekeyShortPressIrq();
  if (hit) pmu.clearIrqStatus();
  return hit;
}
