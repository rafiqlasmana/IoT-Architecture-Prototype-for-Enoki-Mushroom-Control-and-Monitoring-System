#include "EnvInject.h"
#include <math.h>

// ------------------------------------------------------------
// Jendela waktu (detik sejak envInjectBegin()) untuk simulasi
// sensor gagal baca (NaN) -> uji NAN_FAILSAFE_THRESHOLD.
// 10 detik / 2 detik per siklus = 5x baca berturut-turut gagal.
// Tambah baris untuk uji di titik waktu lain.
// ------------------------------------------------------------
struct FailWindow { unsigned long t_start; unsigned long t_end; };
static FailWindow failWindows[] = {
  {600, 610},   // uji failsafe di menit ke-10 (silakan sesuaikan)
};
static const int failWindowCount = sizeof(failWindows) / sizeof(failWindows[0]);

static unsigned long injectStartMillis = 0;

static bool isInFailWindow(unsigned long elapsedSec) {
#if ENV_INJECT_FAILSAFE_SCENARIO
  for (int i = 0; i < failWindowCount; i++) {
    if (elapsedSec >= failWindows[i].t_start && elapsedSec <= failWindows[i].t_end) {
      return true;
    }
  }
#endif
  return false;
}

// Noise kecil (±range) supaya data tidak flat sempurna,
// meniru fluktuasi sensor riil.
static float noise(float range) {
  return ((float)random(-1000, 1000) / 1000.0f) * range;
}

void envInjectBegin() {
  injectStartMillis = millis();
  randomSeed(analogRead(0));  // seed acak, boleh diganti/dihapus kalau tidak perlu
}

void envInjectRead(float &suhu, float &rh) {
  unsigned long elapsedSec = (millis() - injectStartMillis) / 1000UL;

  // Prioritas 1: simulasi gagal baca (NaN), independen dari mode aktif
  if (isInFailWindow(elapsedSec)) {
    suhu = NAN;
    rh   = NAN;
    return;
  }

  // Prioritas 2: ambil setpoint MODE YANG SEDANG AKTIF secara live
  // dari main.cpp (lewat getCurrentSetpoints()) -> otomatis ikut
  // berubah kapanpun currentMode berubah, manual maupun otomatis.
  float tMin, tMax, rMin, rMax;
  getCurrentSetpoints(tMin, tMax, rMin, rMax);

  float tMid = (tMin + tMax) / 2.0f;
  float rMid = (rMin + rMax) / 2.0f;

  suhu = tMid + noise(0.3f);   // ±0.3°C
  rh   = rMid + noise(1.0f);   // ±1.0% RH
}