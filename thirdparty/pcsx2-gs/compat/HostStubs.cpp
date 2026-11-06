// Host/VM functions PCSX2's GS calls, provided by the port instead of the rest of PCSX2 (docs/GS_PCSX2.md, phase 1).
// Most are empty: no OSD, no dumps, no video capture, no texture replacement yet. PCSX2 is GPL-3; so is this file.
#include "pcsx2gs_host.h"

#include "Config.h"
#include "Counters.h"
#include "GS.h"
#include "GS/GS.h"
#include "GS/GSCapture.h"
#include "GS/GSDump.h"
#include "GS/GSPng.h"
#include "GS/Renderers/HW/GSTextureReplacements.h"
#include "BuildVersion.h"
#include "GSDumpReplayer.h"
#include "Host.h"
#include "ImGui/FullscreenUI.h"
#include "ImGui/ImGuiManager.h"
#include "Memory.h"
#include "MemoryTypes.h"
#include "MTGS.h"
#include "PerformanceMetrics.h"
#include "R5900.h"
#include "VMManager.h"
#include "common/CrashHandler.h"
#include "common/Image.h"
#include "common/StringUtil.h"
#include "imgui.h"
#include "imgui_freetype.h"

#include <algorithm>
#include <cfloat>
#include <cstdarg>
#include <cmath>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <sys/mman.h>
#endif

namespace
{
    WindowInfo g_window;
    double g_verticalFrequency = 59.94;

    // On-screen text (set_osd), drawn by ImGuiManager::RenderOSD on the GS thread.
    std::mutex g_osdMutex;
    std::string g_osdTop;
    std::string g_osdBottom;
    uint32_t g_osdTopRgba = 0;
    float g_osdTopAlpha = 1.0f;
    ImFont *g_osdFont = nullptr;

    std::string g_textureSerial; // SetTextureFolder: the pack folder's name

    // In-game menu (set_menu / menu_input): drawn by RenderOSD on the GS thread, input applied in NewFrame.
    std::mutex g_menuMutex;
    Pcsx2GsMenuFn g_menuDraw = nullptr;
    void *g_menuUser = nullptr;
    Pcsx2GsMenuInput g_menuInput{};
    float g_menuWheel = 0.0f;
    std::vector<uint32_t> g_menuChars; // typed characters not yet given to ImGui (UTF-32)

    int UiBeginWindow(const char *title, int *open, float width, float height)
    {
        // Text and window follow the window's size (sized for ~900 lines; never larger than the screen).
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const float scale = std::clamp(display.y / 900.0f, 0.7f, 1.8f);
        ImGui::GetStyle().FontScaleMain = scale;
        width = std::min(width * scale, display.x - 24.0f);
        height = std::min(height * scale, display.y - 24.0f);
        // Centered at the first open and after the game window is resized; otherwise where and as large as the player
        // left it (ImGui keeps that for the session).
        static ImVec2 lastDisplay(0.0f, 0.0f);
        const ImGuiCond when = (display.x != lastDisplay.x || display.y != lastDisplay.y) ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
        lastDisplay = display;
        ImGui::SetNextWindowSize(ImVec2(width, height), when);
        ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), when, ImVec2(0.5f, 0.5f));
        bool keep = open ? *open != 0 : true;
        const bool visible = ImGui::Begin(title, open ? &keep : nullptr, ImGuiWindowFlags_NoCollapse);
        if (open)
            *open = keep ? 1 : 0;
        return visible ? 1 : 0;
    }
    void UiEndWindow() { ImGui::End(); }
    int UiBeginTabBar(const char *id) { return ImGui::BeginTabBar(id) ? 1 : 0; }
    void UiEndTabBar() { ImGui::EndTabBar(); }
    int UiBeginTab(const char *label) { return ImGui::BeginTabItem(label) ? 1 : 0; }
    void UiEndTab() { ImGui::EndTabItem(); }
    void UiText(const char *text) { ImGui::TextUnformatted(text ? text : ""); }
    void UiTextWrapped(const char *text) { ImGui::TextWrapped("%s", text ? text : ""); }
    void UiTextDisabled(const char *text) { ImGui::TextDisabled("%s", text ? text : ""); }
    void UiTextColored(uint32_t rgba, const char *text)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(rgba));
        ImGui::TextUnformatted(text ? text : "");
        ImGui::PopStyleColor();
    }
    void UiHeading(const char *text)
    {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.35f);
        ImGui::TextUnformatted(text ? text : "");
        ImGui::PopFont();
    }
    int UiCheckbox(const char *label, int *value)
    {
        bool checked = value && *value != 0;
        const bool changed = ImGui::Checkbox(label, &checked);
        if (value)
            *value = checked ? 1 : 0;
        return changed ? 1 : 0;
    }
    int UiButton(const char *label) { return ImGui::Button(label) ? 1 : 0; }
    void UiSameLine() { ImGui::SameLine(); }
    void UiSeparator() { ImGui::Separator(); }
    void UiSpacing() { ImGui::Spacing(); }
    int UiBeginChild(const char *id, float width, float height)
    {
        return ImGui::BeginChild(id, ImVec2(width, height), ImGuiChildFlags_Borders) ? 1 : 0;
    }
    void UiEndChild() { ImGui::EndChild(); }
    void UiPushId(int id) { ImGui::PushID(id); }
    void UiPopId() { ImGui::PopID(); }
    int UiCombo(const char *label, int *current, const char *const *items, int count)
    {
        int value = current ? *current : 0;
        const bool changed = ImGui::Combo(label, &value, items, count);
        if (current)
            *current = value;
        return changed ? 1 : 0;
    }
    int UiRadio(const char *label, int active) { return ImGui::RadioButton(label, active != 0) ? 1 : 0; }
    int UiSmallButton(const char *label) { return ImGui::SmallButton(label) ? 1 : 0; }
    void UiSeparatorText(const char *text) { ImGui::SeparatorText(text ? text : ""); }
    void UiIndent() { ImGui::Indent(); }
    void UiUnindent() { ImGui::Unindent(); }
    void UiSetNextWidth(float width)
    {
        if (width > 0.0f)
            ImGui::SetNextItemWidth(width * ImGui::GetStyle().FontScaleMain);
    }
    void UiBeginDisabled(int disabled) { ImGui::BeginDisabled(disabled != 0); }
    void UiEndDisabled() { ImGui::EndDisabled(); }

    // Version 3: sizes in set_next_width units (scaled with the text).
    float UiScale() { return ImGui::GetStyle().FontScaleMain; }
    ImVec2 UiScaled(float width, float height) { return ImVec2(width * UiScale(), height * UiScale()); }
    int UiInputInt(const char *label, int *value, int step)
    {
        int current = value ? *value : 0;
        const bool changed = ImGui::InputInt(label, &current, step > 0 ? step : 0, step > 0 ? step * 10 : 0);
        if (value)
            *value = current;
        return changed ? 1 : 0;
    }
    int UiInputText(const char *label, const char *hint, char *buffer, uint32_t size)
    {
        if (!buffer || size == 0u)
            return 0;
        const bool changed = hint && *hint ? ImGui::InputTextWithHint(label, hint, buffer, size) : ImGui::InputText(label, buffer, size);
        return changed ? 1 : 0;
    }
    int UiSliderInt(const char *label, int *value, int min, int max)
    {
        int current = value ? *value : 0;
        const bool changed = ImGui::SliderInt(label, &current, min, max);
        if (value)
            *value = current;
        return changed ? 1 : 0;
    }
    int UiSelectable(const char *label, int selected, uint32_t flags)
    {
        ImGuiSelectableFlags select = ImGuiSelectableFlags_None;
        if (flags & PCSX2GS_SELECT_SPAN_COLUMNS)
            select |= ImGuiSelectableFlags_SpanAllColumns;
        if (flags & PCSX2GS_SELECT_ALLOW_OVERLAP)
            select |= ImGuiSelectableFlags_AllowOverlap;
        if (flags & PCSX2GS_SELECT_DOUBLE_CLICK)
            select |= ImGuiSelectableFlags_AllowDoubleClick;
        return ImGui::Selectable(label ? label : "", selected != 0, select) ? 1 : 0;
    }
    int UiBeginTable(const char *id, int columns, uint32_t flags, float width, float height)
    {
        if (columns < 1 || columns > 64)
            return 0;
        ImGuiTableFlags table = ImGuiTableFlags_SizingFixedFit;
        if (flags & PCSX2GS_TABLE_SORTABLE)
            table |= ImGuiTableFlags_Sortable;
        if (flags & PCSX2GS_TABLE_SCROLL)
            table |= ImGuiTableFlags_ScrollY;
        if (flags & PCSX2GS_TABLE_SCROLL_X)
            table |= ImGuiTableFlags_ScrollX;
        if (flags & PCSX2GS_TABLE_BORDERS)
            table |= ImGuiTableFlags_Borders;
        if (flags & PCSX2GS_TABLE_ROW_BG)
            table |= ImGuiTableFlags_RowBg;
        if (flags & PCSX2GS_TABLE_RESIZABLE)
            table |= ImGuiTableFlags_Resizable;
        return ImGui::BeginTable(id, columns, table, UiScaled(width, height)) ? 1 : 0;
    }
    void UiEndTable() { ImGui::EndTable(); }
    void UiTableColumn(const char *label, uint32_t flags, float width)
    {
        ImGuiTableColumnFlags column = width > 0.0f ? ImGuiTableColumnFlags_WidthFixed : ImGuiTableColumnFlags_WidthStretch;
        if (flags & PCSX2GS_COLUMN_DEFAULT_SORT)
            column |= ImGuiTableColumnFlags_DefaultSort;
        if (flags & PCSX2GS_COLUMN_NO_SORT)
            column |= ImGuiTableColumnFlags_NoSort;
        if (flags & PCSX2GS_COLUMN_DESCENDING_FIRST)
            column |= ImGuiTableColumnFlags_PreferSortDescending;
        ImGui::TableSetupColumn(label ? label : "", column, width > 0.0f ? width * UiScale() : 0.0f);
    }
    void UiTableFreeze(int columns, int rows) { ImGui::TableSetupScrollFreeze(columns, rows); }
    void UiTableHeaders() { ImGui::TableHeadersRow(); }
    void UiTableNextRow() { ImGui::TableNextRow(); }
    int UiTableNextColumn() { return ImGui::TableNextColumn() ? 1 : 0; }
    int UiTableSort(int *column, int *descending)
    {
        ImGuiTableSortSpecs *specs = ImGui::TableGetSortSpecs();
        int sortColumn = -1, down = 0, changed = 0;
        if (specs)
        {
            if (specs->SpecsCount > 0)
            {
                sortColumn = specs->Specs[0].ColumnIndex;
                down = specs->Specs[0].SortDirection == ImGuiSortDirection_Descending ? 1 : 0;
            }
            if (specs->SpecsDirty)
            {
                changed = 1;
                specs->SpecsDirty = false;
            }
        }
        if (column)
            *column = sortColumn;
        if (descending)
            *descending = down;
        return changed;
    }
    void UiTableRowColor(uint32_t rgba)
    {
        if (rgba != 0u)
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, rgba);
    }
    void UiTooltip(const char *text)
    {
        if (text && *text && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", text);
    }
    int UiItemHovered() { return ImGui::IsItemHovered() ? 1 : 0; }
    int UiItemDoubleClicked() { return ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ? 1 : 0; }
    int UiCollapsingHeader(const char *label, int openFirst)
    {
        return ImGui::CollapsingHeader(label ? label : "", openFirst ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None) ? 1 : 0;
    }
    void UiProgressBar(float fraction, uint32_t rgba, float width, float height, const char *overlay)
    {
        if (rgba != 0u)
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImGui::ColorConvertU32ToFloat4(rgba));
        ImGui::ProgressBar(std::clamp(fraction, 0.0f, 1.0f),
                           ImVec2(width > 0.0f ? width * UiScale() : -FLT_MIN, height > 0.0f ? height * UiScale() : 0.0f), overlay);
        if (rgba != 0u)
            ImGui::PopStyleColor();
    }
    uint32_t UiKeyMods()
    {
        const ImGuiIO &io = ImGui::GetIO();
        return (io.KeyShift ? PCSX2GS_KEY_SHIFT : 0u) | (io.KeyCtrl ? PCSX2GS_KEY_CTRL : 0u);
    }
    void UiAvail(float *width, float *height)
    {
        const ImVec2 room = ImGui::GetContentRegionAvail();
        if (width)
            *width = room.x / UiScale();
        if (height)
            *height = room.y / UiScale();
    }
    void UiSameLineAt(float x) { ImGui::SameLine(x * UiScale()); }
    int UiButtonColored(const char *label, uint32_t rgba, float width)
    {
        if (rgba != 0u)
        {
            const ImVec4 color = ImGui::ColorConvertU32ToFloat4(rgba);
            ImGui::PushStyleColor(ImGuiCol_Button, color);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(std::min(color.x + 0.15f, 1.0f), std::min(color.y + 0.15f, 1.0f),
                                                                 std::min(color.z + 0.15f, 1.0f), color.w));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
        }
        const bool clicked = ImGui::Button(label ? label : "", ImVec2(width > 0.0f ? width * UiScale() : 0.0f, 0.0f));
        if (rgba != 0u)
            ImGui::PopStyleColor(3);
        return clicked ? 1 : 0;
    }
    int UiBeginPanel(const char *id, float width, float height, int border)
    {
        return ImGui::BeginChild(id, UiScaled(width, height), border ? ImGuiChildFlags_Borders : ImGuiChildFlags_None) ? 1 : 0;
    }
    ImGuiListClipper g_clipper; // one list at a time (GS thread)
    void UiClipperBegin(int count) { g_clipper.Begin(std::max(count, 0)); }
    int UiClipperStep(int *first, int *end)
    {
        if (g_clipper.Step())
        {
            if (first)
                *first = g_clipper.DisplayStart;
            if (end)
                *end = g_clipper.DisplayEnd;
            return 1;
        }
        g_clipper.End();
        return 0;
    }

    const Pcsx2GsUi g_ui = {3u, UiBeginWindow, UiEndWindow, UiBeginTabBar, UiEndTabBar, UiBeginTab, UiEndTab, UiText,
                            UiTextWrapped, UiTextDisabled, UiTextColored, UiHeading, UiCheckbox, UiButton, UiSameLine,
                            UiSeparator, UiSpacing, UiBeginChild, UiEndChild, UiPushId, UiPopId, UiCombo, UiRadio,
                            UiSmallButton, UiSeparatorText, UiIndent, UiUnindent, UiSetNextWidth, UiBeginDisabled,
                            UiEndDisabled, UiInputInt, UiInputText, UiSliderInt, UiSelectable, UiBeginTable, UiEndTable,
                            UiTableColumn, UiTableFreeze, UiTableHeaders, UiTableNextRow, UiTableNextColumn, UiTableSort,
                            UiTableRowColor, UiTooltip, UiItemHovered, UiItemDoubleClicked, UiCollapsingHeader, UiProgressBar,
                            UiKeyMods, UiAvail, UiSameLineAt, UiButtonColored, UiBeginPanel, UiClipperBegin,
                            UiClipperStep};

    // The menu's look: ImGui's dark style with the launcher's red.
    void ApplyMenuStyle()
    {
        ImGui::StyleColorsDark();
        ImGuiStyle &style = ImGui::GetStyle();
        style.WindowRounding = 4.0f;
        style.FrameRounding = 3.0f;
        style.TabRounding = 3.0f;
        style.WindowPadding = ImVec2(16.0f, 14.0f);
        style.ItemSpacing = ImVec2(10.0f, 8.0f);
        ImVec4 *colors = style.Colors;
        const ImVec4 red(0.79f, 0.12f, 0.09f, 1.0f), redHot(0.89f, 0.20f, 0.16f, 1.0f), redDark(0.45f, 0.07f, 0.05f, 1.0f);
        colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.07f, 0.08f, 0.94f);
        colors[ImGuiCol_TitleBg] = colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.12f, 0.06f, 0.05f, 1.0f);
        colors[ImGuiCol_TitleBgActive] = redDark;
        colors[ImGuiCol_Tab] = ImVec4(0.18f, 0.10f, 0.09f, 1.0f);
        colors[ImGuiCol_TabHovered] = redHot;
        colors[ImGuiCol_TabSelected] = red;
        colors[ImGuiCol_Button] = ImVec4(0.30f, 0.31f, 0.33f, 1.0f);
        colors[ImGuiCol_ButtonHovered] = redHot;
        colors[ImGuiCol_ButtonActive] = red;
        colors[ImGuiCol_CheckMark] = redHot;
        colors[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.14f, 0.16f, 1.0f);
        colors[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.12f, 0.11f, 1.0f);
        colors[ImGuiCol_Header] = ImVec4(0.30f, 0.09f, 0.07f, 1.0f);
        colors[ImGuiCol_HeaderHovered] = redHot;
        colors[ImGuiCol_NavCursor] = ImVec4(0.91f, 0.75f, 0.16f, 1.0f);
    }

    // The latest menu input as ImGui events (ImGui drops the ones that repeat the current state).
    void ApplyMenuInput()
    {
        Pcsx2GsMenuInput input;
        float wheel;
        bool active;
        std::vector<uint32_t> chars;
        {
            std::lock_guard<std::mutex> lock(g_menuMutex);
            input = g_menuInput;
            wheel = g_menuWheel;
            g_menuWheel = 0.0f;
            active = g_menuDraw != nullptr;
            chars.swap(g_menuChars);
        }
        ImGuiIO &io = ImGui::GetIO();
        if (!active)
            input = Pcsx2GsMenuInput{-FLT_MAX, -FLT_MAX};
        io.AddMousePosEvent(input.mouse_x, input.mouse_y);
        for (int button = 0; button < 3; ++button)
            io.AddMouseButtonEvent(button, (input.mouse_buttons >> button) & 1u);
        if (wheel != 0.0f)
            io.AddMouseWheelEvent(0.0f, wheel);
        const auto key = [&](ImGuiKey imguiKey, uint32_t bits, uint32_t bit) { io.AddKeyEvent(imguiKey, (bits & bit) != 0u); };
        key(ImGuiKey_UpArrow, input.keys, PCSX2GS_KEY_UP);
        key(ImGuiKey_DownArrow, input.keys, PCSX2GS_KEY_DOWN);
        key(ImGuiKey_LeftArrow, input.keys, PCSX2GS_KEY_LEFT);
        key(ImGuiKey_RightArrow, input.keys, PCSX2GS_KEY_RIGHT);
        key(ImGuiKey_Enter, input.keys, PCSX2GS_KEY_ENTER);
        key(ImGuiKey_Space, input.keys, PCSX2GS_KEY_SPACE);
        key(ImGuiKey_Escape, input.keys, PCSX2GS_KEY_ESCAPE);
        key(ImGuiKey_Tab, input.keys, PCSX2GS_KEY_TAB);
        key(ImGuiMod_Shift, input.keys, PCSX2GS_KEY_SHIFT);
        key(ImGuiMod_Ctrl, input.keys, PCSX2GS_KEY_CTRL);
        key(ImGuiKey_Backspace, input.keys, PCSX2GS_KEY_BACKSPACE);
        key(ImGuiKey_Delete, input.keys, PCSX2GS_KEY_DELETE);
        key(ImGuiKey_Home, input.keys, PCSX2GS_KEY_HOME);
        key(ImGuiKey_End, input.keys, PCSX2GS_KEY_END);
        key(ImGuiKey_PageUp, input.keys, PCSX2GS_KEY_PAGE_UP);
        key(ImGuiKey_PageDown, input.keys, PCSX2GS_KEY_PAGE_DOWN);
        key(ImGuiKey_A, input.keys, PCSX2GS_KEY_A);
        key(ImGuiKey_C, input.keys, PCSX2GS_KEY_C);
        key(ImGuiKey_V, input.keys, PCSX2GS_KEY_V);
        key(ImGuiKey_X, input.keys, PCSX2GS_KEY_X);
        key(ImGuiKey_Z, input.keys, PCSX2GS_KEY_Z);
        key(ImGuiKey_Y, input.keys, PCSX2GS_KEY_Y);
        key(ImGuiKey_GamepadDpadUp, input.pad, PCSX2GS_PAD_UP);
        key(ImGuiKey_GamepadDpadDown, input.pad, PCSX2GS_PAD_DOWN);
        key(ImGuiKey_GamepadDpadLeft, input.pad, PCSX2GS_PAD_LEFT);
        key(ImGuiKey_GamepadDpadRight, input.pad, PCSX2GS_PAD_RIGHT);
        key(ImGuiKey_GamepadFaceDown, input.pad, PCSX2GS_PAD_ACCEPT);
        key(ImGuiKey_GamepadFaceRight, input.pad, PCSX2GS_PAD_BACK);
        key(ImGuiKey_GamepadL1, input.pad, PCSX2GS_PAD_L1);
        key(ImGuiKey_GamepadR1, input.pad, PCSX2GS_PAD_R1);
        if (active)
            for (const uint32_t c : chars)
                if (c >= 0x20u && c != 0x7Fu)
                    io.AddInputCharacter(c);
    }
}

void pcsx2gs::SetMenu(Pcsx2GsMenuFn draw, void *user)
{
    std::lock_guard<std::mutex> lock(g_menuMutex);
    g_menuDraw = draw;
    g_menuUser = user;
}

void pcsx2gs::MenuInput(const Pcsx2GsMenuInput *input)
{
    if (!input)
        return;
    std::lock_guard<std::mutex> lock(g_menuMutex);
    g_menuWheel += input->wheel;
    g_menuInput = *input;
    for (uint32_t i = 0; i < std::min<uint32_t>(input->char_count, 16u) && g_menuChars.size() < 256u; ++i)
        g_menuChars.push_back(input->chars[i]);
}

void pcsx2gs::SetOsd(const char *top, uint32_t topRgba, float topAlpha, const char *bottom)
{
    std::lock_guard<std::mutex> lock(g_osdMutex);
    g_osdTop = top ? top : "";
    g_osdBottom = bottom ? bottom : "";
    g_osdTopRgba = topRgba;
    g_osdTopAlpha = std::clamp(topAlpha, 0.0f, 1.0f);
}

void pcsx2gs::SetRenderWindow(const WindowInfo &window)
{
    g_window = window;
}

void pcsx2gs::SetFolders(const std::string &resources, const std::string &cache)
{
    EmuFolders::Resources = resources;
    EmuFolders::Cache = cache;
}

void pcsx2gs::SetTextureFolder(const std::string &folder)
{
    std::filesystem::path path(folder);
    while (!path.empty() && !path.has_filename())
        path = path.parent_path();
    if (path.filename() == "replacements")
        path = path.parent_path();
    EmuFolders::Textures = path.parent_path().string();
    g_textureSerial = path.filename().string();
}

void pcsx2gs::SetVerticalFrequency(double hz)
{
    g_verticalFrequency = hz;
}

// --- configuration and emulator state the GS reads ---
// BuildVersion.cpp of PCSX2 comes from its git checkout (svnrev.h); the Vulkan device reports this version to the
// driver. The PCSX2 commit thirdparty/pcsx2 was copied from (thirdparty/README.md).
namespace BuildVersion
{
    const char *GitTag = "2954ebe";
    bool GitTaggedCommit = false;
    int GitTagHi = 0;
    int GitTagMid = 0;
    int GitTagLo = 0;
    const char *GitRev = "2954ebe";
    const char *GitHash = "2954ebe6965669219f1bbd256e958fbd8f83a85d";
    const char *GitDate = "2026-10-11";
}

Pcsx2Config EmuConfig;
bool FMVstarted = false;
alignas(16) cpuRegistersPack _cpuRegistersPack;
alignas(__pagealignsize) u8 eeHw[Ps2MemSize::Hardware];

namespace EmuFolders
{
    std::string Cache;
    std::string Logs;
    std::string Resources;
    std::string Snapshots;
    std::string Textures;
    std::string Videos;
}

Pcsx2Config::Pcsx2Config()
{
    bitset = 0;
}

Pcsx2Config::PadOptions::PadOptions()
{
    bitset = 0;
}

double GetVerticalFrequency()
{
    return g_verticalFrequency;
}

const char *ReportVideoMode()
{
    return "NTSC";
}

const char *ReportInterlaceMode()
{
    return "Progressive";
}

// The software renderer's JIT code lives in PCSX2's code reservation; here only that part (64 MB) exists.
u8 *SysMemory::GetCodePtr(size_t offset)
{
#if defined(_WIN32)
    static u8 *base = static_cast<u8 *>(
        VirtualAlloc(nullptr, HostMemoryMap::SWrecSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
#else
    static u8 *base = []
    {
        void *memory = mmap(nullptr, HostMemoryMap::SWrecSize, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        return memory == MAP_FAILED ? nullptr : static_cast<u8 *>(memory);
    }();
#endif
    if (!base || offset < HostMemoryMap::SWrecOffset || offset > HostMemoryMap::SWrecOffset + HostMemoryMap::SWrecSize)
        return nullptr;
    return base + (offset - HostMemoryMap::SWrecOffset);
}

// --- Host ---
std::string_view Host::TranslateToStringView(const std::string_view context, const std::string_view msg)
{
    return msg;
}

std::string Host::TranslateToString(const std::string_view context, const std::string_view msg)
{
    return std::string(msg);
}

void Host::AddKeyedOSDMessage(std::string key, std::string message, float duration)
{
    std::fprintf(stderr, "[pcsx2-gs] %s\n", message.c_str());
}

void Host::AddIconOSDMessage(std::string key, const char *icon, const std::string_view message, float duration)
{
    std::fprintf(stderr, "[pcsx2-gs] %.*s\n", static_cast<int>(message.size()), message.data());
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
    std::fprintf(stderr, "[pcsx2-gs] %.*s: %.*s\n", static_cast<int>(title.size()), title.data(), static_cast<int>(message.size()),
                 message.data());
}

// As PCSX2's pcsx2/Host.cpp (the Vulkan device reports errors through it).
void Host::ReportFormattedErrorAsync(const std::string_view title, const char *format, ...)
{
    std::va_list ap;
    va_start(ap, format);
    std::string message(StringUtil::StdStringFromFormatV(format, ap));
    va_end(ap);
    ReportErrorAsync(title, message);
}

void Host::RunOnCPUThread(std::function<void()> function, bool block)
{
    function();
}

bool Host::GetBaseBoolSettingValue(const char *section, const char *key, bool default_value)
{
    return default_value;
}

std::string Host::GetStringSettingValue(const char *section, const char *key, const char *default_value)
{
    return default_value ? default_value : "";
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
    return g_window;
}

void Host::BeginPresentFrame()
{
}

void Host::ReleaseRenderWindow()
{
}

bool Host::IsFullscreen()
{
    return false;
}

void Host::SetFullscreen(bool enabled)
{
}

// --- VM / GS thread ---
bool VMManager::HasValidVM()
{
    return true;
}

// The texture replacement folder is <EmuFolders::Textures>/<serial>/replacements; with SetTextureFolder the serial is
// the name of the folder given (a pack folder named anything, e.g. kept beside PCSX2's own).
std::string VMManager::GetDiscSerial()
{
    return g_textureSerial.empty() ? std::string("SLUS-21209") : g_textureSerial;
}

std::string VMManager::GetTitle(bool prefer_en)
{
    return "Urban Reign";
}

u32 VMManager::GetDiscCRC()
{
    return 0u;
}

const std::vector<u32> &VMManager::Internal::GetSoftwareRendererProcessorList()
{
    static const std::vector<u32> none;
    return none;
}

void MTGS::WaitGS(bool syncRegs, bool weakWait, bool isMTVU)
{
}

void MTGS::RunOnGSThread(AsyncCallType func)
{
    func();
}

void MTGS::ApplySettings()
{
}

void MTGS::ToggleSoftwareRendering()
{
}

bool GSDumpReplayer::IsReplayingDump()
{
    return false;
}

bool GSDumpReplayer::IsRunner()
{
    return false;
}

void GSDumpReplayer::RenderUI()
{
}

void GSDumpReplayer::UpdateGSStats()
{
}

void CrashHandler::WriteDumpForCaller()
{
}

// --- performance metrics ---
void PerformanceMetrics::Update(bool gs_register_write, bool fb_blit, bool is_skipping_present)
{
}

void PerformanceMetrics::OnGPUPresent(float gpu_time, u64 vs_invocations, u64 ps_invocations)
{
}

void PerformanceMetrics::SetGSSWThreadCount(u32 count)
{
}

void PerformanceMetrics::SetGSSWThread(u32 index, Threading::ThreadHandle thread)
{
}

PerformanceMetrics::InternalFPSMethod PerformanceMetrics::GetInternalFPSMethod()
{
    return InternalFPSMethod::None;
}

void PerformanceMetrics::StartSavingMetrics(u32 seconds)
{
}

bool PerformanceMetrics::IsSavingMetrics()
{
    return false;
}

void PerformanceMetrics::DumpSavedMetrics()
{
}

// --- ImGui: the devices draw ImGui's lists at every present, so a context and a frame must exist. No OSD yet: the
// lists stay empty (ImGuiManager.cpp of PCSX2 also loads its fonts, styles and the OSD, left out here).
bool ImGuiManager::Initialize()
{
    if (!ImGui::GetCurrentContext())
    {
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures |
                           ImGuiBackendFlags_HasGamepad;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        ApplyMenuStyle();
        io.DisplayFramebufferScale = ImVec2(1, 1);
        // The OSD font: PCSX2's Roboto when resources/fonts has it, else ImGui's built-in one.
        const std::string roboto = EmuFolders::Resources + "/fonts/Roboto-Regular.ttf";
        std::error_code error;
        g_osdFont = std::filesystem::exists(roboto, error) ? io.Fonts->AddFontFromFileTTF(roboto.c_str(), 20.0f) : nullptr;
        if (!g_osdFont)
            g_osdFont = io.Fonts->AddFontDefault();
#if defined(_WIN32)
        // Japanese and Korean (player names, the Korean menu tabs): Windows' own fonts merged into it; ImGui 1.92 loads
        // a glyph when it is first drawn. The first Japanese font found, then the first Korean one (Hangul).
        {
            const char *windir = std::getenv("WINDIR");
            const std::string fonts = std::string(windir && *windir ? windir : "C:\\Windows") + "\\Fonts\\";
            ImFontConfig merge;
            merge.MergeMode = true;
            for (const auto &choices : {std::initializer_list<const char *>{"YuGothM.ttc", "meiryo.ttc", "msgothic.ttc"},
                                        std::initializer_list<const char *>{"malgun.ttf", "gulim.ttc"}})
                for (const char *file : choices)
                    if (std::filesystem::exists(fonts + file, error))
                    {
                        io.Fonts->AddFontFromFileTTF((fonts + file).c_str(), 20.0f, &merge);
                        break;
                    }
        }
#endif
    }
    WindowResized();
    NewFrame();
    return true;
}

void ImGuiManager::Shutdown(bool clear_state)
{
    if (ImGui::GetCurrentContext())
        ImGui::DestroyContext();
}

void ImGuiManager::WindowResized()
{
    if (ImGui::GetCurrentContext())
        ImGui::GetIO().DisplaySize = ImVec2(static_cast<float>(std::max(1u, g_window.surface_width)),
                                            static_cast<float>(std::max(1u, g_window.surface_height)));
}

void ImGuiManager::RequestScaleUpdate()
{
}

void ImGuiManager::ReloadFonts()
{
}

void ImGuiManager::NewFrame()
{
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ApplyMenuInput();
    ImGui::NewFrame();
}

void ImGuiManager::SkipFrame()
{
    ImGui::EndFrame();
    NewFrame();
}

// The port's overlays (the host loop draws them with raylib under its own GS): a message box at the top left
// (save-state messages, notes) and the F3 FPS line at the bottom left, sized like the raylib ones.
void ImGuiManager::RenderOSD()
{
    Pcsx2GsMenuFn menu;
    void *menuUser;
    {
        std::lock_guard<std::mutex> lock(g_menuMutex);
        menu = g_menuDraw;
        menuUser = g_menuUser;
    }
    if (menu)
        menu(&g_ui, menuUser);

    std::string top, bottom;
    uint32_t topRgba;
    float topAlpha;
    {
        std::lock_guard<std::mutex> lock(g_osdMutex);
        top = g_osdTop;
        bottom = g_osdBottom;
        topRgba = g_osdTopRgba;
        topAlpha = g_osdTopAlpha;
    }
    if ((top.empty() || topAlpha <= 0.0f) && bottom.empty())
        return;
    ImDrawList *list = ImGui::GetForegroundDrawList();
    ImFont *font = g_osdFont ? g_osdFont : ImGui::GetFont();
    const float height = ImGui::GetIO().DisplaySize.y;
    const float size = std::max(16.0f, std::floor(height / 36.0f));
    const auto box = [&](const std::string &text, float x, float y, ImU32 background, ImU32 foreground)
    {
        const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
        list->AddRectFilled(ImVec2(x - 8.0f, y - 6.0f), ImVec2(x + extent.x + 8.0f, y + extent.y + 6.0f), background);
        list->AddText(font, size, ImVec2(x, y), foreground, text.c_str());
    };
    if (!top.empty() && topAlpha > 0.0f)
    {
        const auto channel = [&](int shift) { return static_cast<int>((topRgba >> shift) & 0xFFu); };
        const int alpha = static_cast<int>(channel(24) * topAlpha);
        box(top, 24.0f, 20.0f, IM_COL32(channel(0), channel(8), channel(16), alpha),
            IM_COL32(255, 255, 255, static_cast<int>(255 * topAlpha)));
    }
    if (!bottom.empty())
        box(bottom, 16.0f, height - size - 20.0f, IM_COL32(0, 0, 0, 153), IM_COL32(255, 255, 255, 255));
}

void FullscreenUI::Render()
{
}

const ImFontLoader *ImGuiFreeType::GetFontLoader()
{
    return nullptr;
}

// --- GS dumps, video capture, PNG snapshots (left out) ---
std::unique_ptr<GSDumpBase> GSDumpBase::CreateUncompressedDump(const std::string &fn, const std::string &serial, u32 crc, u32 screenshot_width,
                                                               u32 screenshot_height, const u32 *screenshot_pixels, const freezeData &fd,
                                                               const GSPrivRegSet *regs)
{
    return nullptr;
}

std::unique_ptr<GSDumpBase> GSDumpBase::CreateXzDump(const std::string &fn, const std::string &serial, u32 crc, u32 screenshot_width,
                                                     u32 screenshot_height, const u32 *screenshot_pixels, const freezeData &fd,
                                                     const GSPrivRegSet *regs)
{
    return nullptr;
}

std::unique_ptr<GSDumpBase> GSDumpBase::CreateZstDump(const std::string &fn, const std::string &serial, u32 crc, u32 screenshot_width,
                                                      u32 screenshot_height, const u32 *screenshot_pixels, const freezeData &fd,
                                                      const GSPrivRegSet *regs)
{
    return nullptr;
}

void GSDumpBase::ReadFIFO(u32 size)
{
}

void GSDumpBase::Transfer(int index, const u8 *mem, size_t size)
{
}

bool GSDumpBase::VSync(int field, bool last, const GSPrivRegSet *regs)
{
    return false;
}

bool GSCapture::BeginCapture(float fps, GSVector2i recommendedResolution, float aspect, std::string filename)
{
    return false;
}

bool GSCapture::DeliverVideoFrame(GSTexture *stex)
{
    return false;
}

void GSCapture::EndCapture()
{
}

bool GSCapture::IsCapturing()
{
    return false;
}

bool GSCapture::IsCapturingVideo()
{
    return false;
}

GSVector2i GSCapture::GetSize()
{
    return GSVector2i(0, 0);
}

std::string GSCapture::GetNextCaptureFileName()
{
    return {};
}

void GSCapture::Flush()
{
}

bool GSPng::Save(GSPng::Format fmt, const std::string &file, const u8 *image, int w, int h, int pitch, int compression, bool rb_swapped)
{
    return false;
}

RGBA8Image::RGBA8Image() = default;

RGBA8Image::RGBA8Image(RGBA8Image &&move)
    : Image(move)
{
}

bool RGBA8Image::SaveToFile(const char *filename, u8 quality) const
{
    return false;
}
