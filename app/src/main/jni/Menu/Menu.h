#ifndef MENU
#define MENU
#include "ImGui/imgui.h"
#include "Themes.h"
#include "../Data/Fonts/Roboto-Regular.h"

using namespace ImGui;
static bool init;
int glWidth, glHeight;

// ================================================================
//  EXTERN — Trỏ về biến/hàm đã khai báo trong Main.cpp
//  KHÔNG khai báo lại ở đây, chỉ báo compiler "nó tồn tại ở chỗ khác"
// ================================================================
extern void*  g_BalanceInstance;
extern void (*set_SoftMoney)(void* instance, long value);
extern void (*set_HardMoney)(void* instance, long value);
extern void (*set_Level)    (void* instance, int  value);
extern void (*set_Exp)      (void* instance, int  value);

void SetupImGui() {
    if (!init) {
        auto context = ImGui::CreateContext();
        if (!context) return;

        ImGuiIO &io = ImGui::GetIO();
        ImFontConfig font_cfg;
        io.DisplaySize = ImVec2((float)glWidth, (float)glHeight);
        font_cfg.SizePixels = 22.0f;
        io.Fonts->AddFontFromMemoryTTF(Roboto_Regular, 22, 22.0f);
        io.IniFilename = NULL;

        io.KeyMap[ImGuiKey_UpArrow]    = 19;
        io.KeyMap[ImGuiKey_DownArrow]  = 20;
        io.KeyMap[ImGuiKey_LeftArrow]  = 21;
        io.KeyMap[ImGuiKey_RightArrow] = 22;
        io.KeyMap[ImGuiKey_Enter]      = 66;
        io.KeyMap[ImGuiKey_Backspace]  = 67;
        io.KeyMap[ImGuiKey_PageUp]     = 92;
        io.KeyMap[ImGuiKey_PageDown]   = 93;
        io.KeyMap[ImGuiKey_Escape]     = 111;
        io.KeyMap[ImGuiKey_Delete]     = 112;
        io.KeyMap[ImGuiKey_Home]       = 122;
        io.KeyMap[ImGuiKey_End]        = 123;
        io.KeyMap[ImGuiKey_Insert]     = 124;

        Theme::SetCorporateGrayTheme();
        ImGui::GetStyle().ScaleAllSizes(3.0f);
        ImGui_ImplAndroid_Init(nullptr);
        ImGui_ImplOpenGL3_Init();
        init = true;
    }
}

namespace Menu {

    // ================================================================
    //  SWITCH - TRẠNG THÁI CÁC CHỨC NĂNG
    // ================================================================
    struct {
        bool TienMemVoHan    = false;
        bool KimCuongVoHan   = false;
        bool BoQuangCao      = false;
        bool MaxLevel        = false;
        bool BatTu           = false;
        bool TangTocDo       = false;
        bool BypassAC        = true;
    } SWITCH;

    struct {
        MemoryPatch minimap, map;
    } Patch;

    int (*Screen_get_width)()  = nullptr;
    int (*Screen_get_height)() = nullptr;
    bool* p_open;
    float tocDoNhan = 2.0f;

    // ================================================================
    //  DRAW MENU
    // ================================================================
    void DrawMenu() {
        ImGuiIO &io = ImGui::GetIO();

        static bool showStyleEditor = false;
        static bool showFPS         = false;

        ImGui::SetNextWindowSize(ImVec2(400, 480), ImGuiCond_FirstUseEver);
        ImGui::Begin("  THROW.IO MOD  |  AXIOM  ", p_open, ImGuiWindowFlags_MenuBar);

        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("Cong Cu")) {
                ImGui::MenuItem("Style Editor", NULL, &showStyleEditor);
                ImGui::MenuItem("FPS Viewer",   NULL, &showFPS);
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f), "  AXIOM DEVELOPMENT");
        ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f), "  ThrowIO IL2CPP | BNM Auto-Offset");
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::BeginTabBar("##Tabs")) {

            // ── TAB: TIỀN TỆ ──────────────────────────────────────────
            if (ImGui::BeginTabItem("Tien Te")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, 1.0f), "[ Quan Ly Tien ]");
                ImGui::Spacing();

                ImGui::Checkbox("Tien Mem Vo Han",  &SWITCH.TienMemVoHan);
                if (SWITCH.TienMemVoHan)
                    ImGui::TextColored(ImVec4(0,1,0,1), "  >> DANG HOAT DONG");

                ImGui::Spacing();
                ImGui::Checkbox("Kim Cuong Vo Han", &SWITCH.KimCuongVoHan);
                if (SWITCH.KimCuongVoHan)
                    ImGui::TextColored(ImVec4(0,1,0,1), "  >> DANG HOAT DONG");

                ImGui::Spacing();
                ImGui::Checkbox("Bo Quang Cao", &SWITCH.BoQuangCao);

                ImGui::Spacing();
                // Dùng thẳng biến global (extern ở trên) — không cần prefix gì
                if (ImGui::Button("Cap Nhat Tien Ngay!", ImVec2(-1, 40))) {
                    if (set_SoftMoney && g_BalanceInstance)
                        set_SoftMoney(g_BalanceInstance, 0x7FFFFFFF);
                    if (set_HardMoney && g_BalanceInstance)
                        set_HardMoney(g_BalanceInstance, 0x7FFFFFFF);
                }
                ImGui::EndTabItem();
            }

            // ── TAB: TIẾN TRÌNH ───────────────────────────────────────
            if (ImGui::BeginTabItem("Tien Trinh")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, 1.0f), "[ Cap Do & Kinh Nghiem ]");
                ImGui::Spacing();

                ImGui::Checkbox("Max Level 99", &SWITCH.MaxLevel);
                if (SWITCH.MaxLevel)
                    ImGui::TextColored(ImVec4(0,1,0,1), "  >> DANG HOAT DONG");

                ImGui::Spacing();
                if (ImGui::Button("Len Cap Ngay!", ImVec2(-1, 40))) {
                    if (set_Level && g_BalanceInstance)
                        set_Level(g_BalanceInstance, 99);
                    if (set_Exp && g_BalanceInstance)
                        set_Exp(g_BalanceInstance, 0x7FFFFFFF);
                }
                ImGui::EndTabItem();
            }

            // ── TAB: CHIẾN ĐẤU ───────────────────────────────────────
            if (ImGui::BeginTabItem("Chien Dau")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, 1.0f), "[ Chien Dau ]");
                ImGui::Spacing();

                ImGui::Checkbox("Bat Tu (God Mode)", &SWITCH.BatTu);
                if (SWITCH.BatTu)
                    ImGui::TextColored(ImVec4(0,1,0,1), "  >> DANG HOAT DONG");

                ImGui::Spacing();
                ImGui::Checkbox("Tang Toc Do", &SWITCH.TangTocDo);
                if (SWITCH.TangTocDo) {
                    ImGui::TextColored(ImVec4(0,1,0,1), "  >> DANG HOAT DONG");
                    ImGui::SliderFloat("He So Toc Do", &tocDoNhan, 1.0f, 5.0f);
                }
                ImGui::EndTabItem();
            }

            // ── TAB: BẢO MẬT ─────────────────────────────────────────
            if (ImGui::BeginTabItem("Bao Mat")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "[ Chong Phat Hien ]");
                ImGui::Spacing();

                ImGui::Checkbox("Bypass Anti-Cheat", &SWITCH.BypassAC);
                if (SWITCH.BypassAC)
                    ImGui::TextColored(ImVec4(0,1,0,1), "  >> DANG HOAT DONG");

                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f), "[!] Hook vao SaveLocal");
                ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f), "[!] Khong tang qua nhieu 1 luc");
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        if (showStyleEditor) {
            ImGui::Begin("Style Editor", &showStyleEditor);
            ImGui::ShowStyleEditor();
            ImGui::End();
        }
        if (showFPS) {
            ImGui::SetNextWindowBgAlpha(0.4f);
            ImGui::Begin("FPS", &showFPS,
                ImGuiWindowFlags_NoDecoration |
                ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoSavedSettings);
            ImGui::Text("%.1f FPS", ImGui::GetIO().Framerate);
            ImGui::End();
        }

        ImGui::TextColored(ImVec4(0.3f, 0.3f, 0.3f, 1.0f), "%.1f FPS", ImGui::GetIO().Framerate);
        ImGui::End();
    }

    // ================================================================
    //  DRAW IMGUI MENU - GỌI MỖI FRAME
    // ================================================================
    void DrawImGuiMenu() {
        if (init && Screen_get_height) {
            ImGuiIO &io = ImGui::GetIO();
            static bool WantTextInputLast = false;
            if (io.WantTextInput && !WantTextInputLast) displayKeyboard(true);
            WantTextInputLast = io.WantTextInput;

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplAndroid_NewFrame(Screen_get_width(), Screen_get_height());
            ImGui::NewFrame();
            DrawMenu();
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            io.KeysDown[io.KeyMap[ImGuiKey_UpArrow]]    = false;
            io.KeysDown[io.KeyMap[ImGuiKey_DownArrow]]  = false;
            io.KeysDown[io.KeyMap[ImGuiKey_LeftArrow]]  = false;
            io.KeysDown[io.KeyMap[ImGuiKey_RightArrow]] = false;
            io.KeysDown[io.KeyMap[ImGuiKey_Tab]]        = false;
            io.KeysDown[io.KeyMap[ImGuiKey_Enter]]      = false;
            io.KeysDown[io.KeyMap[ImGuiKey_Backspace]]  = false;
            io.KeysDown[io.KeyMap[ImGuiKey_PageUp]]     = false;
            io.KeysDown[io.KeyMap[ImGuiKey_PageDown]]   = false;
            io.KeysDown[io.KeyMap[ImGuiKey_Escape]]     = false;
            io.KeysDown[io.KeyMap[ImGuiKey_Delete]]     = false;
            io.KeysDown[io.KeyMap[ImGuiKey_Home]]       = false;
            io.KeysDown[io.KeyMap[ImGuiKey_End]]        = false;
            io.KeysDown[io.KeyMap[ImGuiKey_Insert]]     = false;
            ImGui::EndFrame();
        }
    }
}

#endif //MENU
