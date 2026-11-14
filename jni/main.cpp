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
 
// ------------------------------------------------------------------- config
static void LoadConfig()
{
    FILE* f = fopen(g_iniPath, "r");
    if (!f) f = fopen(INI_PATH_1, "r");
    if (!f) return;                       // file tidak ada -> pertahankan nilai terakhir
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
    if (!veh) { g_lastVeh = nullptr; g_stillTicks = 0; return; }
 
    Mtx* m = *(Mtx**)((char*)veh + OFF_MATRIX);
    if (!m) return;
 
    if (veh != g_lastVeh) { g_lastVeh = veh; g_lastPos = m->pos; g_stillTicks = 0; return; }
 
    // Seberapa jauh kendaraan bergerak sejak frame lalu
    float dx = m->pos.x - g_lastPos.x, dy = m->pos.y - g_lastPos.y, dz = m->pos.z - g_lastPos.z;
    float moved = sqrtf(dx*dx + dy*dy + dz*dz);
    g_lastPos = m->pos;
 
    if (g_cooldown > 0) { --g_cooldown; return; }
 
    // 'at' = sumbu atas kendaraan. at.z < -0.3 => terbalik / hampir terbalik
    bool upsideDown = (m->at.z < -0.3f);
    bool still      = (moved < 0.02f);    // kira-kira < 1.2 m/s pada 60 fps
 
    if (upsideDown && still) ++g_stillTicks; else g_stillTicks = 0;
 
    // Harus diam >= ~0.5 detik (30 frame) agar yakin sudah bersandar di permukaan,
    // bukan sedang melayang/berputar di puncak lompatan.
    if (g_stillTicks >= 30)
    {
        // Putar 180 derajat pada sumbu depan (top): heading tetap, roda ke bawah.
        m->right.x = -m->right.x; m->right.y = -m->right.y; m->right.z = -m->right.z;
        m->at.x    = -m->at.x;    m->at.y    = -m->at.y;    m->at.z    = -m->at.z;
        m->pos.z  += 0.8f;            // angkat sedikit agar tidak menembus tanah
        g_lastPos  = m->pos;
        g_stillTicks = 0;
        g_cooldown   = 90;            // jeda ~1,5 detik
    }
}
 
// --------------------------------------------------------------- hook frame
static void CGame_Process_hook()
{
    CGame_Process_orig();             // proses game normal dulu
 
    time_t now = time(nullptr);
    if (now != g_lastCfgCheck) { g_lastCfgCheck = now; LoadConfig(); }
 
    if (g_enabled) AntiFlipTick();
    else { g_lastVeh = nullptr; g_stillTicks = 0; }
}
 
// ---------------------------------------------------------------- entry AML
extern "C" void OnModLoad()
{
    snprintf(g_iniPath, sizeof g_iniPath, "%s/configs/AntiKebalik.ini", aml->GetConfigPath());
    LoadConfig();
 
    hGTASA = aml->GetLibHandle("libGTASA.so");
    FindPlayerVehicle = (void*(*)(int,bool))aml->GetSym(hGTASA, "_Z17FindPlayerVehicleib");
     void* pProcess    = (void*)aml->GetSym(hGTASA, "_ZN5CGame7ProcessEv");
   
 if (!FindPlayerVehicle || !pProcess) { __android_log_print(ANDROID_LOG_ERROR, "AntiKebalik", "simbol game tidak ditemukan"); return; }   aml->Hook(pProcess, (void*)CGame_Process_hook, (void**)&CGame_Process_orig);
    __android_log_print(ANDROID_LOG_INFO, "AntiKebalik", "siap (AntiFlip=%d)", g_enabled ? 1 : 0);
}
