const MODES = [
  { name: "M1 Inkubasi",    tempMax: 23, tempMin: 20, rhMin: 60,  rhMax: 70,  useMist: true, useFan: false, useLED: false, color: "#f59e0b", durMin: 20, durMax: 25 },
  { name: "M2a Induksi",   tempMax: 12, tempMin: 10, rhMin: 80, rhMax: 85, useMist: true,  useFan: false, useLED: false, color: "#3b82f6", durMin: 10, durMax: 14 },
  { name: "M2b Pemanjangan", tempMax: 15, tempMin: 12, rhMin: 75, rhMax: 80, useMist: true,  useFan: true,  useLED: false, color: "#10b981", durMin: 3,  durMax: 5  },
  { name: "M2c Fruiting",    tempMax: 12, tempMin: 8, rhMin: 80, rhMax: 90, useMist: true,  useFan: false, useLED: true,  color: "#8b5cf6", durMin: 8,  durMax: 12 },
];

// 1 hari virtual = 1440 detik riil (skala sama dengan getVirtualHour: 1 menit riil = 1 jam virtual)
const VIRTUAL_SEC_PER_DAY = 1440;
let phaseStartMs = Date.now();   // direset tiap kali fase berganti
let minNotifShown = false;       // supaya toast notifikasi tidak berulang tiap tick
let deviceElapsedDays = null;    // kalau ESP32 terhubung, pakai nilai dari device (bukan Date.now() lokal)
let lastDeviceMsgAt = 0;         // timestamp pesan MQTT terakhir dari ESP32 (bukan sekadar broker konek)
const DEVICE_TIMEOUT_MS = 15000; // anggap device offline kalau tak ada data > 15 detik
let mqttClient = null;           // deklarasi awal di 'let' (bukan const) supaya TIDAK terjebak di
                                  // Temporal Dead Zone kalau mqtt.connect() gagal/CDN mqtt.js gagal load —
                                  // nextPhase() harus tetap bisa fallback ke mode simulasi lokal.

const BADGE_CLASS = ["badge-m1", "badge-m2a", "badge-m2b", "badge-m2c"];

let logData = [];
let currentMode = 0;
let simRunning = true;
let simStartMs = Date.now();
let startHour = 6;
let chartMode = "all";
let currentPage = 1;
const ROWS_PER_PAGE = 10;

function getVirtualHour() {
  let elapsed = Math.floor((Date.now() - simStartMs) / 1000);
  return (startHour + Math.floor(elapsed / 60)) % 24;
}

function simulateSensor() {
  const m = MODES[currentMode];
  let baseTemp = (m.tempMin + m.tempMax) / 2 + (Math.random() - 0.5) * 1.5;
  let baseHum = m.useMist ? (m.rhMin + m.rhMax) / 2 + (Math.random() - 0.5) * 4 : 65 + (Math.random() - 0.5) * 5;
  if (!m.useMist) baseHum = 65 + (Math.random() - 0.5) * 5;
  return {
    temp: parseFloat(baseTemp.toFixed(1)),
    hum: parseFloat(baseHum.toFixed(1))
  };
}

function controlActuators(temp, hum) {
  const m = MODES[currentMode];
  const setpoint = (m.tempMin + m.tempMax) / 2;
  const error = temp - setpoint;
  // Perkiraan proporsional sederhana untuk mode simulasi lokal saja
  // (nilai aktual sebenarnya berasal dari fuzzy logic di ESP32 lewat MQTT)
  let peltierPct = error > 0 ? Math.min(100, Math.round(error * 20)) : 0;
  let fanHSPct   = Math.round(peltierPct * 0.8);
  let mistOn = m.useMist ? (hum < m.rhMin) : false;
  let fanOn  = m.useFan;
  let vh = getVirtualHour();
  let ledOn  = m.useLED ? (vh >= 6 && vh < 18) : false;
  return { peltierPct, fanHSPct, mistOn, fanOn, ledOn };
}

// ═════════════════════════════════════════════════════════
// PEMINDAHAN FASE — man-in-the-middle
//  - Device (ESP32) yang punya keputusan akhir: auto-pindah saat durMax
//    tercapai, dan mengirim ulang mode barunya lewat topic "enoki/mode".
//  - Dashboard hanya USULAN: tombol di sini mengirim perintah "next" lewat
//    MQTT; currentMode di dashboard BARU berubah setelah device konfirmasi
//    (supaya dashboard selalu sinkron dengan kondisi aktuator sebenarnya).
//  - Kalau device belum terhubung (mode simulasi lokal), tombol langsung
//    memutar currentMode di browser saja.
// ═════════════════════════════════════════════════════════
function getPhaseElapsedDays() {
  if (deviceElapsedDays !== null) return deviceElapsedDays; // sumber kebenaran: ESP32
  return (Date.now() - phaseStartMs) / 1000 / VIRTUAL_SEC_PER_DAY;
}

function resetPhaseTimer() {
  phaseStartMs = Date.now();
  minNotifShown = false;
  deviceElapsedDays = null; // akan terisi lagi lewat topic enoki/phase_elapsed_days
}

function updatePhaseNotif() {
  const m = MODES[currentMode];
  const elapsed = getPhaseElapsedDays();
  const banner = document.getElementById('phase-notif');
  const progressText = document.getElementById('phase-progress-text');

  if (elapsed >= m.durMax) {
    banner.className = 'phase-notif max';
    banner.innerHTML = `<i class="ti ti-alarm"></i> Durasi maksimum <b>${m.name}</b> (${m.durMax} hari) tercapai — sistem memindahkan fase otomatis.`;
    banner.style.display = 'flex';
  } else if (elapsed >= m.durMin) {
    banner.className = 'phase-notif min';
    banner.innerHTML = `<i class="ti ti-bell"></i> Durasi minimum <b>${m.name}</b> (${m.durMin} hari) tercapai — sudah boleh dipindah manual (batas maks. hari ke-${m.durMax}).`;
    banner.style.display = 'flex';
    minNotifShown = true;
  } else {
    banner.style.display = 'none';
  }

  if (progressText) {
    progressText.textContent = `Hari ke-${elapsed.toFixed(1)} dari rentang ${m.durMin}–${m.durMax} hari`;
  }
}

function isDeviceOnline() {
  return !!mqttClient && mqttClient.connected &&
         lastDeviceMsgAt > 0 && (Date.now() - lastDeviceMsgAt) < DEVICE_TIMEOUT_MS;
}

function nextPhase() {
  if (isDeviceOnline()) {
    mqttClient.publish('enoki/cmd/mode', 'next');
  }
  // Optimistic UI: dashboard tetap langsung berpindah fase secara visual,
  // baik device online maupun tidak. Kalau device online, konfirmasi
  // "enoki/mode" yang menyusul dari ESP32 nanti cuma menegaskan nilai yang
  // sama (tidak dobel loncat) — kalau ternyata beda (mis. ESP32 baru saja
  // auto-pindah fase duluan), nilai dari device tetap menang karena
  // handler "enoki/mode" akan menimpanya.
  currentMode = (currentMode + 1) % 4;
  resetPhaseTimer();
  updateSetpoints();
  updatePhaseNotif();
  syncModeTabsUI();
  tick();   // paksa refresh Mode Aktif, badge aktuator, tabel — jangan tunggu interval 60 detik
}

// ═════════════════════════════════════════════════════════
// Lompat LANGSUNG ke fase tertentu (dipakai tombol mode-tab).
// Sama seperti nextPhase(), tapi payload MQTT berupa index (0-3),
// bukan "next" — firmware (mqttCallback) sudah mendukung ini.
// ═════════════════════════════════════════════════════════
function changeToMode(idx) {
  console.log('[DEBUG] changeToMode(', idx, ') diklik. isDeviceOnline():', isDeviceOnline(),
    '| mqttClient.connected:', mqttClient && mqttClient.connected,
    '| lastDeviceMsgAt:', lastDeviceMsgAt ? (Date.now()-lastDeviceMsgAt)+'ms lalu' : 'belum pernah');
  if (isDeviceOnline()) {
    mqttClient.publish('enoki/cmd/mode', String(idx), (err) => {
      if (err) console.error('[DEBUG] publish enoki/cmd/mode GAGAL (callback error):', err);
      else console.log('[DEBUG] publish enoki/cmd/mode', idx, '-> broker ACK OK');
    });
  } else {
    console.warn('[DEBUG] isDeviceOnline() FALSE -> publish DILEWATI, hanya update tampilan lokal.');
  }
  // Optimistic UI: sama seperti nextPhase(), dashboard langsung
  // berpindah tampilan. Kalau device online, konfirmasi "enoki/mode"
  // yang menyusul akan menegaskan (atau mengoreksi) nilai ini.
  currentMode = idx;
  resetPhaseTimer();
  updateSetpoints();
  updatePhaseNotif();
  syncModeTabsUI();
  tick();
}

function syncModeTabsUI() {
  document.querySelectorAll('.mode-tab').forEach(t => {
    t.classList.toggle('active', t.dataset.mode === String(currentMode));
  });
  chartMode = String(currentMode);
}

function now_str() {
  return new Date().toLocaleTimeString('id-ID', { hour12: false });
}

function now_date_str() {
  return new Date().toLocaleString('id-ID');
}

let tempData = { labels: [], data: [] };
let humData  = { labels: [], data: [] };
let allPhaseData = { labels: [], temp: [], hum: [], modes: [] };

const MAX_LIVE = 60;

let chartT, chartH, chartA;

function initCharts() {
  const tCtx = document.getElementById('chartTemp').getContext('2d');
  chartT = new Chart(tCtx, {
    type: 'line',
    data: { labels: [], datasets: [{
      label: 'Suhu', data: [],
      borderColor: '#ef4444', backgroundColor: 'rgba(239,68,68,.1)',
      tension: .35, fill: true, pointRadius: 2, borderWidth: 2
    }]},
    options: {
      responsive: true, maintainAspectRatio: false,
      plugins: { legend: { display: false } },
      scales: {
        x: { ticks: { maxTicksLimit: 8, font: { size: 10 } } },
        y: { ticks: { font: { size: 10 } }, title: { display: true, text: '°C', font: { size: 10 } } }
      }
    }
  });

  const hCtx = document.getElementById('chartHum').getContext('2d');
  chartH = new Chart(hCtx, {
    type: 'line',
    data: { labels: [], datasets: [{
      label: 'RH', data: [],
      borderColor: '#0891b2', backgroundColor: 'rgba(8,145,178,.08)',
      tension: .35, fill: true, pointRadius: 2, borderWidth: 2
    }]},
    options: {
      responsive: true, maintainAspectRatio: false,
      plugins: { legend: { display: false } },
      scales: {
        x: { ticks: { maxTicksLimit: 8, font: { size: 10 } } },
        y: { min: 0, max: 100, ticks: { font: { size: 10 } }, title: { display: true, text: '%', font: { size: 10 } } }
      }
    }
  });

  const aCtx = document.getElementById('chartAll').getContext('2d');
  chartA = new Chart(aCtx, {
    type: 'line',
    data: { labels: [], datasets: [
      { label: 'Suhu (°C)', data: [], borderColor: '#ef4444', pointRadius: 0, borderWidth: 1.5, tension: .35, yAxisID: 'y' },
      { label: 'RH (%)', data: [], borderColor: '#0891b2', pointRadius: 0, borderWidth: 1.5, tension: .35, yAxisID: 'y2', borderDash: [4,2] }
    ]},
    options: {
      responsive: true, maintainAspectRatio: false,
      plugins: { legend: { display: false } },
      scales: {
        x: { ticks: { maxTicksLimit: 10, font: { size: 10 } } },
        y:  { position: 'left',  title: { display: true, text: '°C', font: { size: 10 } }, ticks: { font: { size: 10 } } },
        y2: { position: 'right', title: { display: true, text: '%RH', font: { size: 10 } }, min: 0, max: 100, ticks: { font: { size: 10 } }, grid: { drawOnChartArea: false } }
      }
    }
  });
}

function updateCharts(entry) {
  if (entry.temp === null || entry.hum === null) return;
  if (isNaN(entry.temp) || isNaN(entry.hum)) return;
  const t = entry.time;
  const showLabel = logData.length % 5 === 0 ? t : '';

  tempData.labels.push(showLabel);
  tempData.data.push(entry.temp);
  humData.labels.push(showLabel);
  humData.data.push(entry.hum);

  if (tempData.labels.length > MAX_LIVE) {
    tempData.labels.shift(); tempData.data.shift();
    humData.labels.shift(); humData.data.shift();
  }

  chartT.data.labels = tempData.labels;
  chartT.data.datasets[0].data = tempData.data;
  chartT.update('none');

  chartH.data.labels = humData.labels;
  chartH.data.datasets[0].data = humData.data;
  chartH.update('none');

  allPhaseData.labels.push(showLabel);
  allPhaseData.temp.push(entry.temp);
  allPhaseData.hum.push(entry.hum);
  allPhaseData.modes.push(entry.modeName);

  chartA.data.labels = allPhaseData.labels;
  chartA.data.datasets[0].data = allPhaseData.temp;
  chartA.data.datasets[1].data = allPhaseData.hum;
  chartA.update('none');
}

function updateSetpoints() {
  const m = MODES[currentMode];
  document.getElementById('sp-temp').innerHTML = `
    <span><span class="sp-dot" style="background:#ef4444"></span>Min: ${m.tempMin}°C</span>
    <span><span class="sp-dot" style="background:#f97316"></span>Max: ${m.tempMax}°C</span>`;
  document.getElementById('sp-hum').innerHTML = m.useMist ?
    `<span><span class="sp-dot" style="background:#0891b2"></span>Min: ${m.rhMin}%</span>
     <span><span class="sp-dot" style="background:#7c3aed"></span>Max: ${m.rhMax}%</span>` :
    `<span style="color:var(--muted)">Kelembaban tidak dikontrol di fase ini</span>`;
}

function updateMetrics(entry) {
  const m = MODES[currentMode];
  document.getElementById('m-temp').textContent = entry.temp.toFixed(1) + '°C';
  document.getElementById('m-temp-sub').textContent = `Setpoint: ${m.tempMin}–${m.tempMax}°C`;
  document.getElementById('m-hum').textContent = entry.hum.toFixed(1) + '%';
  document.getElementById('m-hum-sub').textContent = m.useMist ? `Setpoint: ${m.rhMin}–${m.rhMax}%` : 'Tidak dikontrol';
  document.getElementById('m-mode').textContent = m.name;
  document.getElementById('m-mode-sub').textContent = m.useFan ? 'Kipas aktif terus' : (m.useLED ? 'LED 06:00–18:00' : '');
  document.getElementById('m-vjam').textContent = String(getVirtualHour()).padStart(2,'0') + ':00';
  document.getElementById('m-total').textContent = logData.length;

   // ← tambah ini
  const tempCard = document.getElementById('m-temp').closest('.metric-card');
  const tempOk = entry.temp >= m.tempMin && entry.temp <= m.tempMax;
  tempCard.style.background = tempOk ? '' : '#fee2e2';
  tempCard.style.borderColor = tempOk ? '' : '#dc2626';

  document.getElementById('m-hum').textContent = entry.hum.toFixed(1) + '%';
  document.getElementById('m-hum-sub').textContent = m.useMist ? `Setpoint: ${m.rhMin}–${m.rhMax}%` : 'Tidak dikontrol';

  // ← tambah ini
  const humCard = document.getElementById('m-hum').closest('.metric-card');
  const humOk = !m.useMist || (entry.hum >= m.rhMin && entry.hum <= m.rhMax);
  humCard.style.background = humOk ? '' : '#fee2e2';
  humCard.style.borderColor = humOk ? '' : '#dc2626';
  
}

function updateActuators(act) {
  function setB(id, on) {
    const el = document.getElementById(id);
    el.className = 'actuator-badge ' + (on ? 'on' : 'off');
  }
  function setPctB(id, pct) {
    const el = document.getElementById(id);
    el.className = 'actuator-badge ' + (pct > 0 ? 'on' : 'off');
    const pctEl = el.querySelector('.badge-pct');
    if (pctEl) pctEl.textContent = Math.round(pct) + '%';
  }
  setPctB('b-peltier', act.peltierPct ?? 0);
  setPctB('b-fanhs',   act.fanHSPct ?? 0);
  setB('b-mist', act.mistOn);
  setB('b-led',  act.ledOn);
  setB('b-fan',  act.fanOn);
}

function renderTable() {
  const fMode = document.getElementById('filter-mode').value;
  const fTime = document.getElementById('search-time').value.trim().toLowerCase();

  let filtered = logData.filter(r => {
    if (fMode !== 'all' && r.modeName !== fMode) return false;
    if (fTime && !r.time.toLowerCase().includes(fTime)) return false;
    return true;
  });

  let selectedMode = chartMode === 'all' ? null : parseInt(chartMode);
  if (selectedMode !== null) {
    filtered = filtered.filter(r => r.modeIdx === selectedMode);
  }

  const tbody = document.getElementById('table-body');
  if (filtered.length === 0) {
    tbody.innerHTML = '<tr><td colspan="11" style="text-align:center;padding:24px;color:var(--muted)">Tidak ada data</td></tr>';
    document.getElementById('table-footer').textContent = '0 baris';
    return;
  }

  const totalPages = Math.ceil(filtered.length / ROWS_PER_PAGE);
  const start = (currentPage - 1) * ROWS_PER_PAGE;
  const rows = filtered.slice().reverse().slice(start, start + ROWS_PER_PAGE);
  tbody.innerHTML = rows.map((r, i) => `
    <tr>
      <td style="color:var(--muted)">${filtered.length - i}</td>
      <td>${r.time}</td>
      <td>${String(r.vjam).padStart(2,'0')}:00</td>
      <td><span class="phase-badge ${BADGE_CLASS[r.modeIdx]}">${r.modeName}</span></td>
      <td style="font-weight:500">${r.temp.toFixed(1)}</td>
      <td style="font-weight:500">${r.hum.toFixed(1)}</td>
      <td class="${r.peltierPct>0?'status-on':'status-off'}">${Math.round(r.peltierPct)}%</td>
      <td class="${r.fanHSPct>0?'status-on':'status-off'}">${Math.round(r.fanHSPct)}%</td>
      <td class="${r.mistOn?'status-on':'status-off'}">${r.mistOn?'ON':'off'}</td>
      <td class="${r.ledOn?'status-on':'status-off'}">${r.ledOn?'ON':'off'}</td>
      <td class="${r.fanOn?'status-on':'status-off'}">${r.fanOn?'ON':'off'}</td>
    </tr>`).join('');
  document.getElementById('table-footer').innerHTML = `
  <button onclick="changePage(-1)" ${currentPage===1?'disabled':''}>← Prev</button>
  Halaman ${currentPage} / ${totalPages}
  <button onclick="changePage(1)" ${currentPage===totalPages?'disabled':''}>Next →</button>
  &nbsp;(${filtered.length} total baris)
`;
}

function changePage(dir) {
  const fMode = document.getElementById('filter-mode').value;
  const fTime = document.getElementById('search-time').value.trim().toLowerCase();
  let filtered = logData.filter(r => {
    if (fMode !== 'all' && r.modeName !== fMode) return false;
    if (fTime && !r.time.toLowerCase().includes(fTime)) return false;
    return true;
  });
  const totalPages = Math.ceil(filtered.length / ROWS_PER_PAGE);
  currentPage = Math.min(Math.max(currentPage + dir, 1), totalPages);
  renderTable();
}

function tick() {
  if (!simRunning) return;
  const { temp, hum } = simulateSensor();
  const act = controlActuators(temp, hum);
  const entry = {
    time: now_str(),
    datetime: now_date_str(),
    vjam: getVirtualHour(),
    modeIdx: currentMode,
    modeName: MODES[currentMode].name,
    temp, hum,
    peltierPct: act.peltierPct,
    fanHSPct: act.fanHSPct,
    mistOn: act.mistOn,
    ledOn: act.ledOn,
    fanOn: act.fanOn
  };
  logData.push(entry);
  updateCharts(entry);
  updateMetrics(entry);
  updateActuators(act);
  updatePhaseNotif();
  renderTable();
}

function exportXLSX() {
  if (logData.length === 0) { alert('Belum ada data'); return; }
  const wb = XLSX.utils.book_new();

  const modes = [0,1,2,3];
  modes.forEach(mi => {
    const rows = logData.filter(r => r.modeIdx === mi);
    if (rows.length === 0) return;
    const ws_data = [
      ['Waktu', 'Jam Virtual', 'Mode', 'Suhu (°C)', 'Kelembaban (%)', 'Peltier (%)', 'Fan Heatsink (%)', 'Mist Maker', 'LED Grow', 'Kipas Exhaust'],
      ...rows.map(r => [
        r.datetime, `${String(r.vjam).padStart(2,'0')}:00`, r.modeName,
        r.temp, r.hum,
        r.peltierPct, r.fanHSPct,
        r.mistOn?'ON':'OFF', r.ledOn?'ON':'OFF', r.fanOn?'ON':'OFF'
      ])
    ];
    const ws = XLSX.utils.aoa_to_sheet(ws_data);
    ws['!cols'] = [16,12,18,12,16,12,16,12,10,12].map(w => ({ wch: w }));
    XLSX.utils.book_append_sheet(wb, ws, MODES[mi].name.replace('/', '-'));
  });

  const allRows = logData;
  const ws_all = XLSX.utils.aoa_to_sheet([
    ['Waktu', 'Jam Virtual', 'Mode', 'Suhu (°C)', 'Kelembaban (%)', 'Peltier (%)', 'Fan Heatsink (%)', 'Mist Maker', 'LED Grow', 'Kipas Exhaust'],
    ...allRows.map(r => [
      r.datetime, `${String(r.vjam).padStart(2,'0')}:00`, r.modeName,
      r.temp, r.hum,
      r.peltierPct, r.fanHSPct,
      r.mistOn?'ON':'OFF', r.ledOn?'ON':'OFF', r.fanOn?'ON':'OFF'
    ])
  ]);
  ws_all['!cols'] = [16,12,18,12,16,12,16,12,10,12].map(w => ({ wch: w }));
  XLSX.utils.book_append_sheet(wb, ws_all, 'Semua Fase');

  const fname = `EnokiChamber_${new Date().toISOString().slice(0,10)}.xlsx`;
  XLSX.writeFile(wb, fname);
}

function exportCSV() {
  if (logData.length === 0) { alert('Belum ada data'); return; }
  const header = 'Waktu,Jam Virtual,Mode,Suhu (°C),Kelembaban (%),Peltier (%),Fan Heatsink (%),Mist Maker,LED Grow,Kipas Exhaust\n';
  const rows = logData.map(r =>
    `${r.datetime},${String(r.vjam).padStart(2,'0')}:00,${r.modeName},${r.temp},${r.hum},` +
    `${r.peltierPct},${r.fanHSPct},${r.mistOn?'ON':'OFF'},${r.ledOn?'ON':'OFF'},${r.fanOn?'ON':'OFF'}`
  ).join('\n');
  const blob = new Blob([header + rows], { type: 'text/csv;charset=utf-8;' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url; a.download = `EnokiChamber_${new Date().toISOString().slice(0,10)}.csv`;
  a.click();
  URL.revokeObjectURL(url);
}

document.getElementById('btn-export-xlsx').addEventListener('click', exportXLSX);
document.getElementById('btn-export-csv').addEventListener('click', exportCSV);

document.getElementById('btn-clear').addEventListener('click', () => {
  if (!confirm('Hapus semua data log?')) return;
  logData = [];
  tempData = { labels: [], data: [] };
  humData  = { labels: [], data: [] };
  allPhaseData = { labels: [], temp: [], hum: [], modes: [] };
  chartT.data.labels = []; chartT.data.datasets[0].data = []; chartT.update();
  chartH.data.labels = []; chartH.data.datasets[0].data = []; chartH.update();
  chartA.data.labels = []; chartA.data.datasets[0].data = []; chartA.data.datasets[1].data = []; chartA.update();
  renderTable();
});

document.getElementById('filter-mode').addEventListener('change', () => {
  currentPage = 1; renderTable();
});
document.getElementById('search-time').addEventListener('input', () => {
  currentPage = 1; renderTable();
});

document.querySelectorAll('.mode-tab').forEach(tab => {
  tab.addEventListener('click', () => {
    const mv = tab.dataset.mode;

    if (mv === 'all') {
      // "Semua Fase" murni filter tampilan, TIDAK mengubah fase aktif di alat.
      document.querySelectorAll('.mode-tab').forEach(t => t.classList.remove('active'));
      tab.classList.add('active');
      chartMode = 'all';
      renderTable();
      return;
    }

    // Tab fase spesifik (M1/M2a/M2b/M2c) = perintah GANTI FASE sungguhan,
    // dikirim ke ESP32 lewat MQTT (bukan cuma ubah tampilan lokal).
    changeToMode(parseInt(mv));
    renderTable();
  });
});

document.getElementById('btn-next-phase').addEventListener('click', nextPhase);


setInterval(() => {
  const now = new Date();
  document.getElementById('clock-display').textContent = now.toLocaleTimeString('id-ID', { hour12: false });
}, 1000);

initCharts();
updateSetpoints();
updatePhaseNotif();

tick();
setInterval(tick, 60000);

setTimeout(() => {
  for (let i = 0; i < 5; i++) {
    setTimeout(() => { tick(); }, i * 200);
  }
}, 500);

let mqttData = { temp: null, hum: null, peltierPct: 0, fanHSPct: 0, mistOn: false, ledOn: false, fanOn: false };

try {
  mqttClient = mqtt.connect("wss://test.mosquitto.org:8081", {
    connectTimeout: 8000,
    reconnectPeriod: 3000
  });

  mqttClient.on("connect", () => {
    console.log("MQTT Connected (broker) — menunggu data pertama dari device...");
    mqttClient.subscribe("enoki/temperature");
    mqttClient.subscribe("enoki/humidity");
    mqttClient.subscribe("enoki/mode");
    mqttClient.subscribe("enoki/peltier");
    mqttClient.subscribe("enoki/fanheatsink");
    mqttClient.subscribe("enoki/mist");
    mqttClient.subscribe("enoki/fan");
    mqttClient.subscribe("enoki/led");
    mqttClient.subscribe("enoki/phase_elapsed_days");
    mqttClient.subscribe("enoki/notif");
  });

  mqttClient.on("error", (err) => {
    console.error("MQTT error, dashboard tetap jalan mode simulasi lokal:", err);
  });
} catch (e) {
  console.error("Gagal inisialisasi MQTT (mungkin mqtt.js CDN gagal load) — dashboard tetap jalan mode simulasi lokal:", e);
}

if (mqttClient) {
mqttClient.on("message", (topic, message) => {
  const value = message.toString();
  lastDeviceMsgAt = Date.now();   // heartbeat: device benar-benar mengirim data

  
  switch (topic) {
    case "enoki/mode":
      if (parseInt(value) !== currentMode) {
        currentMode = parseInt(value);
        resetPhaseTimer();   // fase baru dikonfirmasi device -> reset hitungan hari
      }
      updateSetpoints();
      updatePhaseNotif();
      syncModeTabsUI();
      return;
    case "enoki/phase_elapsed_days":
      deviceElapsedDays = parseFloat(value);   // device = sumber kebenaran umur fase
      updatePhaseNotif();
      return;
    case "enoki/notif":
      // "MIN_REACHED" dari device — banner sudah dihitung ulang otomatis
      // lewat updatePhaseNotif(), jadi tidak perlu aksi tambahan di sini.
      return;
    case "enoki/temperature":  mqttData.temp       = parseFloat(value); break;
    case "enoki/humidity":     mqttData.hum        = parseFloat(value); break;
    case "enoki/peltier":      mqttData.peltierPct = parseFloat(value); break;
    case "enoki/fanheatsink":  mqttData.fanHSPct   = parseFloat(value); break;
    case "enoki/mist":         mqttData.mistOn     = value === "1"; break;
    case "enoki/fan":          mqttData.fanOn      = value === "1"; break;
    case "enoki/led":          mqttData.ledOn      = value === "1"; break;
  }

  if (mqttData.temp !== null && mqttData.hum !== null) {
    simRunning = false; // data real pertama sudah masuk, hentikan simulasi lokal
    const entry = {
      time: now_str(), datetime: now_date_str(),
      vjam: getVirtualHour(), modeIdx: currentMode,
      modeName: MODES[currentMode].name,
      temp: mqttData.temp, hum: mqttData.hum,
      peltierPct: mqttData.peltierPct, fanHSPct: mqttData.fanHSPct,
      mistOn: mqttData.mistOn,
      ledOn: mqttData.ledOn, fanOn: mqttData.fanOn
    };
    
    logData.push(entry);
    updateCharts(entry);
    updateMetrics(entry);
    updateActuators(mqttData);
    updatePhaseNotif();
    renderTable();
  }
});
}