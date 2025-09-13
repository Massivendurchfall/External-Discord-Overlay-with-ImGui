#include "Rendering.h"
#include <windows.h>
#include <tlhelp32.h>
#include <vector>
#include <string>
#include <cstdint>
#include <algorithm>
#include <chrono>

struct Spieler {
    std::string schluessel;
    std::string name;
    std::string rolle;
    bool lebendig;
    uint32_t farbeId;
    std::string farbeName;
    std::string farbeHex;
    float x;
    float y;
};

static const uint32_t steamOffset = 0x0298784C;
static const uint64_t epicOffset = 0x0327E990;

static const char* rollen[11] = {
    "Crewmate","Impostor","Scientist","Engineer","Guardian Angel",
    "Shapeshifter","Dead","Dead (Imp)","Noise Maker","Phantom","Tracker"
};

static const char* farbenName[18] = {
    "Red","Blue","Green","Pink","Orange","Yellow","Black","White","Purple",
    "Brown","Cyan","Lime","Maroon","Rose","Banana","Grey","Tan","Coral"
};

static const char* farbenHex[18] = {
    "#D71E22","#1D3CE9","#1B913E","#FF63D4","#FF8D1C","#FFFF67","#4A565E",
    "#E9F7FF","#783DD2","#80582D","#44FFF7","#5BFE4B","#6C2B3D","#FFD6EC",
    "#FFFFBE","#8397A7","#9F9989","#EC7578"
};

template<typename T>
static bool lese(HANDLE h, uintptr_t adr, T& out) {
    SIZE_T g = 0;
    return ReadProcessMemory(h, reinterpret_cast<LPCVOID>(adr), &out, sizeof(T), &g) && g == sizeof(T);
}

static bool leseBytes(HANDLE h, uintptr_t adr, std::vector<uint8_t>& out, SIZE_T bytes) {
    out.resize(bytes);
    SIZE_T g = 0;
    if (!ReadProcessMemory(h, reinterpret_cast<LPCVOID>(adr), out.data(), bytes, &g)) return false;
    return g == bytes;
}

static uintptr_t modulBasis(DWORD pid, const wchar_t* modul) {
    HANDLE schnapp = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (schnapp == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W me{}; me.dwSize = sizeof(me);
    if (Module32FirstW(schnapp, &me)) {
        do {
            if (lstrcmpiW(me.szModule, modul) == 0) {
                CloseHandle(schnapp);
                return reinterpret_cast<uintptr_t>(me.modBaseAddr);
            }
        } while (Module32NextW(schnapp, &me));
    }
    CloseHandle(schnapp);
    return 0;
}

enum class Plattform { Unbekannt, Steam, Epic };

struct SpeicherKontext {
    HANDLE prozess;
    DWORD pid;
    uintptr_t gameAsm;
    Plattform plattform;
    uintptr_t basis;
};

static Plattform erkennePlattform(const SpeicherKontext& ctx) {
    uint64_t test64a = 0, test64b = 0;
    if (lese<uint64_t>(ctx.prozess, ctx.gameAsm + epicOffset, test64a) && test64a) {
        if (lese<uint64_t>(ctx.prozess, test64a + 0xB8, test64b) && test64b) return Plattform::Epic;
    }
    uint32_t test32a = 0, test32b = 0;
    if (lese<uint32_t>(ctx.prozess, ctx.gameAsm + steamOffset, test32a) && test32a) {
        if (lese<uint32_t>(ctx.prozess, test32a + 0x5C, test32b) && test32b) return Plattform::Steam;
    }
    return Plattform::Unbekannt;
}

static bool ermittleBasis(SpeicherKontext& ctx) {
    ctx.plattform = erkennePlattform(ctx);
    if (ctx.plattform == Plattform::Steam) {
        uint32_t a = 0, b = 0, c = 0;
        if (!lese<uint32_t>(ctx.prozess, ctx.gameAsm + steamOffset, a)) return false;
        if (!lese<uint32_t>(ctx.prozess, a + 0x5C, b)) return false;
        if (!lese<uint32_t>(ctx.prozess, b, c)) return false;
        ctx.basis = c;
        return ctx.basis != 0;
    }
    else if (ctx.plattform == Plattform::Epic) {
        uint64_t a = 0, b = 0, c = 0;
        if (!lese<uint64_t>(ctx.prozess, ctx.gameAsm + epicOffset, a)) return false;
        if (!lese<uint64_t>(ctx.prozess, a + 0xB8, b)) return false;
        if (!lese<uint64_t>(ctx.prozess, b, c)) return false;
        ctx.basis = static_cast<uintptr_t>(c);
        return ctx.basis != 0;
    }
    return false;
}

static std::string rolleName(uint32_t id) {
    if (id < 11) return rollen[id];
    return std::to_string(id);
}

static std::string farbeNameAus(uint32_t id) {
    if (id < 18) return farbenName[id];
    return std::to_string(id);
}

static std::string farbeHexAus(uint32_t id) {
    if (id < 18) return farbenHex[id];
    return std::string("#AAAAAA");
}

static std::string leseUtf16(HANDLE h, uintptr_t strPtr, bool x64) {
    if (!strPtr) return {};
    uint32_t len32 = 0;
    uintptr_t lenAdr = strPtr + (x64 ? 0x10 : 0x8);
    if (!lese<uint32_t>(h, lenAdr, len32)) return {};
    if (len32 == 0 || len32 > 256) return {};
    uintptr_t daten = strPtr + (x64 ? 0x14 : 0xC);
    std::vector<uint8_t> raw;
    if (!leseBytes(h, daten, raw, SIZE_T(len32 * 2))) return {};
    std::wstring ws; ws.resize(len32);
    memcpy(ws.data(), raw.data(), len32 * 2);
    int zielLen = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), len32, nullptr, 0, nullptr, nullptr);
    std::string out; out.resize(zielLen);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), len32, out.data(), zielLen, nullptr, nullptr);
    return out;
}

static ImVec4 hexZuFarbe(const std::string& hex) {
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
    return ImVec4(r / 255.f, g / 255.f, b / 255.f, 1.0f);
}

static bool istRoteRolle(const std::string& r) {
    return r == "Impostor" || r == "Shapeshifter" || r == "Phantom";
}

static std::vector<Spieler> leseSpieler(const SpeicherKontext& ctx) {
    std::vector<Spieler> erg;
    if (ctx.plattform == Plattform::Steam) {
        uint32_t allclients = 0, items = 0, count = 0;
        if (!lese<uint32_t>(ctx.prozess, ctx.basis + 0x38, allclients)) return erg;
        if (!lese<uint32_t>(ctx.prozess, allclients + 0x8, items)) return erg;
        if (!lese<uint32_t>(ctx.prozess, allclients + 0xC, count)) return erg;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t itemBase = 0, itemChar = 0, itemData = 0, rolePtr = 0, role = 0;
            if (!lese<uint32_t>(ctx.prozess, items + 0x10 + i * 4, itemBase)) continue;
            if (!lese<uint32_t>(ctx.prozess, itemBase + 0x10, itemChar)) continue;
            if (!lese<uint32_t>(ctx.prozess, itemChar + 0x58, itemData)) continue;
            if (!lese<uint32_t>(ctx.prozess, itemData + 0x4C, rolePtr)) continue;
            if (!lese<uint32_t>(ctx.prozess, rolePtr + 0x10, role)) continue;
            std::string rolle = rolleName(role);
            uint32_t rb2d = 0, rb2d_cached = 0;
            float x = 0.f, y = 0.f;
            if (lese<uint32_t>(ctx.prozess, itemChar + 0xD0, rb2d) && lese<uint32_t>(ctx.prozess, rb2d + 0x8, rb2d_cached)) {
                lese<float>(ctx.prozess, rb2d_cached + 0x7C, x);
                lese<float>(ctx.prozess, rb2d_cached + 0x80, y);
            }
            uint32_t colorId = 0; lese<uint32_t>(ctx.prozess, itemBase + 0x28, colorId);
            uint32_t namePtr = 0; lese<uint32_t>(ctx.prozess, itemBase + 0x1C, namePtr);
            std::string name = leseUtf16(ctx.prozess, namePtr, false);
            bool alive = !(rolle == "Dead" || rolle == "Dead (Imp)" || rolle == "Guardian Angel");
            Spieler s;
            s.schluessel = name;
            s.name = name;
            s.rolle = rolle;
            s.lebendig = alive;
            s.farbeId = colorId;
            s.farbeName = farbeNameAus(colorId);
            s.farbeHex = farbeHexAus(colorId);
            s.x = x; s.y = y;
            if (!s.name.empty()) erg.push_back(std::move(s));
        }
    }
    else if (ctx.plattform == Plattform::Epic) {
        uint64_t allclients = 0, items = 0; uint32_t count = 0;
        if (!lese<uint64_t>(ctx.prozess, ctx.basis + 0x58, allclients)) return erg;
        if (!lese<uint64_t>(ctx.prozess, allclients + 0x10, items)) return erg;
        if (!lese<uint32_t>(ctx.prozess, allclients + 0x18, count)) return erg;
        for (uint32_t i = 0; i < count; i++) {
            uint64_t itemBase = 0, itemChar = 0, itemData = 0, rolePtr = 0; uint32_t role = 0;
            if (!lese<uint64_t>(ctx.prozess, items + 0x20 + i * 8, itemBase)) continue;
            if (!lese<uint64_t>(ctx.prozess, itemBase + 0x18, itemChar)) continue;
            if (!lese<uint64_t>(ctx.prozess, itemChar + 0x78, itemData)) continue;
            if (!lese<uint64_t>(ctx.prozess, itemData + 0x68, rolePtr)) continue;
            if (!lese<uint32_t>(ctx.prozess, rolePtr + 0x20, role)) continue;
            std::string rolle = rolleName(role);
            uint64_t rb2d = 0, rb2d_cached = 0; float x = 0.f, y = 0.f;
            if (lese<uint64_t>(ctx.prozess, itemChar + 0x148, rb2d) && lese<uint64_t>(ctx.prozess, rb2d + 0x10, rb2d_cached)) {
                lese<float>(ctx.prozess, rb2d_cached + 0xB0, x);
                lese<float>(ctx.prozess, rb2d_cached + 0xB4, y);
            }
            uint32_t colorId = 0; lese<uint32_t>(ctx.prozess, itemBase + 0x48, colorId);
            uint64_t namePtr = 0; lese<uint64_t>(ctx.prozess, itemBase + 0x30, namePtr);
            std::string name = leseUtf16(ctx.prozess, namePtr, true);
            bool alive = !(rolle == "Dead" || rolle == "Dead (Imp)" || rolle == "Guardian Angel");
            Spieler s;
            s.schluessel = name;
            s.name = name;
            s.rolle = rolle;
            s.lebendig = alive;
            s.farbeId = colorId;
            s.farbeName = farbeNameAus(colorId);
            s.farbeHex = farbeHexAus(colorId);
            s.x = x; s.y = y;
            if (!s.name.empty()) erg.push_back(std::move(s));
        }
    }
    std::sort(erg.begin(), erg.end(), [](const Spieler& a, const Spieler& b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
    return erg;
}

int main() {
    HWND zielFenster = FindWindowA("UnityWndClass", 0);
    DWORD prozessId = 0;
    GetWindowThreadProcessId(zielFenster, &prozessId);
    Rendering::Initialize(zielFenster, (int)prozessId);

    HANDLE hProc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, prozessId);
    SpeicherKontext ctx{};
    ctx.prozess = hProc;
    ctx.pid = prozessId;
    ctx.gameAsm = modulBasis(prozessId, L"GameAssembly.dll");
    ctx.plattform = Plattform::Unbekannt;
    ctx.basis = 0;

    bool sichtbar = true;
    bool beenden = false;
    SHORT altInsert = 0;

    auto letzterRefresh = std::chrono::steady_clock::now();
    std::vector<Spieler> spielerCache;

    while (!beenden) {
        SHORT neuInsert = GetAsyncKeyState(VK_INSERT);
        bool flanke = ((neuInsert & 0x8000) && !(altInsert & 0x8000));
        altInsert = neuInsert;
        if (flanke) sichtbar = !sichtbar;
        if (GetAsyncKeyState(VK_END) & 0x8000) beenden = true;

        RECT r{}; GetClientRect(zielFenster, &r);
        float breite = float(r.right - r.left);
        float hoehe = float(r.bottom - r.top);
        Rendering::HandleInput();
        Rendering::BeginFrame();

        if (sichtbar) {
            if (ctx.basis == 0 || ctx.plattform == Plattform::Unbekannt) {
                if (ctx.gameAsm && hProc) ermittleBasis(ctx);
            }
            auto jetzt = std::chrono::steady_clock::now();
            if (ctx.basis && std::chrono::duration_cast<std::chrono::milliseconds>(jetzt - letzterRefresh).count() >= 200) {
                spielerCache = leseSpieler(ctx);
                letzterRefresh = jetzt;
            }

            ImGui::SetNextWindowBgAlpha(0.95f);
            ImGui::Begin("Among Us  Players", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
            const char* plf = (ctx.plattform == Plattform::Steam ? "Steam" : (ctx.plattform == Plattform::Epic ? "Epic" : "Unknown"));
            ImGui::Text("Platform: %s | Players: %d", plf, (int)spielerCache.size());
            ImGui::Separator();
            if (ImGui::BeginTable("t", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Role");
                ImGui::TableSetupColumn("Color");
                ImGui::TableSetupColumn("Alive");
                ImGui::TableSetupColumn("Position");
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < spielerCache.size(); ++i) {
                    auto& s = spielerCache[i];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(s.name.c_str());
                    ImGui::TableSetColumnIndex(1);
                    if (istRoteRolle(s.rolle)) {
                        ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.25f, 1.0f), "%s", s.rolle.c_str());
                    }
                    else {
                        ImGui::TextUnformatted(s.rolle.c_str());
                    }
                    ImGui::TableSetColumnIndex(2);
                    {
                        ImVec4 c = hexZuFarbe(s.farbeHex);
                        ImGui::PushID((int)i);
                        ImGui::ColorButton("##c", c, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha, ImVec2(18, 18));
                        ImGui::SameLine();
                        ImGui::TextColored(c, "%s", s.farbeName.c_str());
                        ImGui::PopID();
                    }
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(s.lebendig ? "Yes" : "No");
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("(%.2f, %.2f)", s.x, s.y);
                }
                ImGui::EndTable();
            }
            ImGui::Separator();
            ImGui::Text("Hotkeys: INSERT show/hide | END exit");
            ImGui::End();
        }

        Rendering::EndFrame(breite, hoehe);
    }

    if (hProc) CloseHandle(hProc);
    return 0;
}


