from pathlib import Path
import re

import pandas as pd
import matplotlib.pyplot as plt

# ---------- Konfigurasi ----------
BASE_DIR = Path(__file__).resolve().parent
DATA_DIR = BASE_DIR.parent / "visualization"
OUT_DIR = BASE_DIR / "output"
OUT_DIR.mkdir(exist_ok=True)

LOGS = {
    "THERMALMODE": DATA_DIR / "[THERMALMODE] [TEST] serial monitor 1.txt",
    "ENVINJECT": DATA_DIR / "[ENVINJECT] [TEST] serial monitor 1.txt",
}
RATE_LIMIT = 15    # batas perubahan PLT/FHS per baris log (%)
MIN_MIST_ON = 10   # batas minimum ON mist maker (jumlah baris log)

# mode: (suhu_min, suhu_maks, setpoint, rh_min, rh_maks)
PHASES = {
    "M1 Inkubasi":     (20.0, 23.0, 21.5, 60.0, 70.0),
    "M2a Induksi":     (10.0, 12.0, 11.0, 80.0, 85.0),
    "M2b Pemanjangan": (12.0, 15.0, 13.5, 75.0, 80.0),
    "M2c Fruiting":    (8.0, 12.0, 10.0, 80.0, 90.0),
}
LIMITS = pd.DataFrame(PHASES, index=["tmin", "tmax", "sp", "rhmin", "rhmax"]).T

PATTERN = re.compile(
    r"\[T=([-\d.]+|nan)C H=([-\d.]+|nan)% \| PLT=(\d+)% FHS=(\d+)% \| "
    r"dT=([-\d.]+) \| MST=(\w+) LED=(\w+) FAN=(\w+) \| Jam=(\d+) \| Mode=(.+?)\]"
)


def parse_log(path):
    if not path.exists():
        raise FileNotFoundError(f"File tidak ditemukan: {path}")
    rows, run, new_run = [], 0, True
    for line in open(path, encoding="utf-8", errors="ignore"):
        if "POWERON_RESET" in line:  # reboot ESP32 -> sesi baru
            new_run = True
            continue
        m = PATTERN.search(line)
        if not m or m[10].strip() not in PHASES:
            continue
        if new_run:
            run, new_run = run + 1, False
        rows.append({"run": run, "T": float(m[1]), "H": float(m[2]),
                     "PLT": float(m[3]), "FHS": float(m[4]),
                     "mist": m[6] == "ON", "mode": m[10].strip()})
    if not rows:
        raise ValueError(f"Tidak ada data yang bisa diparse dari: {path}")
    df = pd.DataFrame(rows).join(LIMITS, on="mode")
    df.insert(0, "sample", range(1, len(df) + 1))
    return df


def stability_metrics(df):
    v = df.dropna(subset=["T", "H"]).copy()
    v["err"] = v["T"] - v["sp"]
    v["t_ok"] = v["T"].between(v["tmin"], v["tmax"])
    v["h_ok"] = v["H"].between(v["rhmin"], v["rhmax"])
    g = v.groupby("mode", sort=False)
    return pd.DataFrame({
        "Sampel": g.size(),
        "T rata2": g["T"].mean(),
        "T akhir": g["T"].last(),
        "MAE": g["err"].apply(lambda e: e.abs().mean()),
        "RMSE": g["err"].apply(lambda e: (e ** 2).mean() ** 0.5),
        "T in-range %": g["t_ok"].mean() * 100,
        "RH in-range %": g["h_ok"].mean() * 100,
    }).round(2)


def mist_on_durations(df):  # hanya siklus ON lengkap (tidak terpotong ujung sesi)
    durations = []
    for _, g in df.groupby("run"):
        block = (g["mist"] != g["mist"].shift()).cumsum()
        for _, b in g.groupby(block):
            terpotong = b.index[0] == g.index[0] or b.index[-1] == g.index[-1]
            if b["mist"].iloc[0] and not terpotong:
                durations.append(len(b))
    return durations


def plot_test(name, df):
    x = df["sample"]
    fig, ax = plt.subplots(4, 1, figsize=(15, 12), sharex=True)

    ax[0].plot(x, df["T"], marker="o", ms=2.5, lw=1, label="Temperature")
    ax[0].plot(x, df["sp"], "--", lw=1.5, label="Setpoint")
    ax[1].plot(x, df["H"], marker="o", ms=2.5, lw=1, label="Humidity")
    for a, lo, hi in ((ax[0], "tmin", "tmax"), (ax[1], "rhmin", "rhmax")):
        a.plot(x, df[lo], ":", c="gray", label="Batas min/maks")
        a.plot(x, df[hi], ":", c="gray")
    ax[2].plot(x, df["PLT"], lw=1, label="Peltier (%)")
    ax[2].plot(x, df["FHS"], lw=1, alpha=0.8, label="Fan Heatsink (%)")
    ax[3].step(x, df["mist"].astype(int), where="post", c="purple", label="Mist Maker")
    ax[3].set_yticks([0, 1], ["OFF", "ON"])
    for a, label in zip(ax, ["Temperature (°C)", "Relative Humidity (%)",
                             "Output (%)", "Mist Maker"]):
        a.set_ylabel(label)
        a.grid(True, alpha=0.3)
        a.legend(loc="upper right")
    ax[3].set_xlabel("Sample")
    # garis putus = ganti mode, garis solid = ganti sesi/reboot
    for i in range(1, len(df)):
        run_ganti = df["run"].iloc[i] != df["run"].iloc[i - 1]
        if run_ganti or df["mode"].iloc[i] != df["mode"].iloc[i - 1]:
            for a in ax:
                a.axvline(x.iloc[i], ls="-" if run_ganti else ":", c="gray", alpha=0.7)

    fig.suptitle(f"Enoki Chamber Monitoring - {name}", fontsize=14)
    fig.tight_layout()
    out = OUT_DIR / f"{name.lower()}_timeseries.png"
    fig.savefig(out, dpi=300, bbox_inches="tight")
    plt.close(fig)
    print(f"Grafik tersimpan: {out}")


def main():
    for name, path in LOGS.items():
        print(f"\n{'=' * 60}\n{name}\n{'=' * 60}")
        try:
            df = parse_log(path)
        except (FileNotFoundError, ValueError) as e:
            print(f"ERROR: {e}")
            continue

        print(f"Sampel: {len(df)} | Sesi: {df['run'].nunique()}")
        plot_test(name, df)
        print("\nSTABILITY METRICS")
        print(stability_metrics(df).to_string())

        delta = df.groupby("run")[["PLT", "FHS"]].diff().abs()
        print(f"\nRATE LIMITER: {int((delta > RATE_LIMIT).any(axis=1).sum())} baris "
              f"melebihi {RATE_LIMIT}% per baris log")

        on = mist_on_durations(df)
        if on:
            pendek = sum(d < MIN_MIST_ON for d in on)
            print(f"MIST MAKER: {len(on)} siklus ON lengkap, durasi {min(on)}-{max(on)} "
                  f"baris, {pendek} lebih pendek dari {MIN_MIST_ON}")
        else:
            print("MIST MAKER: tidak ada siklus ON lengkap")


if __name__ == "__main__":
    main()