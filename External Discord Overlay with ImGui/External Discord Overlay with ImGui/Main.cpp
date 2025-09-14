#include "Rendering.h"
#include <windows.h>
#include <tlhelp32.h>
#include <vector>
#include <string>
#include <cstdint>
#include <algorithm>
#include <chrono>
#include <cmath>

struct Player {
    std::string key;
    std::string name;
    std::string role;
    bool alive;
    uint32_t colorId;
    std::string colorName;
    std::string colorHex;
    float x;
    float y;
};

static const uint32_t steamOffset = 0x0298784C;
static const uint64_t epicOffset = 0x0327E990;

static const char* colorNames[18] = {
    "Red","Blue","Green","Pink","Orange","Yellow","Black","White","Purple",
    "Brown","Cyan","Lime","Maroon","Rose","Banana","Grey","Tan","Coral"
};

static const char* colorHexes[18] = {
    "#D71E22","#1D3CE9","#1B913E","#FF63D4","#FF8D1C","#FFFF67","#4A565E",
    "#E9F7FF","#783DD2","#80582D","#44FFF7","#5BFE4B","#6C2B3D","#FFD6EC",
    "#FFFFBE","#8397A7","#9F9989","#EC7578"
};

template<typename T>
static bool readMem(HANDLE h, uintptr_t addr, T& out) {
    SIZE_T got = 0;
    return addr && ReadProcessMemory(h, reinterpret_cast<LPCVOID>(addr), &out, sizeof(T), &got) && got == sizeof(T);
}

static bool readBytes(HANDLE h, uintptr_t addr, std::vector<uint8_t>& out, SIZE_T bytes) {
    if (!addr) return false;
    out.resize(bytes);
    SIZE_T got = 0;
    if (!ReadProcessMemory(h, reinterpret_cast<LPCVOID>(addr), out.data(), bytes, &got)) return false;
    return got == bytes;
}

static uintptr_t moduleBase(DWORD pid, const wchar_t* module) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W me{}; me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            if (lstrcmpiW(me.szModule, module) == 0) {
                CloseHandle(snap);
                return reinterpret_cast<uintptr_t>(me.modBaseAddr);
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return 0;
}

enum class Platform { Unknown, Steam, Epic };

struct MemContext {
    HANDLE process;
    DWORD pid;
    uintptr_t gameAsm;
    Platform platform;
    uintptr_t base;
};

static Platform detectPlatform(const MemContext& ctx) {
    uint64_t t64a = 0, t64b = 0;
    if (readMem<uint64_t>(ctx.process, ctx.gameAsm + epicOffset, t64a) && t64a) {
        if (readMem<uint64_t>(ctx.process, t64a + 0xB8, t64b) && t64b) return Platform::Epic;
    }
    uint32_t t32a = 0, t32b = 0;
    if (readMem<uint32_t>(ctx.process, ctx.gameAsm + steamOffset, t32a) && t32a) {
        if (readMem<uint32_t>(ctx.process, t32a + 0x5C, t32b) && t32b) return Platform::Steam;
    }
    return Platform::Unknown;
}

static bool resolveBase(MemContext& ctx) {
    ctx.platform = detectPlatform(ctx);
    if (ctx.platform == Platform::Steam) {
        uint32_t a = 0, b = 0, c = 0;
        if (!readMem<uint32_t>(ctx.process, ctx.gameAsm + steamOffset, a)) return false;
        if (!readMem<uint32_t>(ctx.process, a + 0x5C, b)) return false;
        if (!readMem<uint32_t>(ctx.process, b, c)) return false;
        ctx.base = c;
        return ctx.base != 0;
    }
    else if (ctx.platform == Platform::Epic) {
        uint64_t a = 0, b = 0, c = 0;
        if (!readMem<uint64_t>(ctx.process, ctx.gameAsm + epicOffset, a)) return false;
        if (!readMem<uint64_t>(ctx.process, a + 0xB8, b)) return false;
        if (!readMem<uint64_t>(ctx.process, b, c)) return false;
        ctx.base = static_cast<uintptr_t>(c);
        return ctx.base != 0;
    }
    return false;
}

static std::string roleName(uint32_t id) {
    switch (id) {
    case 0:  return "Crewmate";
    case 1:  return "Impostor";
    case 2:  return "Scientist";
    case 3:  return "Engineer";
    case 4:  return "Guardian Angel";
    case 5:  return "Shapeshifter";
    case 6:  return "Dead";
    case 7:  return "Dead (Imp)";
    case 8:  return "Noise Maker";
    case 9:  return "Phantom";
    case 10: return "Tracker";
    case 12: return "Detective";
    case 18: return "Viper";
    default: return "Unknown(" + std::to_string(id) + ")";
    }
}

static std::string colorNameFrom(uint32_t id) {
    if (id < 18) return colorNames[id];
    return std::to_string(id);
}

static std::string colorHexFrom(uint32_t id) {
    if (id < 18) return colorHexes[id];
    return std::string("#AAAAAA");
}

static std::string readUtf16(HANDLE h, uintptr_t strPtr, bool x64) {
    if (!strPtr) return {};
    uint32_t len32 = 0;
    uintptr_t lenAddr = strPtr + (x64 ? 0x10 : 0x8);
    if (!readMem<uint32_t>(h, lenAddr, len32)) return {};
    if (len32 == 0 || len32 > 256) return {};
    uintptr_t data = strPtr + (x64 ? 0x14 : 0xC);
    std::vector<uint8_t> raw;
    if (!readBytes(h, data, raw, SIZE_T(len32 * 2))) return {};
    std::wstring ws; ws.resize(len32);
    memcpy(ws.data(), raw.data(), len32 * 2);
    int outLen = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), len32, nullptr, 0, nullptr, nullptr);
    std::string out; out.resize(outLen);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), len32, out.data(), outLen, nullptr, nullptr);
    return out;
}

static ImVec4 srgbToLinear(ImVec4 c) {
    auto f = [](float u) { return u <= 0.04045f ? u / 12.92f : powf((u + 0.055f) / 1.055f, 2.4f); };
    return ImVec4(f(c.x), f(c.y), f(c.z), c.w);
}

static ImVec4 hexToColor(const std::string& hex) {
    if (hex.size() < 7 || hex[0] != '#') return ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
    auto val = [&](char c)->int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return 0;
        };
    int r = val(hex[1]) * 16 + val(hex[2]);
    int g = val(hex[3]) * 16 + val(hex[4]);
    int b = val(hex[5]) * 16 + val(hex[6]);
    return srgbToLinear(ImVec4(r / 255.f, g / 255.f, b / 255.f, 1.0f));
}

static bool isRedRole(const std::string& r) {
    return r == "Impostor" || r == "Shapeshifter" || r == "Phantom" || r == "Viper";
}

static std::vector<Player> readPlayers(const MemContext& ctx) {
    std::vector<Player> out;
    if (!ctx.base) return out;
    if (ctx.platform == Platform::Steam) {
        uint32_t allclients = 0, items = 0, count = 0;
        if (!readMem<uint32_t>(ctx.process, ctx.base + 0x38, allclients)) return out;
        if (!readMem<uint32_t>(ctx.process, allclients + 0x8, items)) return out;
        if (!readMem<uint32_t>(ctx.process, allclients + 0xC, count)) return out;
        if (!allclients || !items || count == 0) return out;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t itemBase = 0;
            if (!readMem<uint32_t>(ctx.process, items + 0x10 + i * 4, itemBase) || !itemBase) continue;
            uint32_t itemChar = 0, itemData = 0, rolePtr = 0, role = 0;
            if (!readMem<uint32_t>(ctx.process, itemBase + 0x10, itemChar) || !itemChar) continue;
            if (!readMem<uint32_t>(ctx.process, itemChar + 0x58, itemData) || !itemData) continue;
            if (!readMem<uint32_t>(ctx.process, itemData + 0x4C, rolePtr) || !rolePtr) continue;
            readMem<uint32_t>(ctx.process, rolePtr + 0x10, role);
            std::string roleS = roleName(role);
            uint32_t rb2d = 0, rb2d_cached = 0;
            float px = 0.f, py = 0.f;
            if (readMem<uint32_t>(ctx.process, itemChar + 0xD0, rb2d) && readMem<uint32_t>(ctx.process, rb2d + 0x8, rb2d_cached)) {
                readMem<float>(ctx.process, rb2d_cached + 0x7C, px);
                readMem<float>(ctx.process, rb2d_cached + 0x80, py);
            }
            uint32_t cid = 0; readMem<uint32_t>(ctx.process, itemBase + 0x28, cid);
            uint32_t namePtr = 0; readMem<uint32_t>(ctx.process, itemBase + 0x1C, namePtr);
            std::string name = readUtf16(ctx.process, namePtr, false);
            bool alive = !(roleS == "Dead" || roleS == "Dead (Imp)" || roleS == "Guardian Angel");
            Player p{ name,name,roleS,alive,cid,colorNameFrom(cid),colorHexFrom(cid),px,py };
            if (!p.name.empty()) out.push_back(std::move(p));
        }
    }
    else if (ctx.platform == Platform::Epic) {
        uint64_t allclients = 0, items = 0; uint32_t count = 0;
        if (!readMem<uint64_t>(ctx.process, ctx.base + 0x58, allclients)) return out;
        if (!readMem<uint64_t>(ctx.process, allclients + 0x10, items)) return out;
        if (!readMem<uint32_t>(ctx.process, allclients + 0x18, count)) return out;
        if (!allclients || !items || count == 0) return out;
        for (uint32_t i = 0; i < count; i++) {
            uint64_t itemBase = 0;
            if (!readMem<uint64_t>(ctx.process, items + 0x20 + i * 8, itemBase) || !itemBase) continue;
            uint64_t itemChar = 0, itemData = 0, rolePtr = 0; uint32_t role = 0;
            if (!readMem<uint64_t>(ctx.process, itemBase + 0x18, itemChar) || !itemChar) continue;
            if (!readMem<uint64_t>(ctx.process, itemChar + 0x78, itemData) || !itemData) continue;
            if (!readMem<uint64_t>(ctx.process, itemData + 0x68, rolePtr) || !rolePtr) continue;
            readMem<uint32_t>(ctx.process, rolePtr + 0x20, role);
            std::string roleS = roleName(role);
            uint64_t rb2d = 0, rb2d_cached = 0; float px = 0.f, py = 0.f;
            if (readMem<uint64_t>(ctx.process, itemChar + 0x148, rb2d) && readMem<uint64_t>(ctx.process, rb2d + 0x10, rb2d_cached)) {
                readMem<float>(ctx.process, rb2d_cached + 0xB0, px);
                readMem<float>(ctx.process, rb2d_cached + 0xB4, py);
            }
            uint32_t cid = 0; readMem<uint32_t>(ctx.process, itemBase + 0x48, cid);
            uint64_t namePtr = 0; readMem<uint64_t>(ctx.process, itemBase + 0x30, namePtr);
            std::string name = readUtf16(ctx.process, namePtr, true);
            bool alive = !(roleS == "Dead" || roleS == "Dead (Imp)" || roleS == "Guardian Angel");
            Player p{ name,name,roleS,alive,cid,colorNameFrom(cid),colorHexFrom(cid),px,py };
            if (!p.name.empty()) out.push_back(std::move(p));
        }
    }
    std::sort(out.begin(), out.end(), [](const Player& a, const Player& b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
    return out;
}

int main() {
    HWND targetWindow = FindWindowA("UnityWndClass", 0);
    DWORD pid = 0;
    GetWindowThreadProcessId(targetWindow, &pid);
    Rendering::Initialize(targetWindow, (int)pid);

    HANDLE hProc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    MemContext ctx{};
    ctx.process = hProc;
    ctx.pid = pid;
    ctx.gameAsm = moduleBase(pid, L"GameAssembly.dll");
    ctx.platform = Platform::Unknown;
    ctx.base = 0;

    bool visible = true;
    bool quit = false;
    SHORT prevInsert = 0;

    auto lastRefresh = std::chrono::steady_clock::now();
    std::vector<Player> cache;

    while (!quit) {
        SHORT nowInsert = GetAsyncKeyState(VK_INSERT);
        bool edge = ((nowInsert & 0x8000) && !(prevInsert & 0x8000));
        prevInsert = nowInsert;
        if (edge) visible = !visible;
        if (GetAsyncKeyState(VK_END) & 0x8000) quit = true;

        RECT rc{}; GetClientRect(targetWindow, &rc);
        float w = float(rc.right - rc.left);
        float h = float(rc.bottom - rc.top);
        Rendering::HandleInput();
        Rendering::BeginFrame();

        if (visible) {
            if (ctx.base == 0 || ctx.platform == Platform::Unknown) {
                if (ctx.gameAsm && hProc) resolveBase(ctx);
            }
            auto now = std::chrono::steady_clock::now();
            if (ctx.base && std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRefresh).count() >= 200) {
                cache = readPlayers(ctx);
                lastRefresh = now;
            }

            ImGui::SetNextWindowBgAlpha(0.95f);
            ImGui::Begin("Among Us – Players", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
            const char* pf = (ctx.platform == Platform::Steam ? "Steam" : (ctx.platform == Platform::Epic ? "Epic" : "Unknown"));
            ImGui::Text("Platform: %s | Players: %d", pf, (int)cache.size());
            ImGui::Separator();
            if (ImGui::BeginTable("t", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Role");
                ImGui::TableSetupColumn("Color");
                ImGui::TableSetupColumn("Alive");
                ImGui::TableSetupColumn("Position");
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < cache.size(); ++i) {
                    auto& p = cache[i];
                    ImGui::TableNextRow();

                    bool dead = !p.alive;
                    if (dead) {
                        ImU32 gray = IM_COL32(128, 128, 128, 80);
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, gray);
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, gray);
                        ImGui::BeginDisabled(true);
                    }

                    ImVec4 roleCol;
                    if (dead) roleCol = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
                    else if (isRedRole(p.role)) roleCol = ImVec4(1.0f, 0.25f, 0.25f, 1.0f);
                    else roleCol = ImVec4(1, 1, 1, 1);

                    ImVec4 colCol = hexToColor(p.colorHex);
                    if (dead) colCol.w = 0.5f;

                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%s", p.name.c_str());

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextColored(roleCol, "%s", p.role.c_str());

                    ImGui::TableSetColumnIndex(2);
                    ImGui::PushID((int)i);
                    ImGui::ColorButton("##c", colCol, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha, ImVec2(18, 18));
                    ImGui::SameLine();
                    ImGui::TextColored(colCol, "%s", p.colorName.c_str());
                    ImGui::PopID();

                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%s", p.alive ? "Yes" : "No");

                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("(%.2f, %.2f)", p.x, p.y);

                    if (dead) {
                        ImGui::EndDisabled();
                    }
                }
                ImGui::EndTable();
            }
            ImGui::Separator();
            ImGui::Text("Hotkeys: INSERT show/hide | END exit");
            ImGui::End();
        }

        Rendering::EndFrame(w, h);
    }

    if (hProc) CloseHandle(hProc);
    return 0;
}
