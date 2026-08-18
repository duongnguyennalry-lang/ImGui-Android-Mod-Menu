// ================================================================
//  THROWIO MOD - AXIOM DEVELOPMENT
//  CRASH FIX v6.1 — THREAD-SAFE COMPLETE IMPLEMENTATION
//
//  ROOT CAUSE FIX SUMMARY:
//    [1] Tách biệt hoàn toàn UI (Render Thread - eglSwapBuffers) 
//        và Game Logic (Main Thread - hook_CharWeapon_update).
//    [2] Tránh tuyệt đối việc gọi hàm Il2Cpp / Game API từ luồng đồ họa
//        gây lỗi Invalid PC / SIGSEGV.
//    [3] Sử dụng hệ thống Flag an toàn từ menu ImGui kích hoạt 
//        xuống luồng Main Thread thực thi.
// ================================================================

#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>
#include "MainHeader.h"
#include "CrashLogger.h"
#include <sys/mman.h>
#include <unistd.h>
#include <atomic>
#include <pthread.h>
#include <dlfcn.h>
#include <thread>
#include <chrono>

// ================================================================
//  FUNCTION POINTERS
// ================================================================
void (*set_SoftMoney)(void* instance, long  value) = nullptr;
void (*set_HardMoney)(void* instance, long  value) = nullptr;
void (*set_Level)    (void* instance, int   value) = nullptr;
void (*set_Exp)      (void* instance, int   value) = nullptr;
void (*set_NoAds)    (void* instance, bool  value) = nullptr;
long (*get_SoftMoney)(void* instance)              = nullptr;
long (*get_HardMoney)(void* instance)              = nullptr;
int  (*get_Level)    (void* instance)              = nullptr;

void (*old_ApplyDamage)      (void*, long, void*, bool, void*) = nullptr;
void (*old_SetDeath)         (void*, bool)     = nullptr;
void (*old_CharWeapon_update)(void*, float)    = nullptr;
void (*old_SaveLocal)        (void*)           = nullptr;
void (*orig_set_SoftMoney)   (void*, long)     = nullptr;

// ================================================================
//  TOGGLES & FLAGS
// ================================================================
namespace SWITCH {
    bool InfiniteMoney   = false;
    bool InfinitePremium = false;
    bool MaxLevel        = false;
    bool NoAds           = false;
    bool GodMode         = false;
    bool SpeedHack       = false;
    bool AntiCheat       = true;
    
    // Cờ an toàn xử lý sự kiện giao diện từ UI Thread sang Main Thread
    bool ForceUpdateMoney = false;
    bool ForceMaxLevel    = false;
}

// ================================================================
//  GLOBALS
// ================================================================
void*  g_BalanceInstance = nullptr;
float  speedMultiplier   = 2.0f;

static std::atomic<int>  g_frameCount{0};
static std::atomic<bool> g_eglReady{false};
static std::atomic<bool> g_imguiSetup{false};
static std::atomic<bool> g_hooksReady{false};
static std::atomic<bool> g_imguiFailed{false};
static std::atomic<int>  g_failCount{0};

static EGLDisplay g_savedDisplay = EGL_NO_DISPLAY;
static EGLContext g_savedContext = EGL_NO_CONTEXT;

EGLBoolean (*old_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;

// ================================================================
//  il2cpp API typedefs 
// ================================================================
typedef void*  (*fn_domain_get)();
typedef void** (*fn_domain_get_assemblies)(void* domain, size_t* count);
typedef void*  (*fn_assembly_get_image)(void* assembly);
typedef const char* (*fn_image_get_name)(void* image);

// ================================================================
//  IsValidPtr
// ================================================================
static bool IsValidPtr(const void* ptr, size_t sz = sizeof(void*)) {
    if (!ptr) return false;
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr < 0x10000ULL)           return false;
    if (addr > 0x7fffffffffffULL)    return false;
    if (addr & 0x1)                  return false;

    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;

    uintptr_t page = addr & ~static_cast<uintptr_t>(ps - 1);
    size_t aligned = (sz + static_cast<size_t>(ps) - 1)
                   & ~static_cast<size_t>(ps - 1);
    if (!aligned) aligned = static_cast<size_t>(ps);

    unsigned char vec = 0;
    return mincore(reinterpret_cast<void*>(page), aligned, &vec) == 0;
}

// ================================================================
//  ValidateAssemblyEntry
// ================================================================
static bool ValidateAssemblyEntry(
    fn_assembly_get_image assembly_get_image,
    fn_image_get_name     image_get_name,
    void* assembly)
{
    if (!IsValidPtr(assembly)) return false;

    void* image = assembly_get_image(assembly);
    if (!IsValidPtr(image)) return false;

    const char* name = image_get_name(image);
    if (!IsValidPtr((void*)name, 4)) return false;
    if (name[0] == '\0') return false;

    return true;
}

// ================================================================
//  WaitForAssembliesReady
// ================================================================
static bool WaitForAssembliesReady(int maxWaitMs = 20000) {
    void* lib = dlopen("libil2cpp.so", RTLD_NOLOAD);
    if (!lib) {
        LOGE(OBFUSCATE("ThrowIO: RTLD_NOLOAD libil2cpp.so failed — fallback sleep"));
        usleep(8000000);
        return true;
    }

    auto domain_get          = (fn_domain_get)         dlsym(lib, "il2cpp_domain_get");
    auto domain_get_assemblies = (fn_domain_get_assemblies)
                                 dlsym(lib, "il2cpp_domain_get_assemblies");
    auto assembly_get_image  = (fn_assembly_get_image) dlsym(lib, "il2cpp_assembly_get_image");
    auto image_get_name      = (fn_image_get_name)     dlsym(lib, "il2cpp_image_get_name");

    if (!domain_get || !domain_get_assemblies ||
        !assembly_get_image || !image_get_name) {
        LOGE(OBFUSCATE("ThrowIO: dlsym il2cpp funcs failed"));
        dlclose(lib);
        usleep(8000000);
        return true;
    }

    const int step = 200; 
    int elapsed = 0;
    int stableCount = 0;   

    while (elapsed < maxWaitMs) {
        void* domain = domain_get();
        if (!IsValidPtr(domain)) {
            stableCount = 0;
            usleep(step * 1000);
            elapsed += step;
            continue;
        }

        size_t count = 0;
        void** assemblies = domain_get_assemblies(domain, &count);

        if (!IsValidPtr(assemblies) || count < 10 || count > 4096) {
            stableCount = 0;
            usleep(step * 1000);
            elapsed += step;
            continue;
        }

        bool allValid = true;
        for (size_t i = 0; i < count; i++) {
            if (!ValidateAssemblyEntry(assembly_get_image, image_get_name, assemblies[i])) {
                allValid = false;
                break;
            }
        }

        if (allValid) {
            stableCount++;
            if (stableCount >= 3) {
                dlclose(lib);
                return true;
            }
        } else {
            stableCount = 0;
        }

        usleep(step * 1000);
        elapsed += step;
    }

    dlclose(lib);
    return false;
}

// ================================================================
//  IsEGLContextCurrent & IsGLReady
// ================================================================
static bool IsEGLContextCurrent() {
    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLContext ctx = eglGetCurrentContext();
    if (dpy == EGL_NO_DISPLAY || ctx == EGL_NO_CONTEXT) return false;
    if (g_savedContext != EGL_NO_CONTEXT && ctx != g_savedContext) return false;
    return true;
}

static bool IsGLReady() {
    while (glGetError() != GL_NO_ERROR) {}

    GLint major = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    if (glGetError() != GL_NO_ERROR || major < 2) return false;

    GLint viewport[4] = {0};
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (glGetError() != GL_NO_ERROR || viewport[2] <= 0 || viewport[3] <= 0)
        return false;

    glFlush();
    glFinish();
    if (glGetError() != GL_NO_ERROR) return false;

    return true;
}

// ================================================================
//  ApplyWatchdog (Main Thread Safe)
// ================================================================
static inline void ApplyWatchdog() {
    void* inst = g_BalanceInstance;
    if (!inst || !IsValidPtr(inst)) return;
    if (SWITCH::InfiniteMoney   && set_SoftMoney) set_SoftMoney(inst, 0x7FFFFFFF);
    if (SWITCH::InfinitePremium && set_HardMoney) set_HardMoney(inst, 0x7FFFFFFF);
    if (SWITCH::MaxLevel        && set_Level)     set_Level(inst, 99);
    if (SWITCH::MaxLevel        && set_Exp)       set_Exp(inst, 0x7FFFFFFF);
    if (SWITCH::NoAds           && set_NoAds)     set_NoAds(inst, true);
}

// ================================================================
//  HOOKS (GAME MAIN THREAD SAFE)
// ================================================================
void capture_set_SoftMoney(void* instance, long value) {
    if (instance && IsValidPtr(instance) && !g_BalanceInstance) {
        g_BalanceInstance = instance;
        LOGI(OBFUSCATE("ThrowIO: balance instance captured -> %p"), instance);
    }
    if (orig_set_SoftMoney) orig_set_SoftMoney(instance, value);
}

void hook_ApplyDamage(void* inst, long dmg, void* from, bool crit, void* extra) {
    if (SWITCH::GodMode && inst) return;
    if (old_ApplyDamage) old_ApplyDamage(inst, dmg, from, crit, extra);
}

void hook_SetDeath(void* inst, bool isDead) {
    if (SWITCH::GodMode && inst) isDead = false;
    if (old_SetDeath) old_SetDeath(inst, isDead);
}

// [FIX v6.1] Xử lý cờ giao diện trực tiếp trên luồng Game Logic an toàn tuyệt đối
void hook_CharWeapon_update(void* inst, float dt) {
    if (SWITCH::SpeedHack && inst) dt *= speedMultiplier;

    void* balInst = g_BalanceInstance;
    if (balInst && IsValidPtr(balInst)) {
        if (SWITCH::ForceUpdateMoney) {
            if (set_SoftMoney) set_SoftMoney(balInst, 0x7FFFFFFF);
            if (set_HardMoney) set_HardMoney(balInst, 0x7FFFFFFF);
            SWITCH::ForceUpdateMoney = false;
        }
        if (SWITCH::ForceMaxLevel) {
            if (set_Level) set_Level(balInst, 99);
            if (set_Exp)   set_Exp(balInst, 0x7FFFFFFF);
            SWITCH::ForceMaxLevel = false;
        }
    }

    if (SWITCH::AntiCheat) ApplyWatchdog(); 

    if (old_CharWeapon_update) old_CharWeapon_update(inst, dt);
}

void hook_SaveLocal(void* inst) {
    if (SWITCH::AntiCheat) ApplyWatchdog();
    if (old_SaveLocal) old_SaveLocal(inst);
    if (SWITCH::AntiCheat) ApplyWatchdog();
}

// ================================================================
//  ImGui Style Setup
// ================================================================
static void ApplyImGuiStyle() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 10.0f; st.FrameRounding  =  5.0f;
    st.ScrollbarRounding =  5.0f; st.GrabRounding =  4.0f;
    st.WindowPadding  = ImVec2(12, 12);
    st.ItemSpacing    = ImVec2( 8,  6);
    st.Colors[ImGuiCol_WindowBg]       = ImVec4(0.04f, 0.04f, 0.07f, 0.97f);
    st.Colors[ImGuiCol_TitleBg]        = ImVec4(0.0f,  0.12f, 0.30f, 1.0f);
    st.Colors[ImGuiCol_TitleBgActive]  = ImVec4(0.0f,  0.20f, 0.50f, 1.0f);
    st.Colors[ImGuiCol_FrameBg]        = ImVec4(0.10f, 0.10f, 0.16f, 1.0f);
    st.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.15f, 0.15f, 0.25f, 1.0f);
    st.Colors[ImGuiCol_CheckMark]      = ImVec4(0.0f,  0.90f, 1.0f,  1.0f);
    st.Colors[ImGuiCol_SliderGrab]     = ImVec4(0.0f,  0.70f, 1.0f,  1.0f);
    st.Colors[ImGuiCol_Button]         = ImVec4(0.0f,  0.25f, 0.55f, 1.0f);
    st.Colors[ImGuiCol_ButtonHovered]  = ImVec4(0.0f,  0.40f, 0.80f, 1.0f);
    st.Colors[ImGuiCol_ButtonActive]   = ImVec4(0.0f,  0.60f, 1.0f,  1.0f);
    st.Colors[ImGuiCol_Header]         = ImVec4(0.0f,  0.30f, 0.60f, 0.8f);
    st.Colors[ImGuiCol_Separator]      = ImVec4(0.20f, 0.20f, 0.30f, 1.0f);
}

// ================================================================
//  SafeSetupImGui
// ================================================================
static bool SafeSetupImGui(EGLDisplay dpy, EGLSurface surface) {
    if (!IsEGLContextCurrent()) return false;

    EGLint w = 0, h = 0;
    if (eglQuerySurface(dpy, surface, EGL_WIDTH,  &w) != EGL_TRUE ||
        eglQuerySurface(dpy, surface, EGL_HEIGHT, &h) != EGL_TRUE ||
        w <= 0 || h <= 0) return false;

    if (!IsGLReady()) return false;

    GLenum fbStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fbStatus != GL_FRAMEBUFFER_COMPLETE) return false;

    g_savedDisplay = eglGetCurrentDisplay();
    g_savedContext = eglGetCurrentContext();
    while (glGetError() != GL_NO_ERROR) {}

    SetupImGui();

    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx) return false;

    if (glGetError() != GL_NO_ERROR) return false;

    ApplyImGuiStyle();
    return true;
}

// ================================================================
//  HOOK: eglSwapBuffers (RENDER THREAD - UI ONLY)
// ================================================================
EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    if (!old_eglSwapBuffers) return EGL_FALSE;

    if (dpy == EGL_NO_DISPLAY || surface == EGL_NO_SURFACE)
        return old_eglSwapBuffers(dpy, surface);
    if (eglGetCurrentContext() == EGL_NO_CONTEXT)
        return old_eglSwapBuffers(dpy, surface);

    EGLint w = 0, h = 0;
    if (eglQuerySurface(dpy, surface, EGL_WIDTH,  &w) != EGL_TRUE ||
        eglQuerySurface(dpy, surface, EGL_HEIGHT, &h) != EGL_TRUE ||
        w <= 0 || h <= 0)
        return old_eglSwapBuffers(dpy, surface);

    glWidth  = w;
    glHeight = h;

    int frame = g_frameCount.fetch_add(1, std::memory_order_relaxed);
    if (frame < 300) return old_eglSwapBuffers(dpy, surface);

    if (g_imguiFailed.load(std::memory_order_acquire)) {
        int fails = g_failCount.load(std::memory_order_relaxed);
        int retryAt = (fails < 3) ? 120 : (fails < 6) ? 240 : 360;
        if (frame % retryAt != 0) return old_eglSwapBuffers(dpy, surface);
        g_imguiFailed.store(false, std::memory_order_release);
        g_imguiSetup.store(false,  std::memory_order_release);
        g_eglReady.store(false,    std::memory_order_release);
    }

    if (!g_imguiSetup.load(std::memory_order_acquire)) {
        bool ok = SafeSetupImGui(dpy, surface);
        if (ok) {
            g_failCount.store(0, std::memory_order_relaxed);
            g_imguiSetup.store(true, std::memory_order_release);
            g_eglReady.store(true,   std::memory_order_release);
        } else {
            g_failCount.fetch_add(1, std::memory_order_relaxed);
            g_imguiFailed.store(true, std::memory_order_release);
            return old_eglSwapBuffers(dpy, surface);
        }
    }

    if (!g_eglReady.load(std::memory_order_acquire))
        return old_eglSwapBuffers(dpy, surface);

    if (!IsEGLContextCurrent()) {
        g_imguiSetup.store(false, std::memory_order_release);
        g_eglReady.store(false,   std::memory_order_release);
        g_savedContext = EGL_NO_CONTEXT;
        g_savedDisplay = EGL_NO_DISPLAY;
        return old_eglSwapBuffers(dpy, surface);
    }

    ImGuiContext* imCtx = ImGui::GetCurrentContext();
    if (!imCtx) {
        g_imguiSetup.store(false, std::memory_order_release);
        g_eglReady.store(false,   std::memory_order_release);
        return old_eglSwapBuffers(dpy, surface);
    }

    // [QUAN TRỌNG] KHÔNG gọi bất kỳ hàm Il2Cpp nào tại đây (Render Thread)

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowSize(ImVec2(370, 540), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos (ImVec2(10,  10),  ImGuiCond_FirstUseEver);
    ImGui::Begin(OBFUSCATE("THROWIO MOD - AXIOM"), nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);

    ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f), OBFUSCATE("  AXIOM DEVELOPMENT"));
    ImGui::Separator(); ImGui::Spacing();

    bool connected = (g_BalanceInstance != nullptr);
    bool hooksLive = g_hooksReady.load(std::memory_order_acquire);

    ImGui::TextColored(
        hooksLive ? ImVec4(0.2f,1.0f,0.2f,1.0f) : ImVec4(1.0f,0.85f,0.0f,1.0f),
        hooksLive ? OBFUSCATE("  [OK] Hooks Live")
                  : OBFUSCATE("  [..] Dang Hook...")
    );
    ImGui::TextColored(
        connected ? ImVec4(0.2f,1.0f,0.2f,1.0f) : ImVec4(1.0f,0.35f,0.35f,1.0f),
        connected ? OBFUSCATE("  [OK] Instance Bat Duoc")
                  : OBFUSCATE("  [..] Chua Co Instance — vao man choi")
    );

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f,0.75f,0.0f,1.0f), OBFUSCATE(" TIEN TE"));
    ImGui::Spacing();
    ImGui::Checkbox(OBFUSCATE("Tien Mem Vo Han"),       &SWITCH::InfiniteMoney);
    ImGui::Checkbox(OBFUSCATE("Tien Premium Vo Han"),   &SWITCH::InfinitePremium);
    ImGui::Checkbox(OBFUSCATE("Bo Quang Cao (No Ads)"), &SWITCH::NoAds);
    ImGui::Spacing();
    
    // Gán cờ an toàn, logic thực tế được xử lý ở Main Thread
    if (ImGui::Button(OBFUSCATE("Cap Nhat Tien Ngay"), ImVec2(-1.0f, 36.0f))) {
        SWITCH::ForceUpdateMoney = true;
    }

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f,0.75f,0.0f,1.0f), OBFUSCATE(" TIEN TRINH"));
    ImGui::Spacing();
    ImGui::Checkbox(OBFUSCATE("Max Level 99"), &SWITCH::MaxLevel);
    ImGui::Spacing();
    
    // Gán cờ an toàn, logic thực tế được xử lý ở Main Thread
    if (ImGui::Button(OBFUSCATE("Len Cap Ngay"), ImVec2(-1.0f, 36.0f))) {
        SWITCH::ForceMaxLevel = true;
    }

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f,0.75f,0.0f,1.0f), OBFUSCATE(" CHIEN DAU"));
    ImGui::Spacing();
    ImGui::Checkbox(OBFUSCATE("God Mode (Bat Tu)"), &SWITCH::GodMode);
    ImGui::Checkbox(OBFUSCATE("Speed Hack"),        &SWITCH::SpeedHack);
    if (SWITCH::SpeedHack) {
        ImGui::Spacing();
        ImGui::SliderFloat(OBFUSCATE("Toc Do x"), &speedMultiplier, 1.0f, 5.0f);
    }

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f,0.75f,0.0f,1.0f), OBFUSCATE(" BAO MAT"));
    ImGui::Spacing();
    ImGui::Checkbox(OBFUSCATE("Bypass Anti-Cheat"), &SWITCH::AntiCheat);

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.4f,0.4f,0.4f,1.0f),
        OBFUSCATE("frame=%d | inst=%p | fail=%d"),
        frame, g_BalanceInstance,
        g_failCount.load(std::memory_order_relaxed)
    );

    ImGui::End();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    return old_eglSwapBuffers(dpy, surface);
}

// ================================================================
//  JNI_OnLoad
// ================================================================
extern "C"
JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM* vm, void* reserved) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
        return JNI_VERSION_1_6;

    UnityPlayer_cls = env->FindClass(OBFUSCATE("com/unity3d/player/UnityPlayer"));
    if (!UnityPlayer_cls) return JNI_VERSION_1_6;

    UnityPlayer_CurrentActivity_fid = env->GetStaticFieldID(
        UnityPlayer_cls,
        OBFUSCATE("currentActivity"),
        OBFUSCATE("Landroid/app/Activity;")
    );

    DobbyHook(
        reinterpret_cast<void*>(env->functions->RegisterNatives),
        reinterpret_cast<void*>(hook_RegisterNatives),
        reinterpret_cast<void**>(&old_RegisterNatives)
    );

    return JNI_VERSION_1_6;
}

// ================================================================
//  HACK THREAD
// ================================================================
void* hack_thread(void*) {
    do { sleep(1); } while (!isLibraryLoaded(targetLibName));
    address = findLibrary(targetLibName);

    usleep(1500000); 

    if (!WaitForAssembliesReady(20000)) {
        return nullptr;
    }

    bool attached = false;
    for (int attempt = 0; attempt < 5 && !attached; attempt++) {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        AttachIl2Cpp();

        auto probe = getClass(OBFUSCATE("PlayerBalance"), OBFUSCATE("ThrowIO"));
        if (probe) {
            attached = true;
        } else {
            DetachIl2Cpp();
            usleep(2000000); 
        }
    }

    if (!attached) return nullptr;

    Menu::Screen_get_height = reinterpret_cast<int(*)()>(
        OBFBNM("UnityEngine", "Screen", "get_height", 0));
    Menu::Screen_get_width  = reinterpret_cast<int(*)()>(
        OBFBNM("UnityEngine", "Screen", "get_width",  0));

    BNM::LoadClass balanceClass    = getClass(OBFUSCATE("PlayerBalance"), OBFUSCATE("ThrowIO"));
    BNM::LoadClass charClass       = getClass(OBFUSCATE("Character"),     OBFUSCATE("ThrowIO"));
    BNM::LoadClass playerDataClass = getClass(OBFUSCATE("PlayerData"),    OBFUSCATE("ThrowIO"));
    BNM::LoadClass charWeaponClass = getClass(OBFUSCATE("CharWeapon"),    OBFUSCATE("ThrowIO"));

    auto safe_offset = [](BNM::LoadClass cls, const char* name) -> uintptr_t {
        if (!cls) return 0;
        uintptr_t off = getOffset(cls, name, 0);
        return off;
    };

    auto off_setSoftMoney = safe_offset(balanceClass,    OBFUSCATE("set_SoftMoney"));
    auto off_setHardMoney = safe_offset(balanceClass,    OBFUSCATE("set_HardMoney"));
    auto off_setLevel     = safe_offset(balanceClass,    OBFUSCATE("set_Level"));
    auto off_setExp       = safe_offset(balanceClass,    OBFUSCATE("set_Exp"));
    auto off_setNoAds     = safe_offset(balanceClass,    OBFUSCATE("set_NoAds"));
    auto off_getSoftMoney = safe_offset(balanceClass,    OBFUSCATE("get_SoftMoney"));
    auto off_getHardMoney = safe_offset(balanceClass,    OBFUSCATE("get_HardMoney"));
    auto off_getLevel     = safe_offset(balanceClass,    OBFUSCATE("get_Level"));
    auto off_applyDamage  = safe_offset(charClass,       OBFUSCATE("ApplyDamage"));
    auto off_setDeath     = safe_offset(charClass,       OBFUSCATE("SetDeath"));
    auto off_cwUpdate     = safe_offset(charWeaponClass, OBFUSCATE("update"));
    auto off_saveLocal    = safe_offset(playerDataClass, OBFUSCATE("SaveLocal"));

    if (off_setSoftMoney) AddPointer(set_SoftMoney, off_setSoftMoney);
    if (off_setHardMoney) AddPointer(set_HardMoney, off_setHardMoney);
    if (off_setLevel)     AddPointer(set_Level,     off_setLevel);
    if (off_setExp)       AddPointer(set_Exp,       off_setExp);
    if (off_setNoAds)     AddPointer(set_NoAds,     off_setNoAds);
    if (off_getSoftMoney) AddPointer(get_SoftMoney, off_getSoftMoney);
    if (off_getHardMoney) AddPointer(get_HardMoney, off_getHardMoney);
    if (off_getLevel)     AddPointer(get_Level,     off_getLevel);

    DetachIl2Cpp();
    std::atomic_thread_fence(std::memory_order_seq_cst);

    if (off_setSoftMoney) DHK(off_setSoftMoney, capture_set_SoftMoney,  orig_set_SoftMoney);
    if (off_applyDamage)  DHK(off_applyDamage,  hook_ApplyDamage,       old_ApplyDamage);
    if (off_setDeath)     DHK(off_setDeath,      hook_SetDeath,          old_SetDeath);
    if (off_cwUpdate)     DHK(off_cwUpdate,      hook_CharWeapon_update, old_CharWeapon_update);
    if (off_saveLocal)    DHK(off_saveLocal,      hook_SaveLocal,         old_SaveLocal);

    g_hooksReady.store(true, std::memory_order_release);
    return nullptr;
}

// ================================================================
//  ENTRY POINT
// ================================================================
__attribute__((constructor))
void lib_main() {
    InitCrashLogger();

    void* eglhandle = nullptr;
    for (int retry = 0; retry < 30 && !eglhandle; retry++) {
        eglhandle = dlopen(OBFUSCATE("libEGL.so"), RTLD_NOW | RTLD_GLOBAL);
        if (!eglhandle) {
            usleep(100000);
        }
    }

    if (!eglhandle) return;

    void* eglSwapSym = dlsym(eglhandle, OBFUSCATE("eglSwapBuffers"));
    if (!eglSwapSym) return;

    DHK(eglSwapSym, hook_eglSwapBuffers, old_eglSwapBuffers);

    pthread_t ptid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 4 * 1024 * 1024);

    pthread_create(&ptid, &attr, hack_thread, nullptr);
    pthread_attr_destroy(&attr);
}
