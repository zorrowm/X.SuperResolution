// Native, statically linked NSIS skin. The UI thread stays responsive while NSIS extracts files.
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "pluginapi.h"
#include "Generated.Theme.h"

using namespace Gdiplus;
namespace fs = std::filesystem;

namespace {
constexpr int Width = 920, Height = 600, Sidebar = 320;
constexpr UINT FinishMessage = WM_APP + 2, CloseMessage = WM_APP + 3;
constexpr int PrimaryId = 100, SecondaryId = 101, BrowseId = 102, DesktopId = 103,
              LaunchId = 104, ThemeId = 105, MinimizeId = 106, CloseId = 107, PathId = 108;
constexpr wchar_t WindowClass[] = L"SuperResolution.Installer.Skin";
HMODULE moduleHandle = nullptr;
HANDLE appMutex = nullptr;
std::wstring registryKey = L"Software\\X.SuperResolution", mutexName = L"X.Lucifer.SuperResolution";

enum class Stage { Welcome, Progress, Complete, Error };
struct Progress { int percent; std::wstring text; };
struct FinishInfo { bool success; std::wstring text; };

// Retain top-down DIBs across paints. Only the final BitBlt touches a visible DC.
class PaintSurface {
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    int width_ = 0, height_ = 0;
public:
    PaintSurface() = default;
    PaintSurface(const PaintSurface&) = delete;
    PaintSurface& operator=(const PaintSurface&) = delete;
    ~PaintSurface() { Reset(); }
    void Reset() {
        if (previous_) SelectObject(dc_, previous_);
        if (bitmap_) DeleteObject(bitmap_);
        if (dc_) DeleteDC(dc_);
        dc_ = nullptr; bitmap_ = nullptr; previous_ = nullptr;
        width_ = height_ = 0;
    }
    bool Ensure(HDC reference, int width, int height) {
        if (dc_ && width_ == width && height_ == height) return true;
        Reset();
        if (width <= 0 || height <= 0) return false;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        dc_ = CreateCompatibleDC(reference);
        bitmap_ = CreateDIBSection(reference, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!dc_ || !bitmap_) { Reset(); return false; }
        previous_ = SelectObject(dc_, bitmap_);
        width_ = width; height_ = height;
        return true;
    }
    HDC Dc() const { return dc_; }
};

struct Ui {
    HWND window = nullptr, pathEdit = nullptr;
    HANDLE thread = nullptr, ready = nullptr, decision = nullptr;
    HFONT editFont = nullptr;
    HBRUSH editBrush = nullptr;
    HICON icon = nullptr;
    ULONG_PTR gdiplus = 0;
    std::wstring version, path, iconPath, detail, error;
    int requiredMb = 0, percent = 0, result = 0, hover = 0;
    float scale = 1;
    bool dark = true, uninstall = false, desktop = true, launch = true, reducedMotion = false;
    bool preview = false;
    bool backgroundDirty = true, pathFocused = false;
    PaintSurface background, frame, buttonFrame;
    SRWLOCK progressLock = SRWLOCK_INIT;
    Progress pendingProgress{};
    bool progressPending = false;
    Stage stage = Stage::Welcome;
    ULONGLONG started = 0;
    std::vector<HWND> controls;
    const unsigned int* Colors() const { return dark ? DarkColors : LightColors; }
    Color C(int i, BYTE alpha = 255) const { const auto v = Colors()[i]; return Color(alpha, static_cast<BYTE>(v >> 16), static_cast<BYTE>(v >> 8), static_cast<BYTE>(v)); }
    int Px(float value) const { return static_cast<int>(std::lround(value * scale)); }
};
std::unique_ptr<Ui> ui;

std::wstring Pop(stack_t** stack) {
    if (!stack || !*stack) return {};
    auto entry = *stack;
    std::wstring result(entry->text);
    *stack = entry->next;
    GlobalFree(entry);
    return result;
}
void Push(stack_t** stack, int size, const std::wstring& value) {
    auto entry = static_cast<stack_t*>(GlobalAlloc(GPTR, sizeof(stack_t) + static_cast<SIZE_T>(size) * sizeof(wchar_t)));
    if (!entry) return;
    lstrcpynW(entry->text, value.c_str(), size);
    entry->next = *stack;
    *stack = entry;
}

std::wstring Canonical(const std::wstring& path) {
    wchar_t buffer[32768];
    DWORD count = GetFullPathNameW(path.c_str(), ARRAYSIZE(buffer), buffer, nullptr);
    if (!count || count >= ARRAYSIZE(buffer)) return {};
    std::wstring result(buffer);
    while (result.size() > 3 && result.back() == L'\\') result.pop_back();
    return result;
}
bool SamePath(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
bool IsWithin(const std::wstring& child, const std::wstring& parent) {
    return SamePath(child, parent) || (child.size() > parent.size() && child[parent.size()] == L'\\' && _wcsnicmp(child.c_str(), parent.c_str(), parent.size()) == 0);
}
std::wstring ValidatePath(const std::wstring& candidate, bool uninstall, int requiredMb) {
    if (candidate.size() < 4 || candidate[1] != L':' || candidate[2] != L'\\' || candidate.find_first_of(L"\"<>|?*\r\n", 3) != std::wstring::npos || candidate.find(L':', 2) != std::wstring::npos)
        return L"请选择本机磁盘上的完整安装路径。";
    const auto path = Canonical(candidate);
    if (path.size() <= 3 || path.size() > 180) return L"请选择一个独立文件夹，路径长度需小于 180 个字符。";
    wchar_t windows[MAX_PATH];
    GetWindowsDirectoryW(windows, MAX_PATH);
    if (IsWithin(path, windows)) return L"请勿安装到 Windows 系统目录。";
    std::error_code ec;
    for (fs::path part(path); !part.empty(); part = part.parent_path()) {
        const DWORD attributes = GetFileAttributesW(part.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return L"安装路径不能包含符号链接或目录联接。";
        if (part == part.root_path()) break;
    }
    wchar_t legacyDirectory[MAX_PATH] = {};
    DWORD legacyBytes = sizeof(legacyDirectory);
    const bool legacyRegistered = RegGetValueW(HKEY_CURRENT_USER, registryKey.c_str(), L"InstallDir", RRF_RT_REG_SZ, nullptr, legacyDirectory, &legacyBytes) == ERROR_SUCCESS
        && SamePath(Canonical(legacyDirectory), path)
        && fs::is_regular_file(fs::path(path) / L"X.SuperResolution.exe", ec)
        && fs::is_regular_file(fs::path(path) / L"Uninstall.exe", ec);
    const bool owned = fs::is_regular_file(fs::path(path) / L"install-manifest.txt", ec) || legacyRegistered;
    if (uninstall && !owned) return L"未找到安装文件清单，无法安全卸载。请先重新安装。";
    if (!uninstall) {
        wchar_t installed[MAX_PATH] = {};
        DWORD bytes = sizeof(installed);
        if (RegGetValueW(HKEY_CURRENT_USER, registryKey.c_str(), L"InstallDir", RRF_RT_REG_SZ, nullptr, installed, &bytes) == ERROR_SUCCESS && *installed && !SamePath(Canonical(installed), path))
            return L"已安装的版本位于其他目录。如需更换位置，请先卸载旧版本。";
        if (fs::exists(path, ec) && !fs::is_directory(path, ec)) return L"该路径已被文件占用，请选择文件夹。";
        if (!owned && fs::is_directory(path, ec) && !fs::is_empty(path, ec)) return L"请选择空文件夹，或已有的 SuperResolution 安装目录。";
        fs::path existing(path);
        while (!existing.empty() && !fs::exists(existing, ec)) existing = existing.parent_path();
        ULARGE_INTEGER available{};
        if (existing.empty() || !GetDiskFreeSpaceExW(existing.c_str(), &available, nullptr, nullptr)) return L"无法读取目标磁盘，请检查路径与访问权限。";
        if (available.QuadPart < static_cast<ULONGLONG>(requiredMb) * 1024 * 1024) return L"目标磁盘空间不足，请选择其他安装位置。";
    }
    return {};
}

std::vector<std::wstring> ReadManifest(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(file)), {});
    if (bytes.compare(0, 3, "\xef\xbb\xbf") == 0) bytes.erase(0, 3);
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring contents(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), contents.data(), length);
    std::vector<std::wstring> result;
    size_t offset = 0;
    while (offset < contents.size()) {
        auto end = contents.find(L'\n', offset);
        if (end == std::wstring::npos) end = contents.size();
        auto line = contents.substr(offset, end - offset);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (!line.empty()) result.push_back(line);
        offset = end + 1;
    }
    return result;
}

void RoundedPath(GraphicsPath& path, RectF rect, float radius) {
    const float d = radius * 2;
    path.AddArc(rect.X, rect.Y, d, d, 180, 90);
    path.AddArc(rect.GetRight() - d, rect.Y, d, d, 270, 90);
    path.AddArc(rect.GetRight() - d, rect.GetBottom() - d, d, d, 0, 90);
    path.AddArc(rect.X, rect.GetBottom() - d, d, d, 90, 90);
    path.CloseFigure();
}
void Box(Graphics& g, RectF rect, Color fill, Color border = Color(0, 0, 0, 0), float radius = 9) {
    GraphicsPath shape;
    RoundedPath(shape, rect, radius);
    SolidBrush brush(fill);
    g.FillPath(&brush, &shape);
    if (border.GetA()) { Pen pen(border, 1); g.DrawPath(&pen, &shape); }
}
void Text(Graphics& g, const std::wstring& value, float x, float y, float w, float h, float size, Color color, bool bold = false, StringAlignment align = StringAlignmentNear, bool mono = false) {
    FontFamily family(mono ? L"Consolas" : L"Microsoft YaHei UI");
    Font font(&family, size, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
    StringFormat format;
    format.SetAlignment(align);
    format.SetLineAlignment(StringAlignmentCenter);
    format.SetTrimming(StringTrimmingEllipsisCharacter);
    SolidBrush brush(color);
    g.DrawString(value.c_str(), -1, &font, RectF(x, y, w, h), &format, &brush);
}
void Line(Graphics& g, Color color, float x, float y, float x2, float y2, float width = 1) {
    Pen pen(color, width);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawLine(&pen, x, y, x2, y2);
}
void Circle(Graphics& g, float x, float y, float radius, Color fill, Color edge = Color(0, 0, 0, 0)) {
    SolidBrush brush(fill);
    g.FillEllipse(&brush, x - radius, y - radius, radius * 2, radius * 2);
    if (edge.GetA()) { Pen pen(edge, 1); g.DrawEllipse(&pen, x - radius, y - radius, radius * 2, radius * 2); }
}
void Check(Graphics& g, float x, float y, Color color, float size = 1) {
    Line(g, color, x, y + 4 * size, x + 4 * size, y + 8 * size, 1.7f * size);
    Line(g, color, x + 4 * size, y + 8 * size, x + 11 * size, y, 1.7f * size);
}

RectF ControlRect(int id) {
    switch (id) {
    case PrimaryId: return {708, 524, 168, 44};
    case SecondaryId: return {528, 524, 168, 44};
    case BrowseId: return {788, 314, 80, 40};
    case DesktopId: return {364, 381, 224, 28};
    case LaunchId: return {364, 405, 330, 28};
    case ThemeId: return {816, 20, 28, 28};
    case MinimizeId: return {65, 20, 28, 28};
    case CloseId: return {29, 20, 28, 28};
    default: return {};
    }
}

void PaintControl(Ui& state, Graphics& g, int id, bool hot, bool pressed, bool focus, bool enabled) {
    RectF r = ControlRect(id);
    const auto text = state.C(21), muted = state.C(22), accent = state.C(11);
    if (id == CloseId || id == MinimizeId) {
        Color fill = id == CloseId ? Color(255, 237, 120, 109) : Color(255, 224, 183, 95);
        if (!enabled) fill = state.C(5);
        Circle(g, r.X + 14, r.Y + 14, 8, fill);
        if (hot || focus) {
            const Color ink = id == CloseId ? Color(255, 93, 45, 43) : Color(255, 85, 65, 25);
            Line(g, ink, r.X + 11, r.Y + 14, r.X + 17, r.Y + 14, 1.4f);
            if (id == CloseId) {
                Circle(g, r.X + 14, r.Y + 14, 8, fill);
                Line(g, ink, r.X + 11, r.Y + 11, r.X + 17, r.Y + 17, 1.4f);
                Line(g, ink, r.X + 17, r.Y + 11, r.X + 11, r.Y + 17, 1.4f);
            }
        }
    } else if (id == ThemeId) {
        if (hot || focus) Box(g, r, state.C(3), state.C(5), 7);
        Pen pen(muted, 1.4f);
        if (state.dark) {
            g.DrawArc(&pen, r.X + 7, r.Y + 7, 14.0f, 14.0f, 30.0f, 275.0f);
            g.DrawArc(&pen, r.X + 12, r.Y + 4, 13.0f, 13.0f, 68.0f, 178.0f);
        } else {
            g.DrawEllipse(&pen, r.X + 10, r.Y + 10, 8.0f, 8.0f);
            for (int i = 0; i < 8; ++i) {
                const float a = i * 3.14159265f / 4;
                Line(g, muted, r.X + 14 + std::cos(a) * 7, r.Y + 14 + std::sin(a) * 7, r.X + 14 + std::cos(a) * 10, r.Y + 14 + std::sin(a) * 10, 1.3f);
            }
        }
    } else if (id == DesktopId || id == LaunchId) {
        const bool checked = id == DesktopId ? state.desktop : state.launch;
        Box(g, {r.X, r.Y + 6, 16, 16}, checked ? state.C(7) : state.C(2), hot ? accent : state.C(6), 4);
        if (checked) Check(g, r.X + 3, r.Y + 10, state.C(12), .85f);
        Text(g, id == DesktopId ? L"创建桌面快捷方式" : L"完成后启动 SuperResolution", r.X + 27, r.Y, r.Width - 27, r.Height, 12, text);
    } else {
        const bool primary = id == PrimaryId;
        Color fill = primary ? state.C(pressed ? 8 : (hot ? 8 : 7)) : state.C(hot ? 4 : 3);
        if (!enabled) fill = state.C(3);
        Box(g, r, fill, primary ? fill : state.C(5));
        std::wstring caption;
        if (id == BrowseId) caption = L"浏览…";
        if (id == SecondaryId) caption = state.stage == Stage::Complete ? L"打开安装目录" : L"取消";
        if (primary) {
            switch (state.stage) {
            case Stage::Welcome: caption = state.uninstall ? L"确认卸载" : L"开始安装"; break;
            case Stage::Progress: caption = state.uninstall ? L"正在卸载…" : L"正在安装…"; break;
            case Stage::Complete: caption = state.uninstall ? L"完成" : (state.launch ? L"开始使用" : L"完成"); break;
            case Stage::Error: caption = L"关闭"; break;
            }
        }
        Text(g, caption, r.X, r.Y, r.Width, r.Height, 13, enabled ? (primary ? state.C(12) : text) : muted, primary, StringAlignmentCenter);
    }
    if (focus) {
        Pen pen(accent, 1);
        pen.SetDashStyle(DashStyleDot);
        g.DrawRectangle(&pen, r.X + 3, r.Y + 3, r.Width - 6, r.Height - 6);
    }
}

void DrawArtwork(Ui& state, Graphics& g) {
    // A quiet routing diagram echoes the app's connection dashboard and X monogram.
    const auto edge = state.C(10, 150);
    for (int x = 42; x <= 280; x += 22)
        for (int y = 174; y <= 372; y += 22) Circle(g, static_cast<float>(x), static_cast<float>(y), .7f, state.C(6, 65));
    Pen orbit(state.C(10, 90), 1);
    g.DrawEllipse(&orbit, 69.0f, 187.0f, 182.0f, 182.0f);
    g.DrawEllipse(&orbit, 92.0f, 210.0f, 136.0f, 136.0f);
    Line(g, edge, 54, 215, 160, 278);
    Line(g, edge, 160, 278, 266, 226);
    Line(g, edge, 160, 278, 266, 340);
    Line(g, edge, 54, 340, 160, 278);
    const PointF nodes[] = {{54,215},{266,226},{266,340},{54,340}};
    for (const auto& p : nodes) {
        Box(g, {p.X - 13, p.Y - 13, 26, 26}, state.C(1), state.C(10), 7);
        Circle(g, p.X, p.Y, 3, state.C(7));
    }
    Box(g, {113, 231, 94, 94}, state.C(9), state.C(10), 24);
    Box(g, {121, 239, 78, 78}, state.C(9), state.C(10, 120), 18);
    Text(g, L"X", 121, 240, 78, 74, 47, state.C(11), true, StringAlignmentCenter);
    Box(g, {98, 371, 124, 25}, state.C(9), state.C(10), 12);
    Circle(g, 113, 383.5f, 3, state.C(7));
    Text(g, L"IMAGE · ENHANCE", 124, 371, 91, 25, 10, state.C(11), false, StringAlignmentNear, true);
}

void DrawMotion(Ui& state, Graphics& g) {
    if (state.reducedMotion) return;
    const float t = static_cast<float>((GetTickCount64() - state.started) % 2400) / 2400;
    Circle(g, 54 + 106 * t, 215 + 63 * t, 3, state.C(11));
    Circle(g, 160 + 106 * t, 278 - 52 * t, 3, state.C(11));
}

void DrawProgressValues(Ui& state, Graphics& g) {
    Text(g, std::to_wstring(state.percent), 361, 264, 126, 81, 64, state.C(11), true, StringAlignmentFar, true);
    Text(g, L"%", 497, 302, 48, 32, 20, state.C(22), false, StringAlignmentNear, true);
    Box(g, {364, 363, 512, 7}, state.C(3), Color(0, 0, 0, 0), 3.5f);
    if (state.percent > 0) Box(g, {364, 363, std::max(7.0f, 512 * state.percent / 100.0f), 7}, state.C(7), Color(0, 0, 0, 0), 3.5f);
    Text(g, state.detail.empty() ? L"正在准备文件…" : state.detail, 364, 384, 512, 31, 12, state.C(22));
}

void Render(Ui& state, Graphics& g, bool includeControls, bool includeDynamic = true) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.ScaleTransform(state.scale, state.scale);
    SolidBrush canvas(state.C(0)), sidebar(state.C(1));
    g.FillRectangle(&canvas, 0, 0, Width, Height);
    g.FillRectangle(&sidebar, 0, 0, Sidebar, Height);
    Line(g, state.C(5), 320, 0, 320, 600);
    Line(g, state.C(5), 320, 64, 920, 64);
    Line(g, state.C(5), 320, 500, 920, 500);
    Text(g, state.uninstall ? L"SuperResolution  /  卸载" : L"SuperResolution  /  安装", 364, 18, 330, 30, 12, state.C(22));
    Text(g, L"SuperResolution", 36, 81, 248, 48, 23, state.C(21), true);
    Text(g, L"让每一处细节，都清晰可见。", 38, 132, 245, 26, 12, state.C(22));
    DrawArtwork(state, g);
    if (includeDynamic && state.stage == Stage::Progress) DrawMotion(state, g);
    Text(g, L"唤醒细节，重现清晰。", 38, 430, 250, 30, 17, state.C(21), true);
    Text(g, L"本地增强  ·  批量处理  ·  GPU 加速", 38, 468, 254, 23, 11, state.C(22));
    Text(g, L"WINDOWS  /  X64", 38, 541, 225, 22, 10, state.C(22), false, StringAlignmentNear, true);

    const int active = state.stage == Stage::Welcome ? 0 : state.stage == Stage::Progress ? 1 : 2;
    const wchar_t* steps[] = {L"准备", state.uninstall ? L"卸载" : L"安装", L"完成"};
    for (int i = 0; i < 3; ++i) {
        const float x = 365.0f + i * 174;
        const bool failed = state.stage == Stage::Error;
        const bool highlighted = !failed && i <= active;
        Circle(g, x + 10, 103, 10, failed && i == active ? state.C(19) : (highlighted ? state.C(9) : state.C(3)), failed && i == active ? state.C(20) : (highlighted ? state.C(10) : state.C(5)));
        if (!failed && i < active) Check(g, x + 5, 99, state.C(11), .85f);
        else Text(g, failed && i == active ? L"!" : std::to_wstring(i + 1), x, 92, 20, 21, 10, failed && i == active ? state.C(18) : (highlighted ? state.C(11) : state.C(22)), true, StringAlignmentCenter, true);
        Text(g, failed && i == active ? L"未完成" : steps[i], x + 30, 91, 58, 24, 11, highlighted ? state.C(21) : state.C(22));
        if (i < 2) Line(g, state.C(5), x + 84, 103, x + 154, 103);
    }

    if (state.stage == Stage::Welcome) {
        Text(g, state.uninstall ? L"暂时告别，也能随时回来。" : L"让图片，呈现更多细节。", 364, 155, 520, 45, 27, state.C(21), true);
        Text(g, state.uninstall ? L"移除应用，保留你的图片、处理结果与个性化设置。" : L"在本机完成图片放大与降噪，准备好你的图像工作台。", 364, 212, 520, 27, 12, state.C(22));
        Text(g, state.uninstall ? L"当前安装位置" : L"安装位置", 364, 272, 260, 24, 12, state.C(21), true);
        if (state.uninstall) {
            Box(g, {364, 310, 512, 52}, state.C(2), state.C(5));
            Text(g, state.path, 380, 317, 480, 38, 12, state.C(22));
            Box(g, {364, 383, 512, 64}, state.C(9), state.C(10));
            Check(g, 381, 409, state.C(11));
            Text(g, L"原图、output 与 settings.json 会保留", 406, 395, 450, 38, 12, state.C(11));
        } else {
            Box(g, {364, 310, 512, 48}, state.C(2), state.pathFocused ? state.C(11) : state.C(5));
            if (includeControls) Text(g, state.path, 378, 313, 394, 41, 12, state.C(21));
            Text(g, L"仅为当前用户安装 · 所需空间约 " + std::to_wstring(state.requiredMb) + L" MB", 364, 427, 512, 25, 11, state.C(22));
        }
        if (!state.error.empty()) Text(g, state.error, 364, 461, 512, 28, 11, state.C(18));
    } else if (state.stage == Stage::Progress) {
        Text(g, state.uninstall ? L"正在移除应用。" : L"正在为你准备图像工作台。", 364, 155, 520, 45, 27, state.C(21), true);
        Text(g, state.uninstall ? L"你的个人配置将保留，稍后即可完成。" : L"正在安装应用与模型，即将开始本地图片增强。", 364, 212, 520, 27, 12, state.C(22));
        if (includeDynamic) DrawProgressValues(state, g);
        Text(g, L"完成后将自动进入下一步", 364, 433, 512, 26, 11, state.C(22));
    } else {
        const bool success = state.stage == Stage::Complete;
        Circle(g, 390, 178, 25, state.C(success ? 9 : 19), state.C(success ? 10 : 20));
        if (success) Check(g, 379, 170, state.C(11), 1.85f);
        else Text(g, L"!", 365, 151, 50, 50, 29, state.C(18), true, StringAlignmentCenter);
        Text(g, success ? (state.uninstall ? L"已卸载，期待再见。" : L"一切就绪，即刻增强。") : L"还差一步，未能完成。", 364, 228, 512, 47, 27, state.C(21), true);
        Text(g, success ? (state.uninstall ? L"SuperResolution 已移除。再次安装时，你的配置依然在。" : L"SuperResolution 已安装，可以开始增强图片。") : state.detail, 364, 286, 512, 63, 12, state.C(success ? 22 : 18));
        if (success && !state.uninstall) {
            Box(g, {364, 362, 512, 30}, state.C(9), Color(0,0,0,0), 6);
            Text(g, state.path, 376, 362, 488, 30, 11, state.C(11));
        }
    }
    Text(g, L"v" + state.version, 364, 530, 158, 18, 10, state.C(22), false, StringAlignmentNear, true);
    Text(g, L"X Lucifer", 364, 548, 140, 18, 10, state.C(22));
    if (includeControls) {
        for (int id : {CloseId, MinimizeId, ThemeId, PrimaryId}) PaintControl(state, g, id, false, false, false, id != PrimaryId || state.stage != Stage::Progress);
        if (state.stage == Stage::Welcome || (state.stage == Stage::Complete && !state.uninstall)) PaintControl(state, g, SecondaryId, false, false, false, true);
        if (state.stage == Stage::Welcome && !state.uninstall) {
            PaintControl(state, g, BrowseId, false, false, false, true);
            PaintControl(state, g, DesktopId, false, false, false, true);
        }
        if (state.stage == Stage::Complete && !state.uninstall) PaintControl(state, g, LaunchId, false, false, false, true);
    }
}

void InvalidateArea(Ui& state, int left, int top, int right, int bottom) {
    RECT rect{state.Px(static_cast<float>(left)), state.Px(static_cast<float>(top)), state.Px(static_cast<float>(right)), state.Px(static_cast<float>(bottom))};
    InvalidateRect(state.window, &rect, FALSE);
}

bool EnsureBackground(Ui& state, HDC dc) {
    if (!state.background.Ensure(dc, state.Px(Width), state.Px(Height))) return false;
    if (state.backgroundDirty) {
        Graphics g(state.background.Dc());
        Render(state, g, false, false);
        g.Flush(FlushIntentionSync);
        state.backgroundDirty = false;
    }
    return true;
}

void PaintWindow(Ui& state, HDC dc, const RECT& dirty) {
    if (!EnsureBackground(state, dc) || !state.frame.Ensure(dc, state.Px(Width), state.Px(Height))) return;
    const int width = dirty.right - dirty.left, height = dirty.bottom - dirty.top;
    BitBlt(state.frame.Dc(), dirty.left, dirty.top, width, height, state.background.Dc(), dirty.left, dirty.top, SRCCOPY);
    if (state.stage == Stage::Progress) {
        Graphics g(state.frame.Dc());
        g.SetClip(Rect(dirty.left, dirty.top, width, height));
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.ScaleTransform(state.scale, state.scale);
        if (dirty.left < state.Px(290) && dirty.top < state.Px(400) && dirty.bottom > state.Px(175)) DrawMotion(state, g);
        if (dirty.right > state.Px(361) && dirty.top < state.Px(416) && dirty.bottom > state.Px(264)) DrawProgressValues(state, g);
        g.Flush(FlushIntentionSync);
    }
    BitBlt(dc, dirty.left, dirty.top, width, height, state.frame.Dc(), dirty.left, dirty.top, SRCCOPY);
}

void QueueProgress(Ui& state, int percent, std::wstring detail) {
    AcquireSRWLockExclusive(&state.progressLock);
    state.pendingProgress = {std::clamp(percent, 0, 100), std::move(detail)};
    state.progressPending = true;
    ReleaseSRWLockExclusive(&state.progressLock);
}

void Refresh(Ui& state) {
    if (state.editBrush) DeleteObject(state.editBrush);
    auto color = state.C(2);
    state.editBrush = CreateSolidBrush(RGB(color.GetR(), color.GetG(), color.GetB()));
    BOOL dark = state.dark;
    DwmSetWindowAttribute(state.window, 20, &dark, sizeof(dark));
    state.backgroundDirty = true;
    RedrawWindow(state.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}
void Layout(Ui& state) {
    state.background.Reset();
    state.frame.Reset();
    state.backgroundDirty = true;
    for (HWND control : state.controls) {
        auto r = ControlRect(GetDlgCtrlID(control));
        SetWindowPos(control, nullptr, state.Px(r.X), state.Px(r.Y), state.Px(r.Width), state.Px(r.Height), SWP_NOZORDER | SWP_NOACTIVATE);
    }
    SetWindowPos(state.pathEdit, nullptr, state.Px(378), state.Px(325), state.Px(394), state.Px(22), SWP_NOZORDER | SWP_NOACTIVATE);
    if (state.editFont) DeleteObject(state.editFont);
    state.editFont = CreateFontW(-state.Px(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    SendMessageW(state.pathEdit, WM_SETFONT, reinterpret_cast<WPARAM>(state.editFont), TRUE);
    // Rounded clipping also works on Windows 10; the window itself has no nonclient frame.
    SetWindowRgn(state.window, CreateRoundRectRgn(0, 0, state.Px(Width) + 1, state.Px(Height) + 1, state.Px(22), state.Px(22)), TRUE);
}
void SetStage(Ui& state, Stage stage) {
    state.stage = stage;
    state.backgroundDirty = true;
    if (stage == Stage::Progress) SetTimer(state.window, 1, 40, nullptr);
    else KillTimer(state.window, 1);
    ShowWindow(state.pathEdit, stage == Stage::Welcome && !state.uninstall ? SW_SHOW : SW_HIDE);
    for (HWND control : state.controls) {
        const int id = GetDlgCtrlID(control);
        bool visible = true;
        if (id == SecondaryId) visible = stage == Stage::Welcome || (stage == Stage::Complete && !state.uninstall);
        if (id == BrowseId || id == DesktopId) visible = stage == Stage::Welcome && !state.uninstall;
        if (id == LaunchId) visible = stage == Stage::Complete && !state.uninstall;
        ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
        EnableWindow(control, !(stage == Stage::Progress && (id == CloseId || id == PrimaryId)));
    }
    SetWindowTextW(GetDlgItem(state.window, PrimaryId), stage == Stage::Welcome ? (state.uninstall ? L"确认卸载" : L"开始安装") : stage == Stage::Complete ? L"完成" : stage == Stage::Error ? L"关闭" : L"正在安装");
    SetWindowTextW(GetDlgItem(state.window, SecondaryId), stage == Stage::Complete ? L"打开安装目录" : L"取消");
    SetFocus(GetDlgItem(state.window, PrimaryId));
    RedrawWindow(state.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

LRESULT CALLBACK ControlProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR reference) {
    auto& state = *reinterpret_cast<Ui*>(reference);
    const int id = GetDlgCtrlID(window);
    // Standard buttons otherwise erase using the system background before WM_DRAWITEM.
    if (message == WM_ERASEBKGND && id != PathId) return TRUE;
    if (message == WM_MOUSEMOVE) {
        if (id != PathId && state.hover != id) {
            if (state.hover) InvalidateRect(GetDlgItem(state.window, state.hover), nullptr, FALSE);
            state.hover = id;
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
            TrackMouseEvent(&tracking);
            InvalidateRect(window, nullptr, FALSE);
        }
    } else if (message == WM_MOUSELEAVE) {
        if (state.hover == id) {
            state.hover = 0;
            InvalidateRect(window, nullptr, FALSE);
        }
    } else if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
        if (id == PathId) {
            state.pathFocused = message == WM_SETFOCUS;
            state.backgroundDirty = true;
            InvalidateArea(state, 364, 310, 876, 358);
        }
    } else if (message == WM_SETCURSOR && GetDlgCtrlID(window) != PathId) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

void Browse(Ui& state) {
    IFileDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    dialog->SetTitle(L"选择安装位置（会使用 X.SuperResolution 子文件夹）");
    if (SUCCEEDED(dialog->Show(state.window))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                fs::path selected(path);
                if (_wcsicmp(selected.filename().c_str(), L"X.SuperResolution") != 0) selected /= L"X.SuperResolution";
                SetWindowTextW(state.pathEdit, selected.c_str());
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
}

void Command(Ui& state, int id) {
    if (id == MinimizeId) { ShowWindow(state.window, SW_MINIMIZE); return; }
    if (id == ThemeId) {
        state.dark = !state.dark;
        SetWindowTextW(GetDlgItem(state.window, ThemeId), state.dark ? L"当前石墨主题，切换到雾白" : L"当前雾白主题，切换到石墨");
        Refresh(state);
        return;
    }
    if (id == BrowseId) { Browse(state); return; }
    if (id == DesktopId || id == LaunchId) {
        (id == DesktopId ? state.desktop : state.launch) ^= true;
        std::wstring label = id == DesktopId ? L"创建桌面快捷方式" : L"完成后启动 SuperResolution";
        label += (id == DesktopId ? state.desktop : state.launch) ? L"，已勾选" : L"，未勾选";
        SetWindowTextW(GetDlgItem(state.window, id), label.c_str());
        InvalidateRect(GetDlgItem(state.window, id), nullptr, FALSE);
        if (id == LaunchId) InvalidateRect(GetDlgItem(state.window, PrimaryId), nullptr, FALSE);
        return;
    }
    if (id == SecondaryId && state.stage == Stage::Complete) {
        ShellExecuteW(state.window, L"open", state.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (id == PrimaryId) {
        if (state.stage == Stage::Progress) return;
        if (state.stage == Stage::Welcome) {
            if (!state.uninstall) {
                wchar_t path[1024];
                GetWindowTextW(state.pathEdit, path, ARRAYSIZE(path));
                state.path = path;
            }
            state.error = state.preview ? L"" : ValidatePath(state.path, state.uninstall, state.requiredMb);
            if (!state.error.empty()) { Refresh(state); SetFocus(state.pathEdit); return; }
            state.path = Canonical(state.path);
            state.result = 1;
            state.started = GetTickCount64();
            SetStage(state, Stage::Progress);
            SetEvent(state.decision);
            return;
        }
        state.result = state.stage == Stage::Complete && !state.uninstall && state.launch ? 2 : 1;
    } else if (id == CloseId || id == SecondaryId || id == IDCANCEL) {
        if (state.stage == Stage::Progress) return;
        state.result = 0;
    } else return;
    SetEvent(state.decision);
    ShowWindow(state.window, SW_HIDE);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<Ui*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<Ui*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_ERASEBKGND: return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        PaintWindow(*state, dc, paint.rcPaint);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_PRINTCLIENT: {
        RECT rect{0, 0, state->Px(Width), state->Px(Height)};
        PaintWindow(*state, reinterpret_cast<HDC>(wparam), rect);
        return 0;
    }
    case WM_DRAWITEM: {
        auto item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
        const int width = item->rcItem.right - item->rcItem.left, height = item->rcItem.bottom - item->rcItem.top;
        if (!state->buttonFrame.Ensure(item->hDC, width, height)) return TRUE;
        auto rect = ControlRect(static_cast<int>(item->CtlID));
        {
            Graphics g(state->buttonFrame.Dc());
            const auto backgroundColor = state->C(rect.X < Sidebar ? 1 : (item->CtlID == BrowseId ? 2 : 0));
            g.Clear(backgroundColor);
            g.TranslateTransform(-static_cast<float>(state->Px(rect.X)), -static_cast<float>(state->Px(rect.Y)));
            g.ScaleTransform(state->scale, state->scale);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            const bool showFocus = (SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) == 0;
            PaintControl(*state, g, static_cast<int>(item->CtlID), state->hover == static_cast<int>(item->CtlID), (item->itemState & ODS_SELECTED) != 0, showFocus && (item->itemState & ODS_FOCUS) != 0, IsWindowEnabled(item->hwndItem) != FALSE);
            g.Flush(FlushIntentionSync);
        }
        BitBlt(item->hDC, item->rcItem.left, item->rcItem.top, width, height, state->buttonFrame.Dc(), 0, 0, SRCCOPY);
        return TRUE;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        if (reinterpret_cast<HWND>(lparam) != state->pathEdit) break;
        auto color = state->C(21), background = state->C(2);
        SetTextColor(reinterpret_cast<HDC>(wparam), RGB(color.GetR(), color.GetG(), color.GetB()));
        SetBkColor(reinterpret_cast<HDC>(wparam), RGB(background.GetR(), background.GetG(), background.GetB()));
        return reinterpret_cast<LRESULT>(state->editBrush);
    }
    case WM_COMMAND:
        if (HIWORD(wparam) == BN_CLICKED) Command(*state, LOWORD(wparam));
        return 0;
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(window, &point);
        if (point.y < state->Px(64) && ChildWindowFromPointEx(window, point, CWP_SKIPINVISIBLE) == window) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_DPICHANGED: {
        state->scale = HIWORD(wparam) / 96.0f;
        auto suggested = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, state->Px(Width), state->Px(Height), SWP_NOZORDER | SWP_NOACTIVATE);
        Layout(*state);
        Refresh(*state);
        return 0;
    }
    case WM_TIMER: {
        if (wparam != 1 || state->stage != Stage::Progress) return 0;
        AcquireSRWLockExclusive(&state->progressLock);
        const bool changed = state->progressPending;
        if (changed) {
            state->percent = state->pendingProgress.percent;
            state->detail = std::move(state->pendingProgress.text);
            state->progressPending = false;
        }
        ReleaseSRWLockExclusive(&state->progressLock);
        if (!IsIconic(window)) {
            if (changed) InvalidateArea(*state, 361, 264, 878, 416);
            if (!state->reducedMotion) InvalidateArea(*state, 30, 175, 290, 400);
        }
        return 0;
    }
    case FinishMessage: {
        std::unique_ptr<FinishInfo> finish(reinterpret_cast<FinishInfo*>(lparam));
        state->detail = finish->text;
        state->percent = 100;
        SetStage(*state, finish->success ? Stage::Complete : Stage::Error);
        ShowWindow(window, SW_RESTORE);
        SetForegroundWindow(window);
        return 0;
    }
    case WM_CLOSE: Command(*state, CloseId); return 0;
    case WM_QUERYENDSESSION: return state->stage != Stage::Progress;
    case CloseMessage: DestroyWindow(window); return 0;
    case WM_DESTROY: SetEvent(state->decision); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

DWORD WINAPI UiThread(void* parameter) {
    auto& state = *static_cast<Ui*>(parameter);
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    GdiplusStartupInput startup;
    if (GdiplusStartup(&state.gdiplus, &startup, nullptr) != Ok) { SetEvent(state.ready); SetEvent(state.decision); return 1; }
    ANIMATIONINFO animation{sizeof(animation)};
    SystemParametersInfoW(SPI_GETANIMATION, sizeof(animation), &animation, 0);
    state.reducedMotion = !animation.iMinAnimate;
    state.icon = static_cast<HICON>(LoadImageW(nullptr, state.iconPath.c_str(), IMAGE_ICON, 48, 48, LR_LOADFROMFILE));
    WNDCLASSEXW cls{sizeof(cls)};
    cls.hInstance = moduleHandle;
    cls.lpszClassName = WindowClass;
    cls.lpfnWndProc = WindowProc;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = state.icon;
    cls.style = CS_DROPSHADOW;
    RegisterClassExW(&cls);
    POINT mouse;
    GetCursorPos(&mouse);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(mouse, MONITOR_DEFAULTTONEAREST), &monitor);
    state.scale = GetDpiForSystem() / 96.0f;
    // Fit small work areas without clipping controls; real DPI changes are handled above.
    state.scale = std::max(.75f, std::min(state.scale, std::min((monitor.rcWork.right - monitor.rcWork.left - 32) / static_cast<float>(Width), (monitor.rcWork.bottom - monitor.rcWork.top - 32) / static_cast<float>(Height))));
    HWND window = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_CONTROLPARENT, WindowClass, state.uninstall ? L"SuperResolution 卸载" : L"SuperResolution 安装", WS_POPUP | WS_CLIPCHILDREN | WS_MINIMIZEBOX | WS_SYSMENU,
        monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - state.Px(Width)) / 2,
        monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - state.Px(Height)) / 2,
        state.Px(Width), state.Px(Height), nullptr, nullptr, moduleHandle, &state);
    if (window) {
        const std::pair<int, const wchar_t*> controls[] = {{PrimaryId,L"开始安装"},{SecondaryId,L"取消"},{BrowseId,L"浏览安装目录"},{DesktopId,L"创建桌面快捷方式，已勾选"},{LaunchId,L"完成后启动 SuperResolution，已勾选"},{ThemeId,L"当前石墨主题，切换到雾白"},{MinimizeId,L"最小化"},{CloseId,L"关闭"}};
        for (auto [id, name] : controls) {
            HWND control = CreateWindowExW(0, L"BUTTON", name, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 1, 1, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), moduleHandle, nullptr);
            SetWindowSubclass(control, ControlProc, 1, reinterpret_cast<DWORD_PTR>(&state));
            state.controls.push_back(control);
        }
        state.pathEdit = CreateWindowExW(0, L"EDIT", state.path.c_str(), WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 1, 1, window, reinterpret_cast<HMENU>(PathId), moduleHandle, nullptr);
        SendMessageW(state.pathEdit, EM_SETLIMITTEXT, 180, 0);
        SetWindowSubclass(state.pathEdit, ControlProc, 1, reinterpret_cast<DWORD_PTR>(&state));
        Layout(state);
        SendMessageW(window, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
        Refresh(state);
        SetStage(state, Stage::Welcome);
        HDC initialDc = GetDC(window);
        EnsureBackground(state, initialDc);
        ReleaseDC(window, initialDc);
        ShowWindow(window, SW_SHOWNORMAL);
        UpdateWindow(window);
        SetForegroundWindow(window);
    }
    SetEvent(state.ready);
    if (window) {
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) { Command(state, IDCANCEL); continue; }
            if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN) {
                HWND focused = GetFocus();
                Command(state, focused && GetDlgCtrlID(focused) != PathId ? GetDlgCtrlID(focused) : PrimaryId);
                continue;
            }
            if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
    }
    state.window = nullptr;
    SetEvent(state.decision);
    if (state.editFont) DeleteObject(state.editFont);
    if (state.editBrush) DeleteObject(state.editBrush);
    UnregisterClassW(WindowClass, moduleHandle);
    if (state.icon) DestroyIcon(state.icon);
    GdiplusShutdown(state.gdiplus);
    if (SUCCEEDED(com)) CoUninitialize();
    return 0;
}

void StopUi() {
    if (ui) {
        if (ui->window) PostMessageW(ui->window, CloseMessage, 0, 0);
        if (ui->thread) { WaitForSingleObject(ui->thread, INFINITE); CloseHandle(ui->thread); }
        if (ui->ready) CloseHandle(ui->ready);
        if (ui->decision) CloseHandle(ui->decision);
        ui.reset();
    }
    if (appMutex) { ReleaseMutex(appMutex); CloseHandle(appMutex); appMutex = nullptr; }
}
UINT_PTR __cdecl PluginCallback(NSPIM message) {
    if (message == NSPIM_UNLOAD) StopUi();
    return 0;
}
void StartUi() {
    ui->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ui->decision = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ui->thread = CreateThread(nullptr, 0, UiThread, ui.get(), 0, nullptr);
    if (ui->thread) WaitForSingleObject(ui->ready, INFINITE);
}
} // namespace

#define PLUGIN_ARGS HWND, int stringSize, wchar_t*, stack_t** stack, extra_parameters* extra

extern "C" void __cdecl Configure(PLUGIN_ARGS) {
    (void)stringSize;
    extra->RegisterPluginCallback(moduleHandle, PluginCallback);
    registryKey = Pop(stack);
    mutexName = Pop(stack);
}
extern "C" void __cdecl Show(PLUGIN_ARGS) {
    extra->RegisterPluginCallback(moduleHandle, PluginCallback);
    StopUi();
    ui = std::make_unique<Ui>();
    ui->version = Pop(stack);
    ui->path = Pop(stack);
    ui->iconPath = Pop(stack);
    ui->uninstall = Pop(stack) == L"uninstall";
    ui->requiredMb = _wtoi(Pop(stack).c_str());
    StartUi();
    if (ui->thread && ui->window) WaitForSingleObject(ui->decision, INFINITE);
    Push(stack, stringSize, ui->desktop ? L"1" : L"0");
    Push(stack, stringSize, ui->path);
    Push(stack, stringSize, ui->window && ui->result == 1 ? L"install" : L"cancel");
}
extern "C" void __cdecl Update(PLUGIN_ARGS) {
    (void)stringSize; (void)extra;
    const auto percent = Pop(stack), detail = Pop(stack);
    if (ui && ui->window) {
        // A bounded latest-value slot avoids blocking extraction on GDI or flooding the UI queue.
        QueueProgress(*ui, _wtoi(percent.c_str()), detail);
    }
}
extern "C" void __cdecl Finish(PLUGIN_ARGS) {
    (void)extra;
    const auto success = Pop(stack), text = Pop(stack);
    if (!ui || !ui->window) { Push(stack, stringSize, L"0"); return; }
    ResetEvent(ui->decision);
    SendMessageW(ui->window, FinishMessage, 0, reinterpret_cast<LPARAM>(new FinishInfo{success == L"success", text}));
    WaitForSingleObject(ui->decision, INFINITE);
    Push(stack, stringSize, ui->result == 2 ? L"1" : L"0");
}
extern "C" void __cdecl Close(PLUGIN_ARGS) {
    (void)stringSize; (void)stack; (void)extra;
    StopUi();
}
extern "C" void __cdecl Validate(PLUGIN_ARGS) {
    extra->RegisterPluginCallback(moduleHandle, PluginCallback);
    const auto path = Pop(stack), mode = Pop(stack), size = Pop(stack);
    auto error = ValidatePath(path, mode == L"uninstall", _wtoi(size.c_str()));
    if (error.empty() && !appMutex) {
        appMutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
        DWORD status = appMutex ? WaitForSingleObject(appMutex, 0) : WAIT_FAILED;
        if (status != WAIT_OBJECT_0 && status != WAIT_ABANDONED) {
            if (appMutex) CloseHandle(appMutex);
            appMutex = nullptr;
            error = L"请先退出 SuperResolution，再重新运行安装程序。正在进行的安装或卸载也需先完成。";
        }
    }
    Push(stack, stringSize, error);
}
extern "C" void __cdecl Prune(PLUGIN_ARGS) {
    (void)extra;
    const auto root = Pop(stack), newManifest = Pop(stack);
    const auto oldFiles = ReadManifest(fs::path(root) / L"install-manifest.txt");
    const auto newFiles = ReadManifest(newManifest);
    std::set<std::wstring> current(newFiles.begin(), newFiles.end());
    std::wstring error;
    for (const auto& relative : oldFiles) {
        fs::path path(relative);
        if (path.is_absolute() || relative.find(L':') != std::wstring::npos || relative.find(L"..") != std::wstring::npos) {
            error = L"旧安装文件清单无效，无法继续更新。"; break;
        }
        if (current.count(relative)) continue;
        const auto target = fs::path(root) / path;
        std::error_code ec;
        for (auto parent = target.parent_path(); IsWithin(parent.wstring(), root) && parent.wstring().size() >= root.size(); parent = parent.parent_path()) {
            const auto attributes = GetFileAttributesW(parent.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) { error = L"旧安装目录包含目录联接，无法继续更新。"; break; }
            if (SamePath(parent.wstring(), root)) break;
        }
        if (!error.empty()) break;
        if (fs::is_regular_file(target, ec) && !DeleteFileW(target.c_str())) { error = L"无法清理旧版本文件，请确认 SuperResolution 已退出且目录可写。"; break; }
    }
    Push(stack, stringSize, error);
}

#ifdef XRAY_SKIN_PREVIEW
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    moduleHandle = instance;
    int count = 0;
    auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (count >= 3 && std::wstring(args[1]) == L"--render") {
        GdiplusStartupInput startup;
        ULONG_PTR token;
        GdiplusStartup(&token, &startup, nullptr);
        CLSID encoder;
        CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &encoder);
        fs::create_directories(args[2]);
        {
            Ui state;
            state.version = L"1.0.9753.26000";
            state.path = L"C:\\Users\\Lucifer\\AppData\\Local\\Programs\\X.SuperResolution";
            state.requiredMb = 186;
            state.percent = 64;
            state.detail = L"正在准备应用文件 · Avalonia.Controls.dll";
            const std::pair<Stage, const wchar_t*> stages[] = {{Stage::Welcome,L"welcome"},{Stage::Progress,L"progress"},{Stage::Complete,L"complete"},{Stage::Error,L"error"}};
            for (bool dark : {true, false}) for (const auto& [stage, name] : stages) for (float scale : {1.0f, 1.5f}) {
                state.dark = dark; state.stage = stage; state.scale = scale;
                state.detail = stage == Stage::Error ? L"请先退出 SuperResolution，再重新运行安装程序。" : L"正在准备应用文件 · Avalonia.Controls.dll";
                Bitmap bitmap(state.Px(Width), state.Px(Height), PixelFormat32bppARGB);
                Graphics g(&bitmap);
                Render(state, g, true);
                auto path = fs::path(args[2]) / ((dark ? std::wstring(L"dark-") : std::wstring(L"light-")) + name + (scale == 1 ? L".png" : L"-150.png"));
                bitmap.Save(path.c_str(), &encoder);
            }
            state.uninstall = true; state.stage = Stage::Welcome; state.scale = 1;
            for (bool dark : {true, false}) {
                state.dark = dark;
                Bitmap bitmap(Width, Height, PixelFormat32bppARGB);
                Graphics g(&bitmap);
                Render(state, g, true);
                bitmap.Save((fs::path(args[2]) / (dark ? L"dark-uninstall.png" : L"light-uninstall.png")).c_str(), &encoder);
            }
        }
        GdiplusShutdown(token);
        LocalFree(args);
        return 0;
    }
    (void)commandLine;
    ui = std::make_unique<Ui>();
    ui->version = L"1.0.preview";
    ui->path = L"C:\\Users\\Lucifer\\AppData\\Local\\Programs\\X.SuperResolution";
    ui->requiredMb = 186;
    ui->preview = true;
    StartUi();
    if (ui->window) {
        WaitForSingleObject(ui->decision, INFINITE);
        if (ui->result == 1) {
            for (int i = 0; i <= 100; ++i) {
                QueueProgress(*ui, i, L"正在准备应用文件 · Avalonia.Controls.dll");
                Sleep(60);
            }
            ResetEvent(ui->decision);
            SendMessageW(ui->window, FinishMessage, 0, reinterpret_cast<LPARAM>(new FinishInfo{true,L""}));
            WaitForSingleObject(ui->decision, INFINITE);
        }
    }
    StopUi();
    LocalFree(args);
    return 0;
}
#else
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { moduleHandle = instance; DisableThreadLibraryCalls(instance); }
    return TRUE;
}
#endif
