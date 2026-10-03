/// @file TicoOverlay.h
/// @brief Overlay UI for tico-integrated
#pragma once

#include "imgui.h"
#include <SDL.h>
#include <string>
#include <vector>
#include <map>
#include <memory>

class TicoCore;
class TicoShaderChain;

/// @brief Overlay menu types
enum class OverlayMenu
{
    None,
    QuickMenu,
    SaveStates,
    Settings,
    ShaderBrowser,
    ShaderParams
};

/// @brief Display mode for the emulator viewport
enum class GambatteDisplayMode
{
    Integer = 0,
    Display = 1,
    COUNT = 2
};

/// @brief Display size
enum class GambatteDisplaySize
{
    Stretch = 0,
    _4_3 = 1,
    _16_9 = 2,
    Original = 3,
    _1x = 4,
    _2x = 5,
    Auto = 6
};

/// @brief Overlay UI with tico styling
class TicoOverlay
{
public:
    TicoOverlay();
    ~TicoOverlay();

    /// @brief Update overlay animation
    void Update(float deltaTime);

    /// @brief Where the game goes on screen (x, y, width, height), in whole
    /// pixels, from the display mode and size settings.
    ImVec4 ComputeGameRect(ImVec2 displaySize, float aspectRatio) const;

    /// @brief Render the overlay; `gameTexture` fills `gameRect` 1:1
    void Render(ImVec2 displaySize, ImTextureID gameTexture, ImVec4 gameRect);

    /// @brief Handle input
    /// @return true if input was consumed by overlay
    bool HandleInput(SDL_GameController *controller);

    /// @brief Show/hide overlay
    void Show();
    void Hide();
    bool IsVisible() const { return m_currentMenu != OverlayMenu::None; }

    /// @brief Set game title for title card
    void SetGameTitle(const std::string &title) { m_gameTitle = title; }

    /// @brief Set core reference for save states
    void SetCore(TicoCore *core) { m_core = core; }

    /// @brief Check if user wants to exit to system (close owned title)
    bool ShouldExitToSystem() const { return m_shouldExitToSystem; }
    void ClearExitToSystem() { m_shouldExitToSystem = false; }

    /// @brief Check if user wants to exit to tico (chainload)
    bool ShouldExit() const { return m_shouldExit; }
    void ClearExit() { m_shouldExit = false; }

    /// @brief Selected slang preset path ("" = none)
    const std::string &GetShaderPreset() const { return m_shaderPreset; }
    /// @brief Put the selection back, e.g. after the preset failed to load
    void SetShaderPreset(const std::string &path) { m_shaderPreset = path; }

    /// @brief The chain whose parameters the Shader Parameters menu edits
    void SetShaderChain(TicoShaderChain *chain) { m_chain = chain; }
    /// @brief Apply the saved parameter values after the main loop loads a preset
    void OnShaderLoaded();

    /// @brief Check if user wants to reset
    bool ShouldReset() const { return m_shouldReset; }
    void ClearReset() { m_shouldReset = false; }

private:
    void RenderGame(ImDrawList *dl, ImVec2 displaySize, ImTextureID texture, ImVec4 rect);
    void RenderOverlayBackground(ImDrawList *dl, ImVec2 displaySize);
    void RenderTitleCard(ImDrawList *dl, ImVec2 displaySize);
    void RenderQuickMenu(ImDrawList *dl, ImVec2 displaySize);
    void RenderSaveStatesMenu(ImDrawList *dl, ImVec2 displaySize);
    void RenderSettingsMenu(ImDrawList *dl, ImVec2 displaySize);
    void RenderShaderBrowser(ImDrawList *dl, ImVec2 displaySize);
    void RenderShaderParams(ImDrawList *dl, ImVec2 displaySize);
    struct ListRow { std::string label, value; };
    void RenderScrollList(ImDrawList *dl, ImVec2 displaySize, const std::string &title,
                          const std::vector<ListRow> &rows, int selection, int &scroll, bool arrows);
    void RenderHelpersBar(ImDrawList *dl, ImVec2 displaySize);
    void RenderStatusBar(ImDrawList *dl, ImVec2 displaySize);
    void RenderRAAlerts(ImDrawList *dl, ImVec2 displaySize, float deltaTime);

    OverlayMenu m_currentMenu = OverlayMenu::None;
    std::string m_gameTitle;
    TicoCore *m_core = nullptr;

    float m_animTimer = 0.0f;

    int m_quickMenuSelection = 0;
    int m_saveStateSlot = 0;
    bool m_isSaveMode = true;
    int m_settingsSelection = 0;
    std::string m_shaderPreset; // "" = none
    std::vector<std::string> m_shaderPresets; // "" first, then built-ins, then SD
    void ScanShaderPresets();
    void CycleShaderPreset(int dir);
    std::string ShaderPresetLabel() const;

    // Shader browser: folders and .slangp files under the user shader dir.
    struct BrowseEntry { std::string label, path; bool isDir; };
    std::vector<BrowseEntry> m_browseEntries;
    std::string m_browseDir;
    int m_browseSel = 0;
    int m_browseScroll = 0;
    void OpenShaderBrowser(const std::string &dir);
    void ActivateBrowseEntry();

    // Shader parameters, saved per preset (only values off their default).
    TicoShaderChain *m_chain = nullptr;
    std::map<std::string, std::map<std::string, float>> m_shaderParams;
    int m_paramSel = 0;
    int m_paramScroll = 0;
    bool m_paramsDirty = false;
    void AdjustShaderParam(int dir);
    void ResetShaderParams();
    void LeaveShaderParams();
    
    GambatteDisplayMode m_displayMode = GambatteDisplayMode::Display;
    GambatteDisplaySize m_displaySize = GambatteDisplaySize::_4_3;

    void LoadCoreSettings();
    void SaveCoreSettings();
    void ApplyScalingSettings(bool save = true);


    bool m_upHeld = false;
    bool m_downHeld = false;
    bool m_leftHeld = false;
    bool m_rightHeld = false;
    bool m_confirmHeld = false;
    bool m_backHeld = false;
    bool m_toggleHeld = false;
    bool m_xHeld = false;
    uint32_t m_lastInputTime = 0;
    static constexpr uint32_t DEBOUNCE_MS = 200;

    bool m_shouldExitToSystem = false;
    bool m_shouldExit = false;
    bool m_shouldReset = false;

    uint32_t m_batteryLevel = 100;
    bool m_isCharging = false;
    float m_batteryTimer = 0.0f;
    float m_chargingStateProgress = 0.0f;
    ImTextureID m_boltTexture = ImTextureID_Invalid;
    int m_boltWidth = 0;
    int m_boltHeight = 0;

    bool m_isDarkMode = true;
    bool m_showNickname = false;
    std::string m_hourFormat = "24h";
    void LoadConfig();
    void LoadGeneralConfig();
    void LoadSVGIcon();

    ImTextureID m_avatarTexture = ImTextureID_Invalid;
    std::string m_nickname;
    void LoadAccountData();
    void RenderSocialArea(ImDrawList *dl, ImVec2 displaySize);
};
