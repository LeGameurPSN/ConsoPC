/*
 * PowerMon - Moniteur de consommation électrique Windows 11
 * Lecture réelle via : WMI, PDH, SetupAPI USB, NVML-like pour GPU AMD
 * Compile : MSVC 2022 ou MinGW-w64 (CMake)
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <powrprof.h>
#include <setupapi.h>
#include <devguid.h>
#include <cfgmgr32.h>
#include <comdef.h>
#include <wbemidl.h>
#include <psapi.h>
#include <intrin.h>

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <array>
#include <thread>
#include <chrono>
#include <numeric>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <cstdint>
#include <cassert>
#include <cmath>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "psapi.lib")

// ─────────────────────────────────────────────────────────────
//  CONSTANTES MATÉRIELLES  (ajuste si nécessaire)
// ─────────────────────────────────────────────────────────────
static constexpr double GPU_TDP_MAX   = 185.0;   // W  RX 580 TDP officiel AMD
static constexpr double GPU_TDP_IDLE  =  12.0;   // W  GPU au repos
static constexpr double CPU_TDP_MAX   =  95.0;   // W  à 100% charge (modifie selon ton proc)
static constexpr double CPU_TDP_IDLE  =   8.0;   // W  CPU au repos
static constexpr double SYS_BASE      =  45.0;   // W  carte mère + RAM + stockage + ventilos
static constexpr double TARIF_KWH     =   0.2516; // €/kWh EDF HP 2024

static constexpr int    REFRESH_MS    = 1000;
static constexpr int    HISTORY_SIZE  = 60;

// ─────────────────────────────────────────────────────────────
//  CONSOLE : séquences ANSI
// ─────────────────────────────────────────────────────────────
#define ANSI_RESET   "\033[0m"
#define ANSI_BOLD    "\033[1m"
#define ANSI_DIM     "\033[2m"
#define CYAN         "\033[96m"
#define ORANGE       "\033[38;5;208m"
#define GREEN        "\033[92m"
#define RED          "\033[91m"
#define YELLOW       "\033[93m"
#define WHITE        "\033[97m"
#define GRAY         "\033[90m"
#define BG_BLACK     "\033[40m"

static void enableAnsi() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  mode = 0;
    GetConsoleMode(h, &mode);
    SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleOutputCP(CP_UTF8);
}
static void clearScreen() { std::cout << "\033[2J\033[H"; }
static void moveCursor(int row, int col) {
    std::cout << "\033[" << row << ";" << col << "H";
}
static void hideCursor() { std::cout << "\033[?25l"; }
static void showCursor() { std::cout << "\033[?25h"; }

// ─────────────────────────────────────────────────────────────
//  LECTURE CPU via PDH (%Processor Time total)
// ─────────────────────────────────────────────────────────────
struct CpuReader {
    PDH_HQUERY   query   = nullptr;
    PDH_HCOUNTER counter = nullptr;
    bool         ok      = false;

    bool init() {
        if (PdhOpenQuery(nullptr, 0, &query) != ERROR_SUCCESS) return false;
        if (PdhAddEnglishCounterW(query,
                L"\\Processor(_Total)\\% Processor Time",
                0, &counter) != ERROR_SUCCESS) return false;
        PdhCollectQueryData(query);   // première collecte
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ok = true;
        return true;
    }

    // Retourne usage CPU en % (0-100)
    double read() {
        if (!ok) return 0.0;
        PdhCollectQueryData(query);
        PDH_FMT_COUNTERVALUE val{};
        PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &val);
        return std::clamp(val.doubleValue, 0.0, 100.0);
    }

    ~CpuReader() { if (query) PdhCloseQuery(query); }
};

// ─────────────────────────────────────────────────────────────
//  LECTURE GPU AMD via WMI (Win32_VideoController + perf)
//  + fallback estimation TDP si les perf ne sont pas dispo
// ─────────────────────────────────────────────────────────────
struct GpuInfo {
    std::wstring name;
    double       usagePct  = 0.0;   // % engine (via PDH ou WMI)
    double       powerW    = 0.0;   // W estimé
    bool         found     = false;
};

struct GpuReader {
    PDH_HQUERY   query    = nullptr;
    PDH_HCOUNTER ctr3D    = nullptr;   // GPU Engine 3D
    PDH_HCOUNTER ctrCopy  = nullptr;
    PDH_HCOUNTER ctrDec   = nullptr;
    bool         pdhOk    = false;
    std::wstring gpuName;

    bool initPdh() {
        if (PdhOpenQuery(nullptr, 0, &query) != ERROR_SUCCESS) return false;
        // Windows 10/11 expose les GPU via "GPU Engine"
        if (PdhAddEnglishCounterW(query,
                L"\\GPU Engine(*)\\Utilization Percentage",
                0, &ctr3D) == ERROR_SUCCESS) {
            PdhCollectQueryData(query);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            pdhOk = true;
        }
        return pdhOk;
    }

    // Nom GPU via WMI
    void detectGpuName() {
        IWbemLocator*  pLoc  = nullptr;
        IWbemServices* pSvc  = nullptr;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        CoInitializeSecurity(nullptr,-1,nullptr,nullptr,
            RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr, EOAC_NONE, nullptr);
        CoCreateInstance(CLSID_WbemLocator,0,CLSCTX_INPROC_SERVER,
            IID_IWbemLocator,(LPVOID*)&pLoc);
        if (!pLoc) return;
        pLoc->ConnectServer(_bstr_t(L"ROOT\\CIMV2"),
            nullptr,nullptr,0,0,0,0,&pSvc);
        if (!pSvc) { pLoc->Release(); return; }
        CoSetProxyBlanket(pSvc,RPC_C_AUTHN_WINNT,RPC_C_AUTHZ_NONE,
            nullptr,RPC_C_AUTHN_LEVEL_CALL,RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr,EOAC_NONE);
        IEnumWbemClassObject* pEnum = nullptr;
        pSvc->ExecQuery(_bstr_t(L"WQL"),
            _bstr_t(L"SELECT Name FROM Win32_VideoController"),
            WBEM_FLAG_FORWARD_ONLY|WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr,&pEnum);
        if (pEnum) {
            IWbemClassObject* pObj=nullptr; ULONG ret=0;
            while (pEnum->Next(WBEM_INFINITE,1,&pObj,&ret)==S_OK) {
                VARIANT v; VariantInit(&v);
                if (pObj->Get(L"Name",0,&v,0,0)==S_OK && v.vt==VT_BSTR) {
                    gpuName = v.bstrVal;
                }
                VariantClear(&v); pObj->Release();
            }
            pEnum->Release();
        }
        pSvc->Release(); pLoc->Release();
    }

    // Retourne usage GPU (%) via PDH counter GPU Engine (tous engines sommés/moyennés)
    double readUsage() {
        if (!pdhOk) return 0.0;
        PdhCollectQueryData(query);
        DWORD  bufSize = 0, itemCount = 0;
        PdhGetFormattedCounterArrayW(ctr3D, PDH_FMT_DOUBLE,
            &bufSize, &itemCount, nullptr);
        if (bufSize == 0) return 0.0;
        std::vector<BYTE> buf(bufSize);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
        if (PdhGetFormattedCounterArrayW(ctr3D, PDH_FMT_DOUBLE,
                &bufSize, &itemCount, items) != ERROR_SUCCESS) return 0.0;
        double total = 0.0;
        for (DWORD i = 0; i < itemCount; i++)
            total += items[i].FmtValue.doubleValue;
        return std::clamp(total, 0.0, 100.0);
    }

    GpuInfo read() {
        GpuInfo g;
        g.name     = gpuName.empty() ? L"GPU" : gpuName;
        g.usagePct = readUsage();
        g.powerW   = GPU_TDP_IDLE +
                     (GPU_TDP_MAX - GPU_TDP_IDLE) * (g.usagePct / 100.0);
        g.found    = true;
        return g;
    }

    ~GpuReader() { if (query) PdhCloseQuery(query); }
};

// ─────────────────────────────────────────────────────────────
//  LECTURE USB : périphériques actifs via SetupAPI
// ─────────────────────────────────────────────────────────────
struct UsbDevice {
    std::string name;
    double      powerW = 0.0;   // estimation selon classe USB
};

// Estimation consommation USB par classe de périphérique
static double estimateUsbPower(const std::string& devName) {
    std::string lo = devName;
    for (auto& c : lo) c = (char)tolower((unsigned char)c);
    if (lo.find("hub")        != std::string::npos) return 2.5;
    if (lo.find("camera")     != std::string::npos ||
        lo.find("webcam")     != std::string::npos) return 2.5;
    if (lo.find("headset")    != std::string::npos ||
        lo.find("audio")      != std::string::npos ||
        lo.find("microphone") != std::string::npos) return 1.0;
    if (lo.find("keyboard")   != std::string::npos) return 0.5;
    if (lo.find("mouse")      != std::string::npos) return 0.5;
    if (lo.find("storage")    != std::string::npos ||
        lo.find("disk")       != std::string::npos ||
        lo.find("ssd")        != std::string::npos) return 4.0;
    if (lo.find("controller") != std::string::npos ||
        lo.find("gamepad")    != std::string::npos) return 1.0;
    if (lo.find("bluetooth")  != std::string::npos) return 0.5;
    if (lo.find("printer")    != std::string::npos) return 3.0;
    if (lo.find("card reader") != std::string::npos) return 1.0;
    return 0.5;   // périphérique USB inconnu
}

static std::vector<UsbDevice> enumUsbDevices() {
    std::vector<UsbDevice> devs;
    HDEVINFO hDev = SetupDiGetClassDevsW(
        &GUID_DEVCLASS_USB, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (hDev == INVALID_HANDLE_VALUE) return devs;

    SP_DEVINFO_DATA did{};
    did.cbSize = sizeof(did);
    for (DWORD idx = 0; SetupDiEnumDeviceInfo(hDev, idx, &did); ++idx) {
        wchar_t buf[512]{};
        if (SetupDiGetDeviceRegistryPropertyW(hDev, &did,
                SPDRP_DEVICEDESC, nullptr,
                (PBYTE)buf, sizeof(buf)-2, nullptr)) {
            std::wstring ws(buf);
            std::string  name(ws.begin(), ws.end());
            // Filtrer les nœuds racine et contrôleurs hôtes
            if (name.find("Root Hub") == std::string::npos &&
                name.find("Host Controller") == std::string::npos &&
                name.find("Composite") == std::string::npos) {
                UsbDevice d;
                d.name   = name;
                d.powerW = estimateUsbPower(name);
                devs.push_back(d);
            }
        }
    }
    SetupDiDestroyDeviceInfoList(hDev);
    return devs;
}

// ─────────────────────────────────────────────────────────────
//  LECTURE RAM (% utilisée via GlobalMemoryStatusEx)
// ─────────────────────────────────────────────────────────────
struct RamInfo {
    double totalGB = 0;
    double usedGB  = 0;
    double usedPct = 0;
    double powerW  = 0;   // ~3W par 8 Go de DDR4
};

static RamInfo readRam() {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    RamInfo r;
    r.totalGB = (double)ms.ullTotalPhys / (1024.0*1024.0*1024.0);
    r.usedGB  = r.totalGB - (double)ms.ullAvailPhys/(1024.0*1024.0*1024.0);
    r.usedPct = ms.dwMemoryLoad;
    // DDR4 : ~3-4W par barrette 8 Go
    r.powerW  = (r.totalGB / 8.0) * 3.5;
    return r;
}

// ─────────────────────────────────────────────────────────────
//  HISTORIQUE PUISSANCE
// ─────────────────────────────────────────────────────────────
struct History {
    std::array<double, HISTORY_SIZE> data{};
    int    head = 0;
    int    count = 0;
    double cumWh = 0.0;

    void push(double val) {
        data[head] = val;
        head = (head + 1) % HISTORY_SIZE;
        if (count < HISTORY_SIZE) ++count;
        cumWh += val / 3600.0;   // W → Wh (à 1 Hz)
    }
    double last()   const { return count ? data[(head-1+HISTORY_SIZE)%HISTORY_SIZE] : 0; }
    double minVal() const {
        if (!count) return 0;
        double m = 1e9;
        for (int i=0;i<count;i++) m=std::min(m,data[(head-count+i+HISTORY_SIZE)%HISTORY_SIZE]);
        return m;
    }
    double maxVal() const {
        if (!count) return 0;
        double m = 0;
        for (int i=0;i<count;i++) m=std::max(m,data[(head-count+i+HISTORY_SIZE)%HISTORY_SIZE]);
        return m;
    }
    double avg() const {
        if (!count) return 0;
        double s = 0;
        for (int i=0;i<count;i++) s+=data[(head-count+i+HISTORY_SIZE)%HISTORY_SIZE];
        return s/count;
    }
};

// ─────────────────────────────────────────────────────────────
//  SPARKLINE (mini graphe ASCII 8 niveaux)
// ─────────────────────────────────────────────────────────────
static const char* SPARK = " ▁▂▃▄▅▆▇█";
static std::string sparkline(const History& h, int width = 40) {
    if (h.count < 2) return std::string(width, ' ');
    double mn = h.minVal(), mx = h.maxVal();
    if (mx == mn) mx = mn + 1;
    std::string out;
    int start = (h.head - std::min(h.count, width) + HISTORY_SIZE) % HISTORY_SIZE;
    for (int i = 0; i < std::min(h.count, width); i++) {
        int idx = (start + i) % HISTORY_SIZE;
        double norm = (h.data[idx] - mn) / (mx - mn);
        int level = (int)std::round(norm * 8);
        level = std::clamp(level, 0, 8);
        // Encode UTF-8 block char
        const uint32_t cp = 0x2580 + level - 1; // ▁..█
        if (level == 0) { out += ' '; continue; }
        // UTF-8 3-byte sequence for U+2580..U+2588
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
    return out;
}

// ─────────────────────────────────────────────────────────────
//  BARRE DE PROGRESSION
// ─────────────────────────────────────────────────────────────
static std::string bar(double pct, int w=20, const char* col=CYAN) {
    int filled = (int)std::round(pct / 100.0 * w);
    filled = std::clamp(filled, 0, w);
    std::string s;
    s += col;
    s += '[';
    for (int i=0;i<filled;i++) s += '#';
    s += GRAY;
    for (int i=filled;i<w;i++) s += '-';
    s += ANSI_RESET;
    s += ']';
    return s;
}

// ─────────────────────────────────────────────────────────────
//  FORMATAGE DURÉE
// ─────────────────────────────────────────────────────────────
static std::string fmtDuration(int64_t sec) {
    char buf[32];
    snprintf(buf,sizeof(buf),"%02lld:%02lld:%02lld",
        (long long)(sec/3600),(long long)((sec%3600)/60),(long long)(sec%60));
    return buf;
}

// ─────────────────────────────────────────────────────────────
//  AFFICHAGE PRINCIPAL
// ─────────────────────────────────────────────────────────────
static void render(
    double cpuPct, double cpuW,
    const GpuInfo& gpu,
    const RamInfo& ram,
    const std::vector<UsbDevice>& usbs,
    const History& hist,
    double totalW, double usbTotalW,
    int64_t elapsedSec)
{
    clearScreen();
    const int W = 70;
    std::string line(W, '─');

    auto hdr = [&](const std::string& t) {
        std::cout << CYAN << "┌─ " << WHITE << ANSI_BOLD << t
                  << ANSI_RESET << CYAN;
        int pad = W - 4 - (int)t.size();
        if (pad > 0) std::cout << std::string(pad,'─');
        std::cout << "┐" << ANSI_RESET << "\n";
    };
    auto row = [&](const std::string& s) {
        std::cout << CYAN << "│" << ANSI_RESET << " "
                  << s;
        // pad to W-2 visible chars (approximate)
        std::cout << "\n";
    };
    auto sep = [&]() {
        std::cout << CYAN << "├" << std::string(W-2,'─') << "┤" << ANSI_RESET << "\n";
    };
    auto bot = [&]() {
        std::cout << CYAN << "└" << std::string(W-2,'─') << "┘" << ANSI_RESET << "\n";
    };

    // ── TITRE ──
    std::cout << "\n";
    std::cout << CYAN << ANSI_BOLD
              << "  ⚡  POWERMON  —  Moniteur de consommation Windows 11\n"
              << ANSI_RESET;
    std::cout << GRAY << "     Session : " << fmtDuration(elapsedSec)
              << "  |  Rafraîchissement : 1 Hz\n" << ANSI_RESET << "\n";

    // ── PUISSANCE TOTALE ──
    hdr("PUISSANCE TOTALE ESTIMÉE");
    char buf[128];
    const char* col = totalW > 250 ? RED : totalW > 150 ? ORANGE : GREEN;
    snprintf(buf,sizeof(buf),
        " %s%s%.1f W%s    min %.1f  moy %.1f  max %.1f",
        ANSI_BOLD, col, totalW, ANSI_RESET,
        hist.minVal(), hist.avg(), hist.maxVal());
    row(buf);
    // Sparkline
    std::string sp = sparkline(hist, W-4);
    std::cout << CYAN << "│" << ANSI_RESET << " " << CYAN << sp << ANSI_RESET << "\n";
    bot();
    std::cout << "\n";

    // ── CPU ──
    hdr("CPU");
    snprintf(buf,sizeof(buf),
        " Usage : %s%.1f%%%s  %s  Estimation : %s%.1f W%s  (TDP max %.0f W)",
        ORANGE, cpuPct, ANSI_RESET,
        bar(cpuPct, 20, ORANGE).c_str(),
        ORANGE, cpuW, ANSI_RESET, CPU_TDP_MAX);
    row(buf);
    bot();
    std::cout << "\n";

    // ── GPU ──
    hdr("GPU");
    std::string gname(gpu.name.begin(), gpu.name.end());
    snprintf(buf,sizeof(buf),
        " %-28s  Usage : %.1f%%", gname.substr(0,28).c_str(), gpu.usagePct);
    row(buf);
    snprintf(buf,sizeof(buf),
        " %s  Estimation : %s%.1f W%s  (TDP max %.0f W)",
        bar(gpu.usagePct, 20, CYAN).c_str(),
        CYAN, gpu.powerW, ANSI_RESET, GPU_TDP_MAX);
    row(buf);
    bot();
    std::cout << "\n";

    // ── RAM ──
    hdr("MÉMOIRE RAM");
    snprintf(buf,sizeof(buf),
        " %.1f / %.1f Go (%.0f%%)  %s  ~%.1f W",
        ram.usedGB, ram.totalGB, ram.usedPct,
        bar(ram.usedPct, 20, GREEN).c_str(),
        ram.powerW);
    row(buf);
    bot();
    std::cout << "\n";

    // ── USB ──
    if (!usbs.empty()) {
        hdr("PÉRIPHÉRIQUES USB (estimation)");
        double tot = 0;
        for (auto& d : usbs) {
            snprintf(buf,sizeof(buf),
                " %-40s  ~%.1f W", d.name.substr(0,40).c_str(), d.powerW);
            row(buf);
            tot += d.powerW;
        }
        sep();
        snprintf(buf,sizeof(buf), " Total USB                                   ~%.1f W", tot);
        row(buf);
        bot();
        std::cout << "\n";
    }

    // ── BILAN ──
    hdr("BILAN ÉNERGÉTIQUE");
    double kwh   = hist.cumWh / 1000.0;
    double cost  = kwh * TARIF_KWH;
    double avg24 = hist.avg() * 24.0 / 1000.0 * TARIF_KWH;
    snprintf(buf,sizeof(buf),
        " Cumulé   : %s%.4f Wh%s  (%.8f kWh)",
        GREEN, hist.cumWh, ANSI_RESET, kwh);
    row(buf);
    snprintf(buf,sizeof(buf),
        " Coût     : %s%.5f €%s  (tarif %.4f €/kWh)",
        YELLOW, cost, ANSI_RESET, TARIF_KWH);
    row(buf);
    snprintf(buf,sizeof(buf),
        " Projection 24h : %.3f €   mois : %.2f €",
        avg24, avg24 * 30);
    row(buf);

    sep();
    // Répartition
    snprintf(buf,sizeof(buf),
        " CPU %.1fW  GPU %.1fW  RAM %.1fW  USB %.1fW  Sys %.0fW",
        cpuW, gpu.powerW, ram.powerW, usbTotalW, SYS_BASE);
    row(buf);
    bot();

    // ── NOTE ──
    std::cout << "\n" << GRAY
        << "  ⚠  Estimation logicielle (TDP × usage).  "
        << "Précision ±10-20 W selon config.\n"
        << "     Ctrl+C pour quitter.\n"
        << ANSI_RESET << std::flush;
}

// ─────────────────────────────────────────────────────────────
//  LOG CSV (optionnel)
// ─────────────────────────────────────────────────────────────
static void logCsv(std::ofstream& f, int64_t t,
    double totalW, double cpuPct, double cpuW,
    double gpuPct, double gpuW,
    double wh, double cost) {
    f << t << "," << std::fixed << std::setprecision(2)
      << totalW << "," << cpuPct << "," << cpuW << ","
      << gpuPct << "," << gpuW  << ","
      << std::setprecision(6) << wh << ","
      << std::setprecision(8) << cost << "\n";
    f.flush();
}

// ─────────────────────────────────────────────────────────────
//  MAIN
// ─────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    enableAnsi();
    hideCursor();

    bool doCsv = false;
    for (int i=1;i<argc;i++) {
        if (std::string(argv[i]) == "--csv") doCsv = true;
    }

    std::ofstream csvFile;
    if (doCsv) {
        csvFile.open("powermon_log.csv");
        csvFile << "timestamp_s,total_W,cpu_pct,cpu_W,"
                   "gpu_pct,gpu_W,cumul_Wh,cumul_cost_eur\n";
    }

    // Init capteurs
    CpuReader cpu;
    if (!cpu.init()) {
        std::cerr << "ERREUR : impossible d'initialiser PDH CPU.\n";
        showCursor();
        return 1;
    }

    GpuReader gpuRdr;
    gpuRdr.detectGpuName();
    gpuRdr.initPdh();

    // Enum USB une fois au démarrage (on ré-énumère toutes les 10s)
    std::vector<UsbDevice> usbs = enumUsbDevices();
    int  usbRefreshCnt = 0;
    auto t0 = std::chrono::steady_clock::now();

    History hist;

    // Attendre un premier tick PDH
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    while (true) {
        double cpuPct = cpu.read();
        double cpuW   = CPU_TDP_IDLE +
                        (CPU_TDP_MAX - CPU_TDP_IDLE) * (cpuPct / 100.0);

        GpuInfo gpu = gpuRdr.read();

        RamInfo ram = readRam();

        // Ré-énumère USB toutes les 10s
        if (++usbRefreshCnt >= 10) {
            usbs = enumUsbDevices();
            usbRefreshCnt = 0;
        }
        double usbTotalW = 0;
        for (auto& d : usbs) usbTotalW += d.powerW;

        double totalW = cpuW + gpu.powerW + ram.powerW + usbTotalW + SYS_BASE;
        hist.push(totalW);

        auto   now    = std::chrono::steady_clock::now();
        int64_t elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - t0).count();

        render(cpuPct, cpuW, gpu, ram, usbs, hist, totalW, usbTotalW, elapsed);

        if (doCsv && csvFile.is_open()) {
            logCsv(csvFile, elapsed, totalW, cpuPct, cpuW,
                   gpu.usagePct, gpu.powerW,
                   hist.cumWh, (hist.cumWh/1000.0)*TARIF_KWH);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(REFRESH_MS));
    }

    showCursor();
    return 0;
}
