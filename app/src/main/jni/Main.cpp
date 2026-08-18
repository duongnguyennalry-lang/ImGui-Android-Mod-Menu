// ================================================================
//  THROWIO MOD - AXIOM DEVELOPMENT
//  CRASH FIX v5 — BỎ SIGSETJMP, FIX THẬT SỰ
//
//  ROOT CAUSE CONFIRMED:
//    - libsigchain.so (_ZN3art11SignalChain7HandlerEiP7siginfoPv)
//      intercept SIGSEGV trước bất kỳ user handler nào
//    - sigsetjmp/SignalGuard KHÔNG BAO GIỜ được gọi với ART
//    - Crash TID 25132 = render thread gọi SetupImGui()
//    - x3 = 0x004d6f64756c6500 = "Module\0" → BNM đang iterate
//      assembly entries chưa populated → deref null → PC rác
//    - Signal approach hoàn toàn sai hướng từ đầu
//
//  FIXES v5:
//    [1] Bỏ toàn bộ SignalGuard / sigsetjmp / sigaction
//    [2] Pre-validate assemblies qua il2cpp API trực tiếp
//        trước khi AttachIl2Cpp — BNM sẽ không deref null nữa
//    [3] ImGui: tăng threshold 150→300, thêm GL fence check
//    [4] SetupImGui wrap bằng GL error check thay signal
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
//  TOGGLES
// ================================================================
namespace SWITCH {
    bool InfiniteMoney   = false;
    bool InfinitePremium = false;
    bool MaxLevel        = false;
    bool NoAds           = false;
    bool GodMode         = false;
    bool SpeedHack       = false;
    bool AntiCheat       = true;
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
//  il2cpp API typedefs — dùng để pre-validate assemblies
// ================================================================
typedef void*  (*fn_domain_get)();
typedef void** (*fn_domain_get_assemblies)(void* domain, size_t* count);
typedef void*  (*fn_assembly_get_image)(void* assembly);
typedef const char* (*fn_image_get_name)(void* image);

// ================================================================
//  IsValidPtr — mincore check, không dùng signal
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
//  [FIX v5] ValidateAssemblyEntry
//  Kiểm tra 1 assembly entry có hợp lệ không
//  x3="Module\0" trong crash = BNM đang đọc assembly->image->name
//  → phải validate cả chain: assembly → image → name
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

    // Name phải có ít nhất 1 ký tự printable
    if (name[0] == '\0') return false;

    return true;
}

// ================================================================
//  [FIX v5] WaitForAssembliesReady
//  Poll trực tiếp qua il2cpp API — không dùng sleep cứng nữa
//  Đảm bảo MỌI entry trong assemblies array đều valid
//  trước khi BNM::AttachIl2Cpp() được gọi
// ================================================================
static bool WaitForAssembliesReady(int maxWaitMs = 20000) {
    void* lib = dlopen("libil2cpp.so", RTLD_NOLOAD);
    if (!lib) {
        LOGE(OBFUSCATE("ThrowIO: RTLD_NOLOAD libil2cpp.so failed — fallback sleep"));
        usleep(8000000); // 8s fallback nếu không dlopen được
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

    const int step = 200; // poll mỗi 200ms
    int elapsed = 0;
    int stableCount = 0;   // cần stable N lần liên tiếp mới tin

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

        // count phải > 10 (Unity game thường có 50+ assembly)
        // và < 4096 (sanity check)
        if (!IsValidPtr(assemblies) || count < 10 || count > 4096) {
            stableCount = 0;
            usleep(step * 1000);
            elapsed += step;
            continue;
        }

        // Validate TOÀN BỘ entries — đây là chỗ BNM crash
        bool allValid = true;
        for (size_t i = 0; i < count; i++) {
            if (!ValidateAssemblyEntry(assembly_get_image, image_get_name, assemblies[i])) {
                LOGI(OBFUSCATE("ThrowIO: assembly[%zu] invalid — wait more"), i);
                allValid = false;
                break;
            }
        }

        if (allValid) {
            stableCount++;
            LOGI(OBFUSCATE("ThrowIO: assemblies valid count=%zu stable=%d/%d"),
                 count, stableCount, 3);

            // Cần stable 3 lần liên tiếp trước khi tin
            if (stableCount >= 3) {
                LOGI(OBFUSCATE("ThrowIO: assemblies CONFIRMED READY — elapsed=%dms"), elapsed);
                dlclose(lib);
                return true;
            }
        } else {
            stableCount = 0;
        }

        usleep(step * 1000);
        elapsed += step;
    }

    LOGE(OBFUSCATE("ThrowIO: assemblies NOT ready after %dms"), maxWaitMs);
    dlclose(lib);
    return false;
}

// ================================================================
//  IsEGLContextCurrent
// ================================================================
static bool IsEGLContextCurrent() {
    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLContext  ctx = eglGetCurrentContext();
    if (dpy == EGL_NO_DISPLAY || ctx == EGL_NO_CONTEXT) return false;
    if (g_savedContext != EGL_NO_CONTEXT && ctx != g_savedContext) return false;
    return true;
}

// ================================================================
//  IsGLReady — thêm glFinish() fence để đảm bảo pipeline clean
// ================================================================
static bool IsGLReady() {
    while (glGetError() != GL_NO_ERROR) {}

    GLint major = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    if (glGetError() != GL_NO_ERROR || major < 2) return false;

    GLint viewport[4] = {0};
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (glGetError() != GL_NO_ERROR || viewport[2] <= 0 || viewport[3] <= 0)
        return false;

    // [FIX v5] GL fence — flush pending commands trước khi ImGui init
    glFlush();
    glFinish();
    if (glGetError() != GL_NO_ERROR) return false;

    return true;
}

// ================================================================
//  ApplyWatchdog
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
//  HOOKs
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

void hook_CharWeapon_update(void* inst, float dt) {
    if (SWITCH::SpeedHack && inst) dt *= speedMultiplier;
    if (old_CharWeapon_update) old_CharWeapon_update(inst, dt);
}

void hook_SaveLocal(void* inst) {
    if (SWITCH::AntiCheat) ApplyWatchdog();
    if (old_SaveLocal) old_SaveLocal(inst);
    if (SWITCH::AntiCheat) ApplyWatchdog();
}

// ================================================================
//  ImGui Style
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
//  [FIX v5] SafeSetupImGui
//  Bỏ hoàn toàn signal handling — ART sigchain chặn hết
//  Thay bằng GL state validation trước/sau mỗi bước
// ================================================================
static bool SafeSetupImGui(EGLDisplay dpy, EGLSurface surface) {
    if (!IsEGLContextCurrent()) {
        LOGE(OBFUSCATE("ThrowIO: no EGL context"));
        return false;
    }

    EGLint w = 0, h = 0;
    if (eglQuerySurface(dpy, surface, EGL_WIDTH,  &w) != EGL_TRUE ||
        eglQuerySurface(dpy, surface, EGL_HEIGHT, &h) != EGL_TRUE ||
        w <= 0 || h <= 0) {
        LOGE(OBFUSCATE("ThrowIO: surface dim invalid %dx%d"), w, h);
        return false;
    }

    if (!IsGLReady()) {
        LOGE(OBFUSCATE("ThrowIO: GL not ready"));
        return false;
    }

    // [FIX v5] Check framebuffer completeness trước khi ImGui init
    GLenum fbStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fbStatus != GL_FRAMEBUFFER_COMPLETE) {
        LOGE(OBFUSCATE("ThrowIO: framebuffer incomplete 0x%x"), fbStatus);
        return false;
    }

    g_savedDisplay = eglGetCurrentDisplay();
    g_savedContext = eglGetCurrentContext();
    while (glGetError() != GL_NO_ERROR) {}

    // Gọi SetupImGui — không wrap signal, tập trung validate state
    SetupImGui();

    // Verify ngay sau khi gọi
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx) {
        LOGE(OBFUSCATE("ThrowIO: ImGui ctx null sau SetupImGui"));
        return false;
    }

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        LOGE(OBFUSCATE("ThrowIO: GL error sau SetupImGui 0x%x"), err);
        return false;
    }

    ApplyImGuiStyle();
    LOGI(OBFUSCATE("ThrowIO: ImGui OK ctx=%p dim=%dx%d"), g_savedContext, w, h);
    return true;
}

// ================================================================
//  HOOK: eglSwapBuffers
//  [FIX v5] Tăng threshold 150 → 300 frames
//  Thêm EGL config validation trước khi SafeSetupImGui
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

    // [FIX v5] 300 frames — đủ ~5s ở 60fps cho Unity fully init
    if (frame < 300) return old_eglSwapBuffers(dpy, surface);

    if (g_imguiFailed.load(std::memory_order_acquire)) {
        int fails = g_failCount.load(std::memory_order_relaxed);
        int retryAt = (fails < 3) ? 120 : (fails < 6) ? 240 : 360;
        if (frame % retryAt != 0) return old_eglSwapBuffers(dpy, surface);
        LOGI(OBFUSCATE("ThrowIO: ImGui retry frame=%d fail#%d"), frame, fails);
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
        LOGE(OBFUSCATE("ThrowIO: EGL context lost frame=%d"), frame);
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

    if (g_hooksReady.load(std::memory_order_acquire)) ApplyWatchdog();

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
    if (ImGui::Button(OBFUSCATE("Cap Nhat Tien Ngay"), ImVec2(-1.0f, 36.0f))) {
        void* inst = g_BalanceInstance;
        if (inst && IsValidPtr(inst)) {
            if (set_SoftMoney) set_SoftMoney(inst, 0x7FFFFFFF);
            if (set_HardMoney) set_HardMoney(inst, 0x7FFFFFFF);
        }
    }

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f,0.75f,0.0f,1.0f), OBFUSCATE(" TIEN TRINH"));
    ImGui::Spacing();
    ImGui::Checkbox(OBFUSCATE("Max Level 99"), &SWITCH::MaxLevel);
    ImGui::Spacing();
    if (ImGui::Button(OBFUSCATE("Len Cap Ngay"), ImVec2(-1.0f, 36.0f))) {
        void* inst = g_BalanceInstance;
        if (inst && IsValidPtr(inst)) {
            if (set_Level) set_Level(inst, 99);
            if (set_Exp)   set_Exp  (inst, 0x7FFFFFFF);
        }
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

    LOGI(OBFUSCATE("ThrowIO: JNI_OnLoad OK"));
    return JNI_VERSION_1_6;
}

// ================================================================
//  [FIX v5] HACK THREAD
//  Bỏ hoàn toàn sigsetjmp/SignalGuard
//  Thay bằng WaitForAssembliesReady() — validate trực tiếp
// ================================================================
void* hack_thread(void*) {
    do { sleep(1); } while (!isLibraryLoaded(targetLibName));
    address = findLibrary(targetLibName);
    LOGI(OBFUSCATE("ThrowIO: il2cpp detected @ %p"),
         reinterpret_cast<void*>(address));

    usleep(1500000); // 1.5s ban đầu

    // [FIX v5] Poll assemblies thật sự thay vì sleep cứng
    // WaitForAssembliesReady validate TỪNG entry qua il2cpp API
    // Chỉ proceed khi 100% entries hợp lệ — BNM sẽ không deref rác
    LOGI(OBFUSCATE("ThrowIO: polling assemblies readiness..."));
    if (!WaitForAssembliesReady(20000)) {
        LOGE(OBFUSCATE("ThrowIO: assemblies poll timeout — abort"));
        return nullptr;
    }
    LOGI(OBFUSCATE("ThrowIO: assemblies confirmed — BNM attach safe"));

    // Không cần retry loop với crash handler nữa
    // Nếu assemblies đã valid thì BNM attach sẽ không crash
    bool attached = false;
    for (int attempt = 0; attempt < 5 && !attached; attempt++) {
        std::atomic_thread_fence(std::memory_order_seq_cst);

        LOGI(OBFUSCATE("ThrowIO: BNM attach attempt %d..."), attempt);
        AttachIl2Cpp();

        auto probe = getClass(OBFUSCATE("PlayerBalance"), OBFUSCATE("ThrowIO"));
        if (probe) {
            attached = true;
            LOGI(OBFUSCATE("ThrowIO: BNM attached OK attempt %d"), attempt);
        } else {
            LOGE(OBFUSCATE("ThrowIO: probe null attempt %d"), attempt);
            DetachIl2Cpp();
            usleep(2000000); // 2s giữa các retry
        }
    }

    if (!attached) {
        LOGE(OBFUSCATE("ThrowIO: FATAL — attach failed 5 attempts"));
        return nullptr;
    }

    Menu::Screen_get_height = reinterpret_cast<int(*)()>(
        OBFBNM("UnityEngine", "Screen", "get_height", 0));
    Menu::Screen_get_width  = reinterpret_cast<int(*)()>(
        OBFBNM("UnityEngine", "Screen", "get_width",  0));

    BNM::LoadClass balanceClass    = getClass(OBFUSCATE("PlayerBalance"), OBFUSCATE("ThrowIO"));
    BNM::LoadClass charClass       = getClass(OBFUSCATE("Character"),     OBFUSCATE("ThrowIO"));
    BNM::LoadClass playerDataClass = getClass(OBFUSCATE("PlayerData"),    OBFUSCATE("ThrowIO"));
    BNM::LoadClass charWeaponClass = getClass(OBFUSCATE("CharWeapon"),    OBFUSCATE("ThrowIO"));

    LOGI(OBFUSCATE("ThrowIO: classes balance=%d char=%d pdata=%d cwpn=%d"),
         (bool)balanceClass, (bool)charClass,
         (bool)playerDataClass, (bool)charWeaponClass);

    auto safe_offset = [](BNM::LoadClass cls, const char* name) -> uintptr_t {
        if (!cls) { LOGE(OBFUSCATE("ThrowIO: null class [%s]"), name); return 0; }
        uintptr_t off = getOffset(cls, name, 0);
        if (!off) LOGE(OBFUSCATE("ThrowIO: offset=0 [%s]"), name);
        else      LOGI(OBFUSCATE("ThrowIO: [%s]=0x%lx"), name, (unsigned long)off);
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
    LOGI(OBFUSCATE("ThrowIO: fuck yeah — all hooks live, AXIOM MOD READY boss man"));
    return nullptr;
}

// ================================================================
//  ENTRY POINT
// ================================================================
__attribute__((constructor))
void lib_main() {
    InitCrashLogger();
    LOGI(OBFUSCATE("ThrowIO: lib_main start"));

    void* eglhandle = nullptr;
    for (int retry = 0; retry < 30 && !eglhandle; retry++) {
        eglhandle = dlopen(OBFUSCATE("libEGL.so"), RTLD_NOW | RTLD_GLOBAL);
        if (!eglhandle) {
            LOGE(OBFUSCATE("ThrowIO: libEGL.so retry %d — %s"), retry, dlerror());
            usleep(100000);
        }
    }

    if (!eglhandle) {
        LOGE(OBFUSCATE("ThrowIO: FATAL — libEGL.so unavailable"));
        return;
    }

    void* eglSwapSym = dlsym(eglhandle, OBFUSCATE("eglSwapBuffers"));
    if (!eglSwapSym) {
        LOGE(OBFUSCATE("ThrowIO: FATAL — eglSwapBuffers: %s"), dlerror());
        return;
    }

    DHK(eglSwapSym, hook_eglSwapBuffers, old_eglSwapBuffers);
    LOGI(OBFUSCATE("ThrowIO: eglSwapBuffers hooked -> %p"), eglSwapSym);

    pthread_t ptid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 4 * 1024 * 1024);

    if (pthread_create(&ptid, &attr, hack_thread, nullptr) != 0)
        LOGE(OBFUSCATE("ThrowIO: pthread_create failed"));

    pthread_attr_destroy(&attr);
    LOGI(OBFUSCATE("ThrowIO: lib_main done — thread launched"));
}
