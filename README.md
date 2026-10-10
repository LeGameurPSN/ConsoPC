# ⚡ PowerMon

**Moniteur de consommation électrique en temps réel — Windows 11**

```
  ⚡  POWERMON  —  Moniteur de consommation Windows 11
     Session : 00:04:32  |  Rafraîchissement : 1 Hz

┌─ PUISSANCE TOTALE ESTIMÉE ────────────────────────────────────────┐
│ 187.4 W    min 142.1  moy 163.8  max 211.7
│ ▁▂▃▄▅▅▆▇▇▆▅▄▃▃▄▅▆▇█▇▆▅▄▃▂▂▃▄▅▆▇▇▆▅▄▃▄▅▆▇
└───────────────────────────────────────────────────────────────────┘

┌─ CPU ─────────────────────────────────────────────────────────────┐
│ Usage : 42.3%  [########------------]  Estimation : 41.7 W
└───────────────────────────────────────────────────────────────────┘

┌─ GPU ─────────────────────────────────────────────────────────────┐
│ AMD Radeon RX 580 Series          Usage : 78.1%
│ [################----]  Estimation : 144.0 W  (TDP max 185 W)
└───────────────────────────────────────────────────────────────────┘

┌─ MÉMOIRE RAM ─────────────────────────────────────────────────────┐
│ 11.2 / 16.0 Go (70%)  [##############------]  ~7.0 W
└───────────────────────────────────────────────────────────────────┘

┌─ PÉRIPHÉRIQUES USB (estimation) ──────────────────────────────────┐
│ USB Audio Device                         ~1.0 W
│ HID-compliant mouse                      ~0.5 W
│ HID Keyboard Device                      ~0.5 W
├───────────────────────────────────────────────────────────────────┤
│ Total USB                                ~2.0 W
└───────────────────────────────────────────────────────────────────┘

┌─ BILAN ÉNERGÉTIQUE ───────────────────────────────────────────────┐
│ Cumulé   : 12.3456 Wh  (0.01234560 kWh)
│ Coût     : 0.00311 €  (tarif 0.2516 €/kWh)
│ Projection 24h : 0.988 €   mois : 29.64 €
├───────────────────────────────────────────────────────────────────┤
│ CPU 41.7W  GPU 144.0W  RAM 7.0W  USB 2.0W  Sys 45W
└───────────────────────────────────────────────────────────────────┘
```

---

## 📦 Télécharger l'exécutable

> **[⬇ Dernière release → powermon.exe](../../releases/latest)**

Aucune installation, aucune dépendance. Lance directement.

---

## 🔧 Ce que lit PowerMon

| Source | Méthode |
|--------|---------|
| **CPU %** | PDH `\Processor(_Total)\% Processor Time` — valeur noyau réelle |
| **GPU %** | PDH `\GPU Engine(*)\Utilization Percentage` — Win10/11 natif |
| **GPU nom** | WMI `Win32_VideoController` |
| **RAM** | `GlobalMemoryStatusEx` |
| **USB** | `SetupDiGetClassDevs` → liste + estimation par classe |
| **Puissance** | TDP constructeur × % usage (modèle linéaire) |

### Précision

| Composant | Précision typique |
|-----------|-----------------|
| CPU | ±5 W (linéaire sur TDP) |
| GPU | ±15 W (courbe réelle non-linéaire) |
| USB | ±50 % (estimation par classe) |
| RAM | ±2 W |
| **Total** | **±15-25 W** |

Pour la puissance réelle à la prise : prise connectée **Shelly Plug S** ou **TP-Link Kasa EP25**.

---

## ⚙️ Configuration matérielle

Édite `src/main.cpp`, section `CONSTANTES MATÉRIELLES` :

```cpp
static constexpr double GPU_TDP_MAX  = 185.0;  // RX 580 → change si autre GPU
static constexpr double GPU_TDP_IDLE =  12.0;
static constexpr double CPU_TDP_MAX  =  95.0;  // adapte à ton CPU
static constexpr double CPU_TDP_IDLE =   8.0;
static constexpr double SYS_BASE     =  45.0;  // carte mère + stockage + ventilos
static constexpr double TARIF_KWH    =   0.2516; // €/kWh
```

---

## 🛠️ Compiler soi-même

### Prérequis
- Visual Studio 2022 (avec "Développement Desktop C++")
- CMake ≥ 3.20

### Commandes

```bat
:: Dans "Developer Command Prompt for VS 2022"
cmake -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
build\powermon.exe
```

### Log CSV

```bat
powermon.exe --csv
:: → génère powermon_log.csv avec timestamp, W, Wh, coût
```

---

## 🚀 GitHub Actions

Chaque push sur `main` compile automatiquement l'exe.
L'artifact est téléchargeable depuis l'onglet **Actions → powermon-windows-x64**.

Pour créer une release avec l'exe en pièce jointe :
1. `git tag v1.0.0 && git push --tags`
2. Crée une Release sur GitHub → l'action publie `powermon.exe`

---

## 📋 Roadmap

- [ ] Lecture registre AMD pour la puissance GPU réelle (via ADL SDK)
- [ ] Support NVIDIA (NVML)
- [ ] Intégration Shelly Plug via API REST (puissance prise réelle)
- [ ] Export JSON
- [ ] Mode `--interval N` pour changer la fréquence

---

## Licence

MIT — fais-en ce que tu veux.
