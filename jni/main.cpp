// ============================================================================
//  Anti Kebalik  -  mod AML 2.0.0 (32-bit / armeabi-v7a) untuk GTA San Andreas
//  Fungsi : jika AKTIF, kendaraan apa pun yang dipakai CJ dibalikkan otomatis
//           saat sudah BERHENTI dalam posisi terbalik di permukaan.
//           Kendaraan yang masih melayang/jatuh/meluncur TIDAK disentuh.
//  Toggle : configs/AntiKebalik.ini  ->  [Vehicles] AntiFlip = 1 (aktif) / 0 (mati)
//           File dibaca ulang tiap ~1 detik, jadi tidak perlu restart game.
//  Catatan: mod ini TERPISAH dari ARM Cheat Menu dan tidak mengubahnya.
// ============================================================================
#include <mod/amlmod.h>
#include <mod/logger.h>
#include <android/log.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

MYMOD(net.antikebalik.cj, AntiKebalik, 1.0.0, Custom)
NEEDGAME(com.rockstargames.gtasa)

// ---------------------------------------------------------------- tata letak
// Offset berikut stabil untuk GTA SA Android 32-bit (CPlaceable):
//   +0x14 = pointer CMatrix
// CMatrix: right@0x00, top(forward)@0x10, at(up)@0x20, pos@0x30
static const int OFF_MATRIX = 0x14;
struct Vec3 { float x, y, z; };
struct Mtx {
    Vec3 right; unsigned int f0;
    Vec3 top;   unsigned int f1;
    Vec3 at;    unsigned int f2;
    Vec3 pos;   unsigned int f3;
};

// --------------------------------------------------------------------- state
static void* hGTASA = nullptr;
static void* (*FindPlayerVehicle)(int, bool) = nullptr;
static void  (*CGame_Process_orig)() = nullptr;

static bool  g_enabled   = false;
static const char* INI_PATH_1 = "/storage/emulated/0/Android/data/com.rockstargames.gtasa/files/configs/AntiKebalik.ini";
static char  g_iniPath[512];
static time_t g_lastCfgCheck = 0;

static void*  g_lastVeh   = nullptr;
static Vec3   g_lastPos   = {0, 0, 0};
static int    g_stillTicks = 0;      // jumlah frame berturut-turut kendaraan diam
static int    g_cooldown   = 0;

// --- diagnostik (ditulis ke AntiKebalik.log tiap ~1 detik) ---
static unsigned g_frames = 0, g_flips = 0;
static bool   g_iniFound = false;
static char   g_usedIni[512] = "-";
static char   g_cfgPathAML[512] = "-";
static void*  g_dbgVeh = nullptr;
static Mtx*   g_dbgMtx = nullptr;
static float  g_dbgAtZ = 0, g_dbgMoved = 0, g_dbgPlacementZ = 0, g_dbgMatrixZ = 0;
static const char* LOG_PATH_1 = "/storage/emulated/0/Android/data/com.rockstargames.gtasa/files/configs/AntiKebalik.log";
static char g_logPath[512];
static char g_iniPath2[512] = "";   // berbasis lokasi libAntiKebalik.so: <mods>/../configs/
static char g_logPath2[512] = "";
static char g_selfPath[512] = "-";
static bool g_ctorRan = false;
static void* g_symProcess = nullptr;
static void* g_symFindVeh = nullptr;
static void* g_hGTASAdbg  = nullptr;
static int   g_stage = 0;   // 1=OnModLoad masuk, 2=simbol ok, 3=hook terpasang
static const char* LOG_PATH_2 = "/storage/emulated/0/Android/data/com.rockstargames.gtasa/files/AntiKebalik.log";
static const char* LOG_PATH_3 = "/sdcard/AntiKebalik.log";

static void WriteLog()
{
    FILE* f = g_logPath2[0] ? fopen(g_logPath2, "w") : nullptr;
    if (!f) f = fopen(g_logPath, "w");
    if (!f) f = fopen(LOG_PATH_1, "w");
    if (!f) f = fopen(LOG_PATH_2, "w");
    if (!f) f = fopen(LOG_PATH_3, "w");
    if (!f) return;
    fprintf(f, "AntiKebalik v1.2 status\n");
    fprintf(f, "constructor_ran=%d self=%s\n", g_ctorRan ? 1 : 0, g_selfPath);
    fprintf(f, "stage=%d (0=OnModLoad TIDAK dipanggil AML, 1=OnModLoad jalan, 2=simbol ketemu, 3=hook terpasang, 4=mode cadangan polling)\n", g_stage);
    fprintf(f, "libGTASA_handle=%p CGame::Process=%p FindPlayerVehicle=%p\n", g_hGTASAdbg, g_symProcess, g_symFindVeh);
    fprintf(f, "hook_frames=%u (naik terus = hook jalan)\n", g_frames);
    fprintf(f, "ini_found=%d path=%s\n", g_iniFound ? 1 : 0, g_usedIni);
    fprintf(f, "aml_config_path=%s\n", g_cfgPathAML);
    fprintf(f, "AntiFlip_enabled=%d\n", g_enabled ? 1 : 0);
    fprintf(f, "vehicle_ptr=%p matrix_ptr=%p\n", g_dbgVeh, (void*)g_dbgMtx);
    fprintf(f, "at.z=%.3f (negatif = terbalik)  moved/frame=%.4f  still_ticks=%d  cooldown=%d\n",
            g_dbgAtZ, g_dbgMoved, g_stillTicks, g_cooldown);
    fprintf(f, "matrix.pos.z=%.3f placement.pos.z=%.3f (harus hampir sama)\n", g_dbgMatrixZ, g_dbgPlacementZ);
    fprintf(f, "total_flips=%u\n", g_flips);
    fclose(f);
}
// ------------------------------------------------------------------- config
static void LoadConfig()
{
    const char* used = g_iniPath2;
    FILE* f = g_iniPath2[0] ? fopen(g_iniPath2, "r") : nullptr;
    if (!f) { used = g_iniPath; f = fopen(g_iniPath, "r"); }
    if (!f) { f = fopen(INI_PATH_1, "r"); used = INI_PATH_1; }
    g_iniFound = (f != nullptr);
    if (!f) return;                       // file tidak ada -> pertahankan nilai terakhir
    snprintf(g_usedIni, sizeof g_usedIni, "%s", used);
    char line[256]; bool inVeh = false; bool val = false;
    while (fgets(line, sizeof line, f))
    {
        char* p = line; while (*p == ' ' || *p == '\t') ++p;
        if (*p == '[') { inVeh = (strncasecmp(p, "[Vehicles]", 10) == 0); continue; }
        if (inVeh && strncasecmp(p, "AntiFlip", 8) == 0)
        {
            char* eq = strchr(p, '=');
            if (eq) val = atoi(eq + 1) != 0;
        }
    }
    fclose(f);
    g_enabled = val;
}

// --------------------------------------------------------------- logika inti
static void AntiFlipTick()
{
    void* veh = FindPlayerVehicle ? FindPlayerVehicle(-1, false) : nullptr;
    g_dbgVeh = veh; g_dbgMtx = nullptr;
    if (!veh) { g_lastVeh = nullptr; g_stillTicks = 0; return; }

    Mtx* m = *(Mtx**)((char*)veh + OFF_MATRIX);
    g_dbgMtx = m;
    g_dbgPlacementZ = *(float*)((char*)veh + 0x4 + 8);   // CSimpleTransform.pos.z
    if (!m) return;
    g_dbgMatrixZ = m->pos.z; g_dbgAtZ = m->at.z;

    if (veh != g_lastVeh) { g_lastVeh = veh; g_lastPos = m->pos; g_stillTicks = 0; return; }

    // Seberapa jauh kendaraan bergerak sejak frame lalu
    float dx = m->pos.x - g_lastPos.x, dy = m->pos.y - g_lastPos.y, dz = m->pos.z - g_lastPos.z;
    float moved = sqrtf(dx*dx + dy*dy + dz*dz);
    g_lastPos = m->pos;
    g_dbgMoved = moved;

    if (g_cooldown > 0) { --g_cooldown; return; }

    // 'at' = sumbu atas kendaraan. at.z < -0.3 => terbalik / hampir terbalik
    bool upsideDown = (m->at.z < -0.3f);
    bool still      = (moved < 0.06f);    // toleransi getaran fisika

    if (upsideDown && still) ++g_stillTicks; else g_stillTicks = 0;

    // Harus diam >= ~0.5 detik (30 frame) agar yakin sudah bersandar di permukaan,
    // bukan sedang melayang/berputar di puncak lompatan.
    if (g_stillTicks >= 20)
    {
        // Putar 180 derajat pada sumbu depan (top): heading tetap, roda ke bawah.
        m->right.x = -m->right.x; m->right.y = -m->right.y; m->right.z = -m->right.z;
        m->at.x    = -m->at.x;    m->at.y    = -m->at.y;    m->at.z    = -m->at.z;
        m->pos.z  += 0.8f;            // angkat sedikit agar tidak menembus tanah
        g_lastPos  = m->pos;
        g_stillTicks = 0; ++g_flips;
        g_cooldown   = 90;            // jeda ~1,5 detik
    }
}

// --------------------------------------------------------------- hook frame
static void CGame_Process_hook()
{
    CGame_Process_orig();             // proses game normal dulu
    ++g_frames;

    time_t now = time(nullptr);
    if (now != g_lastCfgCheck) { g_lastCfgCheck = now; LoadConfig(); WriteLog(); }

    if (g_enabled) AntiFlipTick();
    else { g_lastVeh = nullptr; g_stillTicks = 0; }
}

// ---------------------------------------------------------------- entry AML
extern "C" void OnModLoad()
{
    const char* cp = aml->GetConfigPath();
    snprintf(g_cfgPathAML, sizeof g_cfgPathAML, "%s", cp ? cp : "null");
    snprintf(g_iniPath, sizeof g_iniPath, "%s/configs/AntiKebalik.ini", cp ? cp : "");
    snprintf(g_logPath, sizeof g_logPath, "%s/configs/AntiKebalik.log", cp ? cp : "");
    g_stage = 1;
    LoadConfig();
    WriteLog();                                   // log pertama: bukti mod termuat

    hGTASA = aml->GetLibHandle("libGTASA.so");
    g_hGTASAdbg = hGTASA;
    FindPlayerVehicle = (void*(*)(int,bool))aml->GetSym(hGTASA, "_Z17FindPlayerVehicleib");
    void* pProcess    = (void*)aml->GetSym(hGTASA, "_ZN5CGame7ProcessEv");
    g_symFindVeh = (void*)FindPlayerVehicle; g_symProcess = pProcess;

    if (!FindPlayerVehicle || !pProcess)
    {
        __android_log_print(ANDROID_LOG_ERROR, "AntiKebalik", "simbol game tidak ditemukan");
        WriteLog();
        return;
    }
    g_stage = 2;
    aml->Hook(pProcess, (void*)CGame_Process_hook, (void**)&CGame_Process_orig);
    g_stage = 3;
    __android_log_print(ANDROID_LOG_INFO, "AntiKebalik", "siap (AntiFlip=%d)", g_enabled ? 1 : 0);
    WriteLog();
}


// ============================================================================
//  MODE CADANGAN (tanpa API AML)
//  Konstruktor berjalan begitu AML melakukan dlopen pada file ini, walau
//  antarmuka AML tidak cocok. Jika OnModLoad tidak pernah memasang hook,
//  thread ini memanggil logika yang sama ~60x/detik lewat polling.
// ============================================================================
static void* PollThread(void*)
{
    sleep(15);                                   // beri waktu AML memanggil OnModLoad
    if (g_stage == 3) return nullptr;            // hook normal sudah jalan -> tidak perlu

    void* h = nullptr;
    for (int i = 0; i < 600 && !h; ++i) { h = dlopen("libGTASA.so", RTLD_NOW | RTLD_NOLOAD); if (!h) sleep(1); }
    if (!h) { WriteLog(); return nullptr; }
    g_hGTASAdbg = h;
    FindPlayerVehicle = (void*(*)(int,bool))dlsym(h, "_Z17FindPlayerVehicleib");
    g_symFindVeh = (void*)FindPlayerVehicle;
    if (!FindPlayerVehicle) { WriteLog(); return nullptr; }

    g_stage = 4;
    for (;;)
    {
        usleep(16000);
        ++g_frames;
        time_t now = time(nullptr);
        if (now != g_lastCfgCheck) { g_lastCfgCheck = now; LoadConfig(); WriteLog(); }
        if (g_enabled) AntiFlipTick(); else { g_lastVeh = nullptr; g_stillTicks = 0; }
    }
    return nullptr;
}

__attribute__((constructor)) static void AntiKebalikCtor()
{
    g_ctorRan = true;
    Dl_info di;
    if (dladdr((void*)&AntiKebalikCtor, &di) && di.dli_fname)
    {
        snprintf(g_selfPath, sizeof g_selfPath, "%s", di.dli_fname);
        char dir[512]; snprintf(dir, sizeof dir, "%s", di.dli_fname);
        char* sl = strrchr(dir, '/'); if (sl) *sl = 0;          // folder mods
        snprintf(g_iniPath2, sizeof g_iniPath2, "%s/../configs/AntiKebalik.ini", dir);
        snprintf(g_logPath2, sizeof g_logPath2, "%s/../configs/AntiKebalik.log", dir);
    }
    LoadConfig();
    WriteLog();                                  // bukti file ini dimuat (stage=0 jika OnModLoad belum jalan)
    pthread_t t; pthread_create(&t, nullptr, PollThread, nullptr); pthread_detach(t);
}
