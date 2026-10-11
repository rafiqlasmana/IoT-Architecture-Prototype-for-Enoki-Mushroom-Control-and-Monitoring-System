#ifndef ENV_INJECT_H
#define ENV_INJECT_H

#include <Arduino.h>

// ============================================================
//   1  = AKTIF  -> suhu/RH disuntik OTOMATIS sesuai currentMode
//                  yang sedang aktif (bereaksi terhadap ganti
//                  fase, baik manual maupun auto-transition).
//   0  = NONAKTIF -> kembali ke dht.readTemperature()/readHumidity().
// ============================================================
#define ENV_TEST_MODE 0

// 1 = sertakan jendela simulasi "sensor gagal baca" (NaN), untuk
//     menguji NAN_FAILSAFE_THRESHOLD. 0 = tidak ada simulasi gagal baca.
#define ENV_INJECT_FAILSAFE_SCENARIO 1

// Fungsi ini HARUS ditambahkan di main.cpp (lihat instruksi integrasi) —
// mengekspos setpoint mode yang SEDANG AKTIF ke modul EnvInject, tanpa
// perlu EnvInject tahu detail enum Mode/struct ModeConfig milik main.cpp.
extern void getCurrentSetpoints(float &tMin, float &tMax, float &rMin, float &rMax);

void envInjectBegin();
void envInjectRead(float &suhu, float &rh);

#endif