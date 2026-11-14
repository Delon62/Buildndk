// ============================================================================
//  Anti Kebalik v2.2 - mod AML (32-bit / armeabi-v7a) untuk GTA San Andreas 2.00
//
//  Fungsi  : jika AKTIF, kendaraan apa pun yang dipakai CJ dibalikkan otomatis
//            saat sudah BERHENTI dalam posisi terbalik di permukaan.
//            Kendaraan yang masih melayang/jatuh/meluncur TIDAK disentuh.
//  Panel   : panel ImGui kecil dengan satu ceklis "Anti Kebalik" (on/off).
//            Tombol kecil "AK" selalu tampil di layar = bukti mod termuat.
//  Config  : AntiKebalik.ini  [Vehicles] AntiFlip = 1/0  (ikut tersimpan dari panel)
//  Catatan : mod ini TERPISAH dari ARM Cheat Menu dan tidak mengubahnya.
// ============================================================================
#include <mod/amlmod.h>
#include <mod/logger.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <mutex>
#include <vector>
 
#include "imgui.h"
#include "imgui_impl_opengl3.h"
 
MYMOD(net.antikebalik.cj, AntiKebalik, 2.2.0, Custom)
NEEDGAME(com.rockstargames.gtasa)
 
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "AntiKebalik", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "AntiKebalik", __VA_ARGS__)
 
// ------------------------------------------------------------ tata letak game
// CPlaceable +0x14 = pointer CMatrix. CMatrix: right@0x00 top@0x10 at(up)@0x20 pos@0x30
static const int OFF_MATRIX = 0x14;
struct Vec3 { float x, y, z; };
struct Mtx {
    Vec3 right; unsigned int f0;
    Vec3 top;   unsigned int f1;
    Vec3 at;    unsigned int f2;
    Vec3 pos;   unsigned int f3;
};
 
static double NowSec() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
 
// ------------------------------------------------------------------- state
static void* hGTASA = nullptr;
static void* (*FindPlayerVehicle)(int, bool) = nullptr;
static void  (*CGame_Process_orig)() = nullptr;
static EGLBoolean (*eglSwapBuffers_orig)(EGLDisplay, EGLSurface) = nullptr;
static void  (*TouchEvent_orig)(int, int, int, int, int) = nullptr;
 
static volatile bool g_enabled  = false;
static bool   g_panelOwns = false;           // true setelah pengguna memakai panel
static bool   g_saveOk    = true;
static int    g_stage     = 0;               // 1=OnModLoad, 2=simbol ok, 3=hook terpasang, 4=mode polling
static bool   g_ctorRan   = false;
 
static char   g_iniPath[512], g_iniPath2[512], g_logPath[512], g_logPath2[512];
static char   g_usedIni[512] = "-", g_cfgPathAML[512] = "-", g_selfPath[512] = "-", g_wroteTo[512] = "-";
static bool   g_iniFound = false;
static time_t g_lastCfgCheck = 0;
 
static const char* INI_PATH_1 = "/storage/emulated/0/Android/data/com.rockstargames.gtasa/files/configs/AntiKebalik.ini";
static const char* INI_PATH_2 = "/storage/emulated/0/Android_unprotect/com.rockstargames.gtasa/configs/AntiKebalik.ini";
 
// logika flip
static void*  g_lastVeh = nullptr;
static Vec3   g_lastPos = {0, 0, 0};
static int    g_stillTicks = 0, g_maxStill = 0;     // milidetik kendaraan diam terbalik
static Vec3   g_anchorPos = {0, 0, 0}, g_anchorAt = {0, 0, 1};
static double g_stillSince = 0, g_cooldownUntil = 0;
static unsigned g_frames = 0, g_flips = 0, g_upsideSeen = 0;
static void*  g_dbgVeh = nullptr;
static float  g_dbgAtZ = 0, g_dbgMoved = 0, g_minAtZ = 9.0f;
 
// diagnostik hook
static const char* g_touchSym = "-";
static bool   g_eglHooked = false, g_touchHooked = false;
static unsigned g_touchCount[8] = {0};
static int    g_lastTouchType = -1, g_lastTouchX = 0, g_lastTouchY = 0;
static unsigned g_renderFrames = 0;
static EGLContext g_ctx = EGL_NO_CONTEXT;
static int      g_winMatch = 0, g_winMis = 0;
static unsigned g_ctxSwitches = 0, g_skipCtx = 0, g_glErr = 0;
static bool     g_needLog = false;
 
// ------------------------------------------------------------------- config
static const char* FirstExistingIni()
{
    const char* c[] = { g_iniPath2, g_iniPath, INI_PATH_1, INI_PATH_2 };
    for (const char* p : c) { if (p[0]) { FILE* f = fopen(p, "r"); if (f) { fclose(f); return p; } } }
    return nullptr;
}
 
static void LoadConfig()
{
    const char* used = FirstExistingIni();
    g_iniFound = (used != nullptr);
    if (!used) return;
    snprintf(g_usedIni, sizeof g_usedIni, "%s", used);
    FILE* f = fopen(used, "r"); if (!f) return;
    char line[256]; bool inVeh = false; bool val = false;
    while (fgets(line, sizeof line, f))
    {
        char* p = line; while (*p == ' ' || *p == '\t') ++p;
        if (*p == '[') { inVeh = (strncasecmp(p, "[Vehicles]", 10) == 0); continue; }
        if (inVeh && strncasecmp(p, "AntiFlip", 8) == 0) { char* eq = strchr(p, '='); if (eq) val = atoi(eq + 1) != 0; }
    }
    fclose(f);
    if (!g_panelOwns) g_enabled = val;          // panel punya prioritas atas file
}
 
static void SaveConfig()
{
    const char* c[] = { g_usedIni, g_iniPath2, g_iniPath, INI_PATH_1, INI_PATH_2 };
    for (const char* p : c)
    {
        if (!p[0] || strcmp(p, "-") == 0) continue;
        FILE* f = fopen(p, "w");
        if (f) { fprintf(f, "[Vehicles]\nAntiFlip = %d\n", g_enabled ? 1 : 0); fclose(f); g_saveOk = true; return; }
    }
    g_saveOk = false;
}
 
// -------------------------------------------------------------------- log
static FILE* OpenLogSomewhere()
{
    if (g_wroteTo[0] && strcmp(g_wroteTo, "-") != 0) { FILE* f0 = fopen(g_wroteTo, "w"); if (f0) return f0; }
    char c[10][512]; int n = 0;
    auto add = [&](const char* s) { if (s && s[0] && n < 10) { snprintf(c[n], 512, "%s", s); ++n; } };
    if (g_usedIni[0] && strcmp(g_usedIni, "-") != 0)
    { char d[512]; snprintf(d, sizeof d, "%s", g_usedIni); char* sl = strrchr(d, '/'); if (sl) { *sl = 0; char t[540]; snprintf(t, sizeof t, "%s/AntiKebalik.log", d); add(t); } }
    add(g_logPath2); add(g_logPath);
    add("/storage/emulated/0/Android_unprotect/com.rockstargames.gtasa/configs/AntiKebalik.log");
    add("/storage/emulated/0/Android/data/com.rockstargames.gtasa/files/configs/AntiKebalik.log");
    add("/storage/emulated/0/Android/data/com.rockstargames.gtasa/files/AntiKebalik.log");
    for (int i = 0; i < n; ++i) { FILE* f = fopen(c[i], "w"); if (f) { snprintf(g_wroteTo, sizeof g_wroteTo, "%s", c[i]); return f; } }
    return nullptr;
}
 
static void WriteLog()
{
    FILE* f = OpenLogSomewhere(); if (!f) return;
    fprintf(f, "AntiKebalik v2.2 status\n");
    fprintf(f, "constructor_ran=%d self=%s\n", g_ctorRan ? 1 : 0, g_selfPath);
    fprintf(f, "stage=%d (1=OnModLoad,2=simbol ok,3=hook terpasang,4=polling cadangan)\n", g_stage);
    fprintf(f, "egl_hook=%d touch_hook=%d touch_sym=%s render_frames=%u\n", g_eglHooked, g_touchHooked, g_touchSym, g_renderFrames);
    fprintf(f, "touch_types: t0=%u t1=%u t2=%u t3=%u t4=%u t5=%u t6=%u t7+=%u last=(%d,%d,%d)\n",
            g_touchCount[0], g_touchCount[1], g_touchCount[2], g_touchCount[3], g_touchCount[4], g_touchCount[5], g_touchCount[6], g_touchCount[7],
            g_lastTouchType, g_lastTouchX, g_lastTouchY);
    fprintf(f, "gl_ctx=%p ctx_switches=%u skipped_frames=%u last_gl_error=0x%x\n", g_ctx, g_ctxSwitches, g_skipCtx, g_glErr);
    fprintf(f, "hook_frames=%u ini_found=%d path=%s aml_config=%s\n", g_frames, g_iniFound ? 1 : 0, g_usedIni, g_cfgPathAML);
    fprintf(f, "AntiFlip_enabled=%d panel_owns=%d save_ok=%d\n", g_enabled ? 1 : 0, g_panelOwns ? 1 : 0, g_saveOk ? 1 : 0);
    fprintf(f, "vehicle_ptr=%p at.z=%.3f moved=%.4f still=%d max_still=%d min_at.z=%.3f upside_seen=%u flips=%u\n",
            g_dbgVeh, g_dbgAtZ, g_dbgMoved, g_stillTicks, g_maxStill, g_minAtZ, g_upsideSeen, g_flips);
    fprintf(f, "log_ditulis_ke=%s\n", g_wroteTo);
    fclose(f);
}
 
// --------------------------------------------------------------- logika inti
static void AntiFlipTick()
{
    void* veh = FindPlayerVehicle ? FindPlayerVehicle(-1, false) : nullptr;
    g_dbgVeh = veh;
    if (!veh) { g_lastVeh = nullptr; g_stillSince = 0; g_stillTicks = 0; return; }
    Mtx* m = *(Mtx**)((char*)veh + OFF_MATRIX);
    if (!m) return;
    g_dbgAtZ = m->at.z; if (m->at.z < g_minAtZ) g_minAtZ = m->at.z;
 
    double now = NowSec();
    if (veh != g_lastVeh) { g_lastVeh = veh; g_stillSince = 0; g_stillTicks = 0; return; }
    if (now < g_cooldownUntil) return;
 
    // Hanya kendaraan TERBALIK yang diperhatikan. Selama kendaraan tidak terbalik,
    // mod ini tidak menyentuh apa pun (rotasi, gravitasi, kecepatan tetap bebas/normal).
    bool upsideDown = (m->at.z < -0.3f);               // 'at' = sumbu atas kendaraan
    if (!upsideDown) { g_stillSince = 0; g_stillTicks = 0; return; }
    ++g_upsideSeen;
 
    // "Di permukaan" = kendaraan TERBALIK DAN benar-benar tidak berpindah/berputar selama 0,5 detik.
    // Jarak diukur dari titik jangkar (bukan per frame), jadi kendaraan yang melayang, jatuh,
    // atau berputar di udara tidak pernah memenuhi syarat ini (jatuh bebas 0,5 detik = >1 meter).
    if (g_stillSince == 0) { g_anchorPos = m->pos; g_anchorAt = m->at; g_stillSince = now; g_stillTicks = 0; return; }
 
    float dx = m->pos.x - g_anchorPos.x, dy = m->pos.y - g_anchorPos.y, dz = m->pos.z - g_anchorPos.z;
    float dist = sqrtf(dx*dx + dy*dy + dz*dz);
    float dotAt = m->at.x*g_anchorAt.x + m->at.y*g_anchorAt.y + m->at.z*g_anchorAt.z;
    g_dbgMoved = dist;
 
    if (dist > 0.12f || dotAt < 0.995f)                // masih bergerak/berputar -> hitung ulang dari awal
    { g_anchorPos = m->pos; g_anchorAt = m->at; g_stillSince = now; g_stillTicks = 0; return; }
 
    g_stillTicks = (int)((now - g_stillSince) * 1000.0);
    if (g_stillTicks > g_maxStill) g_maxStill = g_stillTicks;
 
    if (g_stillTicks >= 500)
    {
        // putar 180 derajat pada sumbu depan (top): heading tetap, roda ke bawah
        m->right.x = -m->right.x; m->right.y = -m->right.y; m->right.z = -m->right.z;
        m->at.x = -m->at.x; m->at.y = -m->at.y; m->at.z = -m->at.z;
        m->pos.z += 0.8f;
        g_stillSince = 0; g_stillTicks = 0; ++g_flips; g_cooldownUntil = now + 1.5;
    }
}
 
static void CGame_Process_hook()
{
    CGame_Process_orig();
    ++g_frames;
    if (g_enabled) AntiFlipTick(); else { g_lastVeh = nullptr; g_stillSince = 0; g_stillTicks = 0; }
}
 
// ============================================================================
//  PANEL ImGui
// ============================================================================
struct TouchEv { int type, finger, x, y; };
static std::mutex g_touchMx;
static std::vector<TouchEv> g_touchQ;
 
static void TouchEvent_hook(int type, int finger, int x, int y, int extra)
{
    {
        std::lock_guard<std::mutex> lk(g_touchMx);
        if (g_touchQ.size() < 256) g_touchQ.push_back({type, finger, x, y});
    }
    if (TouchEvent_orig) TouchEvent_orig(type, finger, x, y, extra);   // game tetap menerima sentuhan
}
 
 
static bool   g_imInit = false, g_panelOpen = false;
static double g_lastFrameT = 0, g_lastDownT = 0;
static bool   g_mouseDown = false;
static int    g_activeFinger = -1;
static float  g_uiScale = 1.0f;
 
static void PumpTouches(ImGuiIO& io)
{
    std::vector<TouchEv> q;
    { std::lock_guard<std::mutex> lk(g_touchMx); q.swap(g_touchQ); }
    double now = NowSec();
    for (const TouchEv& e : q)
    {
        g_touchCount[(e.type >= 0 && e.type < 7) ? e.type : 7]++;
        g_lastTouchType = e.type; g_lastTouchX = e.x; g_lastTouchY = e.y;
        bool isUp   = (e.type >= 3 && e.type <= 6);
        bool isDown = (e.type == 0 || e.type == 1);
        if (!g_mouseDown)
        {
            if (isDown) { io.AddMousePosEvent((float)e.x, (float)e.y); io.AddMouseButtonEvent(0, true); g_mouseDown = true; g_activeFinger = e.finger; g_lastDownT = now; }
        }
        else if (e.finger == g_activeFinger)
        {
            io.AddMousePosEvent((float)e.x, (float)e.y);
            if (isUp) { io.AddMouseButtonEvent(0, false); g_mouseDown = false; g_activeFinger = -1; }
            else g_lastDownT = now;
        }
    }
    // pengaman: jika tidak ada kabar "angkat jari" (tipe event beda), lepas otomatis setelah 250 ms
    if (g_mouseDown && now - g_lastDownT > 0.25) { io.AddMouseButtonEvent(0, false); g_mouseDown = false; g_activeFinger = -1; }
}
 
static void InitImGui(int w, int h)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    g_uiScale = (float)h / 540.0f; if (g_uiScale < 1.3f) g_uiScale = 1.3f; if (g_uiScale > 3.2f) g_uiScale = 3.2f;
    ImGui::GetStyle().ScaleAllSizes(g_uiScale);
    ImGui::GetStyle().WindowRounding = 10.0f * g_uiScale;
    ImGui::GetStyle().FrameRounding  = 6.0f * g_uiScale;
    io.FontGlobalScale = g_uiScale;
    ImGui_ImplOpenGL3_Init(nullptr);
    g_imInit = true;
    (void)w;
}
 
static void DrawPanel(int w, int h)
{
    ImGuiIO& io = ImGui::GetIO();
    float bx = 8.0f * g_uiScale, by = (float)h * 0.30f;
 
    // tombol kecil "AK": warna menunjukkan status
    ImGui::SetNextWindowPos(ImVec2(bx, by));
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##akbtn", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImVec4 col = g_enabled ? ImVec4(0.10f, 0.60f, 0.20f, 0.85f) : ImVec4(0.55f, 0.15f, 0.15f, 0.85f);
    ImGui::PushStyleColor(ImGuiCol_Button, col);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, col);
    if (ImGui::Button(g_enabled ? "AK ON" : "AK OFF")) g_panelOpen = !g_panelOpen;
    ImGui::PopStyleColor(3);
    ImVec2 bsz = ImGui::GetWindowSize();
    ImGui::End();
 
    if (!g_panelOpen) return;
 
    ImGui::SetNextWindowPos(ImVec2(bx + bsz.x + 6.0f * g_uiScale, by));
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGui::Begin("Anti Kebalik", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
    bool v = g_enabled;
    if (ImGui::Checkbox("Anti Kebalik", &v))
    {
        g_enabled = v; g_panelOwns = true; SaveConfig(); WriteLog();
    }
    if (g_enabled) ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "AKTIF");
    else           ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "MATI (normal)");
    ImGui::TextDisabled("Mod termuat - v2.2");
    if (ImGui::CollapsingHeader("Info debug"))
    {
        ImGui::Text("stage=%d  egl=%d  touch=%d", g_stage, g_eglHooked, g_touchHooked);
        ImGui::Text("sentuhan: %u %u %u %u %u", g_touchCount[0], g_touchCount[1], g_touchCount[2], g_touchCount[3], g_touchCount[4]);
        ImGui::Text("kendaraan: %s", g_dbgVeh ? "ya" : "tidak");
        ImGui::Text("at.z=%.2f diam=%dms flip=%u", g_dbgAtZ, g_stillTicks, g_flips);
        ImGui::Text("simpan ini: %s", g_saveOk ? "ok" : "gagal");
        ImGui::Text("ctx ganti=%u lewat=%u glerr=0x%x", g_ctxSwitches, g_skipCtx, g_glErr);
        if (ImGui::Button("Simpan log")) WriteLog();
    }
    if (ImGui::Button("Tutup")) g_panelOpen = false;
    ImGui::End();
    (void)io; (void)w;
}
 
static void RenderPanel(EGLDisplay dpy, EGLSurface surf)
{
    EGLContext cur = eglGetCurrentContext();
    if (cur == EGL_NO_CONTEXT) return;
    if (g_ctx == EGL_NO_CONTEXT) g_ctx = cur;
 
    // Jika game memakai lebih dari satu konteks GL, gambar hanya di konteks yang dominan.
    if (cur == g_ctx) ++g_winMatch; else { ++g_winMis; ++g_skipCtx; }
    if (g_winMatch + g_winMis >= 120)
    {
        if (g_winMis > g_winMatch)                // konteks utama berpindah -> ikuti
        {
            if (g_imInit) ImGui_ImplOpenGL3_DestroyDeviceObjects();
            g_ctx = cur; ++g_ctxSwitches; g_needLog = true;
        }
        g_winMatch = g_winMis = 0;
    }
    if (cur != g_ctx) return;
 
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w); eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w <= 0 || h <= 0) return;
 
    if (!g_imInit) InitImGui(w, h);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    double t = NowSec(); float dt = (float)(t - g_lastFrameT); g_lastFrameT = t;
    if (dt < 0.0001f || dt > 0.25f) dt = 1.0f / 60.0f;
    io.DeltaTime = dt;
    PumpTouches(io);
 
    // --- simpan state GL milik game, lalu pakai state bersih untuk ImGui
    GLint prevFbo = 0, prevArr = 0, prevElem = 0, attrEn[8];
    GLboolean cm[4] = {1, 1, 1, 1};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArr);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &prevElem);
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    for (int i = 0; i < 8; ++i) { attrEn[i] = 0; glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attrEn[i]); }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
 
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    DrawPanel(w, h);
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
 
    // --- kembalikan state GL game (atribut vertex yang dinyalakan ImGui harus dimatikan lagi)
    for (int i = 0; i < 8; ++i) { if (attrEn[i]) glEnableVertexAttribArray(i); else glDisableVertexAttribArray(i); }
    glColorMask(cm[0], cm[1], cm[2], cm[3]);
    glBindBuffer(GL_ARRAY_BUFFER, prevArr);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, prevElem);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    GLenum e = glGetError(); if (e != GL_NO_ERROR) g_glErr = e;
    ++g_renderFrames;
 
    if (g_needLog) { g_needLog = false; WriteLog(); }   // jarang: hanya saat konteks berganti
}
 
static EGLBoolean eglSwapBuffers_hook(EGLDisplay dpy, EGLSurface surf)
{
    RenderPanel(dpy, surf);
    return eglSwapBuffers_orig(dpy, surf);
}
 
// ============================================================================
//  MODE CADANGAN (tanpa API AML): polling ~60x/detik. Panel tidak tersedia.
// ============================================================================
static void* PollThread(void*)
{
    sleep(15);
    if (g_stage == 3) return nullptr;
    void* h = nullptr;
    for (int i = 0; i < 600 && !h; ++i) { h = dlopen("libGTASA.so", RTLD_NOW | RTLD_NOLOAD); if (!h) sleep(1); }
    if (!h) { WriteLog(); return nullptr; }
    FindPlayerVehicle = (void*(*)(int,bool))dlsym(h, "_Z17FindPlayerVehicleib");
    if (!FindPlayerVehicle) { WriteLog(); return nullptr; }
    g_stage = 4;
    for (;;)
    {
        usleep(16000); ++g_frames;
        time_t now = time(nullptr);
        if (now - g_lastCfgCheck >= 10) { g_lastCfgCheck = now; LoadConfig(); }
        if (g_enabled) AntiFlipTick(); else { g_lastVeh = nullptr; g_stillSince = 0; g_stillTicks = 0; }
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
        char* sl = strrchr(dir, '/'); if (sl) *sl = 0;
        snprintf(g_iniPath2, sizeof g_iniPath2, "%s/../configs/AntiKebalik.ini", dir);
        snprintf(g_logPath2, sizeof g_logPath2, "%s/../configs/AntiKebalik.log", dir);
    }
    LoadConfig(); WriteLog();
    pthread_t t; pthread_create(&t, nullptr, PollThread, nullptr); pthread_detach(t);
}
 
// ---------------------------------------------------------------- entry AML
extern "C" void OnModLoad()
{
    const char* cp = aml->GetConfigPath();
    snprintf(g_cfgPathAML, sizeof g_cfgPathAML, "%s", cp ? cp : "null");
    snprintf(g_iniPath, sizeof g_iniPath, "%s/AntiKebalik.ini", cp ? cp : "");
    snprintf(g_logPath, sizeof g_logPath, "%s/AntiKebalik.log", cp ? cp : "");
    g_stage = 1; LoadConfig(); WriteLog();

    hGTASA = aml->GetLibHandle("libGTASA.so");
    FindPlayerVehicle = (void*(*)(int,bool))aml->GetSym(hGTASA, "_Z17FindPlayerVehicleib");
    void* pProcess    = (void*)aml->GetSym(hGTASA, "_ZN5CGame7ProcessEv");
    if (!FindPlayerVehicle || !pProcess) { LOGE("simbol game tidak ditemukan"); WriteLog(); return; }
    g_stage = 2;
    aml->Hook(pProcess, (void*)CGame_Process_hook, (void**)&CGame_Process_orig);
    g_stage = 3;

    // --- panel: hook eglSwapBuffers (render) dan AND_TouchEvent (sentuhan)
    void* hEGL = dlopen("libEGL.so", RTLD_NOW);
    void* pSwap = hEGL ? dlsym(hEGL, "eglSwapBuffers") : nullptr;
    if (pSwap) { aml->Hook(pSwap, (void*)eglSwapBuffers_hook, (void**)&eglSwapBuffers_orig); g_eglHooked = (eglSwapBuffers_orig != nullptr); }

    const char* names[] = { "AND_TouchEvent", "_Z14AND_TouchEventiiii", "_Z14AND_TouchEventiiiii" };
    for (const char* n : names)
    {
        void* p = (void*)aml->GetSym(hGTASA, n);
        if (p) { aml->Hook(p, (void*)TouchEvent_hook, (void**)&TouchEvent_orig); g_touchHooked = (TouchEvent_orig != nullptr); g_touchSym = n; break; }
    }
    LOGI("siap: egl=%d touch=%d AntiFlip=%d", g_eglHooked, g_touchHooked, g_enabled ? 1 : 0);
    WriteLog();
}
