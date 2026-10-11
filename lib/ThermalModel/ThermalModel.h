#ifndef THERMAL_MODEL_H
#define THERMAL_MODEL_H

#include <Arduino.h>

// ============================================================
// SWITCH UTAMA MODUL INI
// ------------------------------------------------------------
//   1  = AKTIF  -> suhu/RH dihitung ulang tiap siklus berdasarkan
//                  peltierOutput/fanHSOutput/mistOn yang BARU SAJA
//                  dihasilkan controlActuators() -> closed-loop
//                  simulasi (loop tertutup secara matematis).
//   0  = NONAKTIF -> baris ini tidak berlaku.
// ------------------------------------------------------------
// PENTING: jangan aktifkan modul ini BERSAMAAN dengan
// ENV_TEST_MODE=1 di EnvInject.h -> keduanya sama-sama menentukan
// nilai suhu/RH, kalau dua-duanya aktif akan tumpang tindih dan
// hasilnya tidak merepresentasikan skenario manapun dengan jelas.
// Pilih salah satu sesuai jenis pengujian yang sedang dilakukan.
// ============================================================
#define THERMAL_MODEL_MODE 1

// ------------------------------------------------------------
// PARAMETER FISIKA (nilai awal perkiraan, silakan dikalibrasi)
// ------------------------------------------------------------
// k1: efektivitas pendinginan Peltier (derajat C per % daya per detik)
//     -> makin besar k1, makin cepat Peltier mendinginkan chamber.
#define THERMAL_K1 0.004f

// k2: laju kebocoran panas dari luar ke dalam chamber (per detik)
//     -> makin besar k2, makin cepat panas luar "bocor" masuk
//        (insulasi jelek = k2 besar; insulasi bagus = k2 kecil).
#define THERMAL_K2 0.0015f

// k3: laju kenaikan RH saat mist maker menyala (% per detik)
#define THERMAL_K3 0.15f

// k4: laju RH kembali ke RH_ambient saat mist maker mati (per detik)
#define THERMAL_K4 0.01f

// Kondisi lingkungan luar chamber (asumsi ruangan di Medan)
#define THERMAL_T_AMBIENT  30.0f   // derajat C
#define THERMAL_RH_AMBIENT 60.0f   // persen

// Suhu & RH awal saat simulasi mulai (kondisi awal sebelum Peltier aktif)
#define THERMAL_T_INITIAL  28.0f
#define THERMAL_RH_INITIAL 65.0f

// Panggil sekali di setup() untuk set kondisi awal chamber simulasi.
void thermalModelBegin();

// Panggil di loop() SETELAH controlActuators() dipanggil -> modul ini
// butuh peltierOutput/fanHSOutput/mistOn yang PALING BARU untuk
// menghitung suhu/RH siklus berikutnya (inilah yang membuatnya
// closed-loop, beda dengan EnvInject yang datanya sudah ditentukan
// sebelumnya tanpa peduli aksi aktuator).
void thermalModelUpdate(float peltierOutputPct, float fanHSOutputPct,
                         bool mistIsOn, float dtSeconds,
                         float &suhuOut, float &rhOut);

#endif
