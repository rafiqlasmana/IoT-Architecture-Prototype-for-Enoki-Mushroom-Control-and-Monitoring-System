#include "ThermalModel.h"

static float simTemp = THERMAL_T_INITIAL;
static float simRH   = THERMAL_RH_INITIAL;

void thermalModelBegin() {
  simTemp = THERMAL_T_INITIAL;
  simRH   = THERMAL_RH_INITIAL;
}

void thermalModelUpdate(float peltierOutputPct, float fanHSOutputPct,
                         bool mistIsOn, float dtSeconds,
                         float &suhuOut, float &rhOut) {
  // fanHSOutputPct belum dipakai di rumus suhu versi sederhana ini
  // (Fan Heatsink cuma mempercepat pembuangan panas SISI PANAS
  // Peltier ke luar chamber, tidak langsung memengaruhi suhu DALAM
  // chamber pada model sesederhana ini). Parameter tetap disediakan
  // untuk pengembangan model yang lebih detail nanti.
  (void)fanHSOutputPct;

  // ---------- SUHU ----------
  // dT/dt = -k1 * peltierOutput  (efek pendinginan Peltier)
  //         + k2 * (T_ambient - T)  (kebocoran panas dari luar)
  float dT = (-THERMAL_K1 * peltierOutputPct
              + THERMAL_K2 * (THERMAL_T_AMBIENT - simTemp)) * dtSeconds;
  simTemp += dT;

  // ---------- KELEMBAPAN (RH) ----------
  // dRH/dt = +k3 saat mist menyala  - k4 * (RH - RH_ambient) saat mist mati
  float dRH;
  if (mistIsOn) {
    dRH = THERMAL_K3 * dtSeconds;
  } else {
    dRH = -THERMAL_K4 * (simRH - THERMAL_RH_AMBIENT) * dtSeconds;
  }
  simRH += dRH;

  // Batasi ke rentang fisis wajar (hindari nilai tidak masuk akal)
  simTemp = constrain(simTemp, -10.0f, 50.0f);
  simRH   = constrain(simRH, 0.0f, 100.0f);

  suhuOut = simTemp;
  rhOut   = simRH;
}
