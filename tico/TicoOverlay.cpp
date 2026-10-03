/// @file TicoOverlay.cpp
/// @brief Overlay UI for tico-integrated snes9x (no disc support)

#define IMGUI_DEFINE_MATH_OPERATORS
#include "TicoOverlay.h"
#include "TicoCore.h"
#include "TicoConfig.h"
#include "TicoTranslationManager.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include "TicoUtils.h"
#include <json.hpp>

#include "TicoShaderChain.h"
#include "TicoVulkan.h"
#include <cstdio>
#include <cstring>
#include <strings.h>

#include <sys/stat.h>
#include <dirent.h>
#include <string>

#ifdef __SWITCH__
#include <switch.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "deps/stb/stb_image.h"
#define NANOSVG_IMPLEMENTATION
#include "deps/nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "deps/nanosvg/nanosvgrast.h"

#ifdef __SWITCH__
static const char *kBuiltinShaderDir = "romfs:/shaders/";
static const char *kUserShaderDir = "sdmc:/tico/shaders/";
#else
static const char *kBuiltinShaderDir = "tico/shaders/";
static const char *kUserShaderDir = "shaders/";
#endif

static std::string GetStatePath(TicoCore *core, int slot)
{
    if (!core) return "";
    std::string romPath = core->GetGamePath();
    std::string romName = romPath;
    size_t lastSlash = romName.find_last_of("/\\");
    if (lastSlash != std::string::npos) romName = romName.substr(lastSlash + 1);
    size_t lastDot = romName.find_last_of(".");
    if (lastDot != std::string::npos) romName = romName.substr(0, lastDot);
    TicoConfig::MakeDirs(TicoConfig::StatesPath());
    return TicoConfig::StatesPath() + romName + ".state" + std::to_string(slot);
}

// Selection border, as tico-nx draws it: a 512x4 gradient strip scrolled
// around the rounded outline. The strip follows the user's Screen Colors >
// Animated Border pick (display.jsonc "border_tint"); these files and their
// index mapping mirror tico-nx's TintPalette::GetBorderGradientFile.
#ifdef __SWITCH__
static const char *kBorderDir = "romfs:/assets/border/";
#else
static const char *kBorderDir = "tico/assets/border/";
#endif
static ImTextureID s_borderTexture = ImTextureID_Invalid;
static int s_borderTint = -2;

static std::string BorderGradientFile(int tint) {
    static const char *kSlugs[] = {"default", "aqua", "violet", "sunset", "lime", "rose", "gold",
                                   "ice", "ember", "mint", "lagoon", "cobalt", "original"};
    const int count = (int)(sizeof(kSlugs) / sizeof(kSlugs[0]));
    const int original = count - 1;
    if (tint <= 0 || tint >= count || tint == original) return "border_gradient.png";
    return std::string("border_gradient_") + kSlugs[tint] + ".png";
}

static void LoadBorderTexture(int tint) {
    if (tint == s_borderTint) return;
    s_borderTint = tint;
    TicoVulkan::DestroyTexture(s_borderTexture);
    s_borderTexture = ImTextureID_Invalid;
    int w, h, ch;
    std::string path = kBorderDir + BorderGradientFile(tint);
    if (unsigned char *px = stbi_load(path.c_str(), &w, &h, &ch, 4)) {
        s_borderTexture = TicoVulkan::CreateTextureRGBA(px, w, h);
        stbi_image_free(px);
    }
}

namespace UIStyle {
    // Port of tico-nx UIStyle::DrawAnimatedGradientBorder (textured path).
    inline void DrawAnimatedGradientBorder(ImDrawList *dl, ImVec2 min, ImVec2 max, float cornerRadius,
                                           float frameWidth, float alpha, float time, ImTextureID texture) {
        float phase = time * 0.5f;
        float phaseMod = phase - floorf(phase);
        float w = max.x - min.x, h = max.y - min.y;
        cornerRadius = std::min(cornerRadius, std::min(w, h) * 0.5f);
        float perimeter = 2.0f * (w + h - 4.0f * cornerRadius) + 2.0f * 3.14159f * cornerRadius;
        struct BorderPoint { ImVec2 pos, normal; float dist; };
        ImVector<BorderPoint> points;
        points.reserve(64);
        float currentDist = 0.0f;
        auto addPoint = [&](ImVec2 pos, ImVec2 normal) {
            if (points.Size > 0) {
                float dx = pos.x - points.back().pos.x, dy = pos.y - points.back().pos.y;
                currentDist += sqrtf(dx * dx + dy * dy);
            }
            points.push_back({pos, normal, currentDist / perimeter});
        };
        ImVec2 centers[4] = {ImVec2(min.x + cornerRadius, min.y + cornerRadius), ImVec2(max.x - cornerRadius, min.y + cornerRadius),
                             ImVec2(max.x - cornerRadius, max.y - cornerRadius), ImVec2(min.x + cornerRadius, max.y - cornerRadius)};
        const int segs = 12;
        for (int c = 0; c < 4; c++) {
            float start = -3.14159f + c * 1.5708f;
            for (int i = 0; i <= segs; i++) {
                float a = start + 1.5708f * i / segs;
                addPoint(ImVec2(centers[c].x + cosf(a) * cornerRadius, centers[c].y + sinf(a) * cornerRadius), ImVec2(cosf(a), sinf(a)));
            }
        }
        ImU32 tint = IM_COL32(255, 255, 255, (int)(255 * alpha));
        float halfW = frameWidth * 0.5f;
        auto lerp2 = [](const ImVec2 &a, const ImVec2 &b, float t) { return ImVec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t); };
        for (int i = 0; i < points.Size; i++) {
            int next = (i + 1) % points.Size;
            const BorderPoint &p1 = points[i], &p2 = points[next];
            ImVec2 v1o(p1.pos.x + p1.normal.x * halfW, p1.pos.y + p1.normal.y * halfW), v1i(p1.pos.x - p1.normal.x * halfW, p1.pos.y - p1.normal.y * halfW);
            ImVec2 v2o(p2.pos.x + p2.normal.x * halfW, p2.pos.y + p2.normal.y * halfW), v2i(p2.pos.x - p2.normal.x * halfW, p2.pos.y - p2.normal.y * halfW);
            float u1 = p1.dist + phaseMod, u2 = (next == 0 ? 1.0f : p2.dist) + phaseMod;
            // Keep UVs inside [0,1]: split a segment that crosses the wrap.
            auto emit = [&](float t1, float t2, float us, float ue) {
                dl->AddImageQuad(texture, lerp2(v1o, v2o, t1), lerp2(v1o, v2o, t2), lerp2(v1i, v2i, t2), lerp2(v1i, v2i, t1),
                                 ImVec2(us, 0.0f), ImVec2(ue, 0.0f), ImVec2(ue, 1.0f), ImVec2(us, 1.0f), tint);
            };
            if (u1 < 1.0f && u2 > 1.0f) {
                float ts = (1.0f - u1) / (u2 - u1);
                emit(0.0f, ts, u1, 1.0f);
                emit(ts, 1.0f, 0.0f, u2 - 1.0f);
            } else {
                if (u1 >= 1.0f) u1 -= 1.0f;
                if (u2 >= 1.0f) u2 -= 1.0f;
                emit(0.0f, 1.0f, u1, u2);
            }
        }
    }

    /// Highlight for the selected row of a menu.
    inline void DrawSelection(ImDrawList *dl, ImVec2 itemMin, ImVec2 itemMax, float cornerRadius, float alpha, bool isDark) {
        float scale = ImGui::GetIO().FontGlobalScale;
        if (s_borderTexture != ImTextureID_Invalid) {
            DrawAnimatedGradientBorder(dl, itemMin, itemMax, cornerRadius, 4.0f * scale,
                                       alpha, (float)ImGui::GetTime(), s_borderTexture);
        } else {
            ImU32 selCol = isDark ? IM_COL32(60,60,60,(int)(255*alpha)) : IM_COL32(190,195,205,(int)(255*alpha));
            dl->AddRectFilled(itemMin, itemMax, selCol, cornerRadius);
        }
    }

    inline void DrawTextWithShadow(ImDrawList *dl, ImVec2 pos, ImU32 color, const char *text, float shadowOffset = 1.5f) {
        dl->AddText(ImVec2(pos.x + shadowOffset, pos.y + shadowOffset), IM_COL32(0,0,0,50), text);
        dl->AddText(pos, color, text);
    }
    static void DrawSwitchButton(ImDrawList *dl, ImFont *font, float fontSize, ImVec2 center, float size, const char *symbol, float alpha, bool isDark) {
        ImU32 fillCol = IM_COL32(220, 220, 220, (int)(255 * alpha));
        ImU32 textCol = IM_COL32(40, 40, 40, (int)(255 * alpha));
        dl->AddCircleFilled(center, size * 0.5f, fillCol, 12);
        float symSize = fontSize * 0.75f;
        ImVec2 textSize = font->CalcTextSizeA(symSize, FLT_MAX, 0.0f, symbol);
        dl->AddText(font, symSize, center - (textSize * 0.5f), textCol, symbol);
    }
}

TicoOverlay::TicoOverlay() {
    m_gameTitle = "Snes9x";
    LoadConfig();
    LoadGeneralConfig();
    LoadAccountData();
    LoadCoreSettings();
#ifdef __SWITCH__
    psmInitialize();
#endif
}

TicoOverlay::~TicoOverlay()
{
    TicoVulkan::DestroyTexture(s_borderTexture);
    s_borderTexture = ImTextureID_Invalid;
    s_borderTint = -2;
    TicoVulkan::DestroyTexture(m_boltTexture);
    TicoVulkan::DestroyTexture(m_avatarTexture);

#ifdef __SWITCH__
    psmExit();
#endif
}

void TicoOverlay::LoadConfig() {
    const char *configPaths[] = {"sdmc:/tiicu/config/display.jsonc","sdmc:/tico/config/display.jsonc","tico/config/display.jsonc"};
    m_isDarkMode = true; m_showNickname = false;
    int borderTint = 0;
    FILE *fp = nullptr;
    for (const char *path : configPaths) { fp = fopen(path, "rb"); if (fp) break; }
    if (fp) {
        fseek(fp, 0, SEEK_END); long size = ftell(fp); fseek(fp, 0, SEEK_SET);
        if (size > 0) {
            std::string content; content.resize(size); fread(&content[0], 1, size, fp);
            auto j = nlohmann::json::parse(content, nullptr, false, true);
                if (!j.is_discarded()) {
                    if (j.contains("dark_mode") && j["dark_mode"].is_boolean()) m_isDarkMode = j["dark_mode"].get<bool>();
                    else if (j.contains("darkMode") && j["darkMode"].is_boolean()) m_isDarkMode = j["darkMode"].get<bool>();
                    if (j.contains("show_nickname") && j["show_nickname"].is_boolean()) m_showNickname = j["show_nickname"].get<bool>();
                    if (j.contains("border_tint") && j["border_tint"].is_number_integer()) borderTint = j["border_tint"].get<int>();
                    else if (j.contains("showNickname") && j["showNickname"].is_boolean()) m_showNickname = j["showNickname"].get<bool>();
                }
        }
        fclose(fp);
    }
    LoadBorderTexture(borderTint);
}

void TicoOverlay::LoadGeneralConfig() {
    const char *configPaths[] = {"sdmc:/tiicu/config/general.jsonc","sdmc:/tico/config/general.jsonc","tico/config/general.jsonc"};
    m_hourFormat = "24h";
    FILE *fp = nullptr;
    for (const char *path : configPaths) { fp = fopen(path, "rb"); if (fp) break; }
    if (fp) {
        fseek(fp, 0, SEEK_END); long size = ftell(fp); fseek(fp, 0, SEEK_SET);
        if (size > 0) {
            std::string content; content.resize(size); fread(&content[0], 1, size, fp);
            auto j = nlohmann::json::parse(content, nullptr, false, true);
                if (!j.is_discarded() && j.contains("hour_format") && j["hour_format"].is_string())
                    m_hourFormat = j["hour_format"].get<std::string>();
        }
        fclose(fp);
    }
}

void TicoOverlay::LoadAccountData() {
#ifdef __SWITCH__
    bool customAvatarLoaded = false;
    const char *avatarPaths[] = {"sdmc:/tico/assets/avatar.jpg"};
    for (const char *path : avatarPaths) {
        FILE *fp = fopen(path, "rb"); if (!fp) continue; fclose(fp);
        int width, height, channels;
        unsigned char *data = stbi_load(path, &width, &height, &channels, 4);
        if (data) {
            TicoVulkan::DestroyTexture(m_avatarTexture);
            m_avatarTexture = TicoVulkan::CreateTextureRGBA(data, width, height);
            stbi_image_free(data);
            m_nickname = "Player 1"; customAvatarLoaded = true; break;
        }
    }
    if (customAvatarLoaded) return;
    Result rc = accountInitialize(AccountServiceType_Application);
    if (R_FAILED(rc)) return;
    AccountUid uid = {0}; bool found = false;
    if (R_SUCCEEDED(accountGetPreselectedUser(&uid)) && accountUidIsValid(&uid)) found = true;
    if (!found && R_SUCCEEDED(accountGetLastOpenedUser(&uid)) && accountUidIsValid(&uid)) found = true;
    if (!found) {
        s32 userCount = 0;
        if (R_SUCCEEDED(accountGetUserCount(&userCount)) && userCount > 0) {
            AccountUid uids[ACC_USER_LIST_SIZE]; s32 actualTotal = 0;
            if (R_SUCCEEDED(accountListAllUsers(uids, ACC_USER_LIST_SIZE, &actualTotal)) && actualTotal > 0) { uid = uids[0]; found = true; }
        }
    }
    if (found) {
        AccountProfile profile; AccountProfileBase profileBase;
        if (R_SUCCEEDED(accountGetProfile(&profile, uid))) {
            if (R_SUCCEEDED(accountProfileGet(&profile, NULL, &profileBase))) m_nickname = std::string(profileBase.nickname);
            u32 imageSize = 0;
            if (R_SUCCEEDED(accountProfileGetImageSize(&profile, &imageSize)) && imageSize > 0) {
                unsigned char *jpegBuf = (unsigned char *)malloc(imageSize);
                if (jpegBuf) {
                    u32 actualSize = 0;
                    if (R_SUCCEEDED(accountProfileLoadImage(&profile, jpegBuf, imageSize, &actualSize))) {
                        int width, height, channels;
                        unsigned char *rgba = stbi_load_from_memory(jpegBuf, actualSize, &width, &height, &channels, 4);
                        if (rgba) {
                            TicoVulkan::DestroyTexture(m_avatarTexture);
                            m_avatarTexture = TicoVulkan::CreateTextureRGBA(rgba, width, height);
                            stbi_image_free(rgba);
                        }
                    }
                    free(jpegBuf);
                }
            }
            accountProfileClose(&profile);
        }
    }
    accountExit();
#else
    m_nickname = "Player 1";
#endif
}

void TicoOverlay::Update(float deltaTime) {
    if (m_currentMenu != OverlayMenu::None) {
        m_animTimer += deltaTime;
#ifdef __SWITCH__
        m_batteryTimer += deltaTime;
        if (m_batteryTimer >= 3.0f) {
            m_batteryTimer = 0.0f;
            psmGetBatteryChargePercentage(&m_batteryLevel);
            PsmChargerType chargerType; psmGetChargerType(&chargerType);
            m_isCharging = (chargerType != PsmChargerType_Unconnected);
        }
        float target = m_isCharging ? 1.0f : 0.0f;
        float diff = target - m_chargingStateProgress;
        if (std::abs(diff) > 0.001f) {
            m_chargingStateProgress += diff * deltaTime * 8.0f;
            m_chargingStateProgress = std::clamp(m_chargingStateProgress, 0.0f, 1.0f);
        }
#endif
    }
}

void TicoOverlay::Show() {
    if (m_currentMenu == OverlayMenu::None) {
        m_currentMenu = OverlayMenu::QuickMenu;
        m_animTimer = 0.0f; m_quickMenuSelection = 0;
        LoadConfig(); LoadGeneralConfig();
    }
}

void TicoOverlay::Hide() { m_currentMenu = OverlayMenu::None; }

void TicoOverlay::Render(ImVec2 displaySize, ImTextureID gameTexture, ImVec4 gameRect) {
    ImDrawList *bgDrawList = ImGui::GetBackgroundDrawList();
    ImDrawList *fgDrawList = ImGui::GetForegroundDrawList();
    RenderGame(bgDrawList, displaySize, gameTexture, gameRect);
    if (m_currentMenu != OverlayMenu::None) {
        RenderOverlayBackground(fgDrawList, displaySize);
        RenderTitleCard(fgDrawList, displaySize);
        switch (m_currentMenu) {
        case OverlayMenu::QuickMenu: RenderQuickMenu(fgDrawList, displaySize); break;
        case OverlayMenu::SaveStates: RenderSaveStatesMenu(fgDrawList, displaySize); break;
        case OverlayMenu::Settings: RenderSettingsMenu(fgDrawList, displaySize); break;
        case OverlayMenu::ShaderBrowser: RenderShaderBrowser(fgDrawList, displaySize); break;
        case OverlayMenu::ShaderParams: RenderShaderParams(fgDrawList, displaySize); break;
        default: break;
        }
        RenderHelpersBar(fgDrawList, displaySize);
        RenderSocialArea(fgDrawList, displaySize);
        RenderStatusBar(fgDrawList, displaySize);
    }
    // RA alerts always render (even during gameplay, not just when menu is open)
    RenderRAAlerts(fgDrawList, displaySize, ImGui::GetIO().DeltaTime);
}

void TicoOverlay::RenderRAAlerts(ImDrawList *dl, ImVec2 displaySize, float deltaTime) {
    if (!m_core) return;
    auto& notifications = m_core->m_raNotifications;
    if (notifications.empty()) return;

    // Lazy-load RA icon from SVG if not loaded yet
    if (m_core->m_raIconTexture == ImTextureID_Invalid) {
        // Load ra.svg as texture using nanosvg (available in this TU)
        const char* svgPath = "romfs:/assets/ra.svg";
        NSVGimage* image = nsvgParseFromFile(svgPath, "px", 96);
        if (image) {
            float sc = 64.0f / image->height;
            int w = (int)(image->width * sc), h = (int)(image->height * sc);
            NSVGrasterizer* rast = nsvgCreateRasterizer();
            if (rast) {
                unsigned char* img = (unsigned char*)malloc(w * h * 4);
                if (img) {
                    nsvgRasterize(rast, image, 0, 0, sc, img, w, h, w * 4);
                    m_core->m_raIconTexture = TicoVulkan::CreateTextureRGBA(img, w, h);
                    free(img);
                }
                nsvgDeleteRasterizer(rast);
            }
            nsvgDelete(image);
        }
    }

    float scale = ImGui::GetIO().FontGlobalScale;
    ImFont *font = ImGui::GetFont();
    ImFont *descFont = font;
    if (ImGui::GetIO().Fonts->Fonts.Size > 1) {
        descFont = ImGui::GetIO().Fonts->Fonts[1];
    }
    float descFontSize = ImGui::GetFontSize() * 0.65f;
    float titleFontSize = ImGui::GetFontSize() * 0.85f;

    // Alert dimensions
    float alertW = 420.0f * scale; // wider
    float alertH = 100.0f * scale; // taller
    float padding = 12.0f * scale;
    float margin = 16.0f * scale;
    float spacing = 8.0f * scale;
    float cornerRadius = 14.0f * scale;
    float badgeSize = 76.0f * scale; // fits padding perfectly (100 - 24 = 76)
    float badgeRadius = 4.0f * scale; // less roundness per RA spec
    float badgeMargin = 12.0f * scale;

    RAAlertPosition pos = m_core->m_raAlertPosition;
    bool isTop = (pos == RAAlertPosition::TopLeft || pos == RAAlertPosition::TopRight);
    bool isRight = (pos == RAAlertPosition::TopRight || pos == RAAlertPosition::BottomRight);

    // Update timers and remove expired
    for (auto& n : notifications) {
        n.timer += deltaTime;
    }
    notifications.erase(
        std::remove_if(notifications.begin(), notifications.end(),
            [](const RANotification& n) { return n.timer >= n.duration; }),
        notifications.end());

    // Render each notification
    for (size_t i = 0; i < notifications.size(); i++) {
        auto& n = notifications[i];

        // Lazy-resolve badge texture (may have been downloaded after notification was pushed)
        if (n.textureId == ImTextureID_Invalid && !n.badge_name.empty()) {
            if (n.badge_name == "ra_icon") {
                n.textureId = m_core->m_raIconTexture;
            } else {
                n.textureId = m_core->GetRABadgeTexture(n.badge_name);
            }
        }

        // Calculate slide animation
        float slideProgress;
        if (n.timer < n.slideIn) {
            float t = n.timer / n.slideIn;
            slideProgress = 1.0f - std::pow(1.0f - t, 3.0f);
        } else if (n.timer > n.duration - n.slideOut) {
            float t = (n.duration - n.timer) / n.slideOut;
            slideProgress = 1.0f - std::pow(1.0f - t, 3.0f);
        } else {
            slideProgress = 1.0f;
        }

        // Calculate position
        float stackOffset = (float)i * (alertH + spacing);
        float anchorX = isRight ? (displaySize.x - alertW - margin) : margin;
        float anchorY = isTop ? (margin + stackOffset) : (displaySize.y - margin - alertH - stackOffset);
        float slideOffsetY = isTop
            ? -(alertH + margin + stackOffset) * (1.0f - slideProgress)
            : (alertH + margin + stackOffset) * (1.0f - slideProgress);

        float drawY = anchorY + slideOffsetY;
        int alpha = (int)(230 * slideProgress);
        if (alpha <= 0) continue;

        ImVec2 rectMin(anchorX, drawY);
        ImVec2 rectMax(anchorX + alertW, drawY + alertH);

        // Background — glassmorphic rounded rectangle
        ImU32 bgColor = m_isDarkMode
            ? IM_COL32(35, 35, 40, alpha)
            : IM_COL32(245, 248, 252, alpha);
        ImU32 borderColor = m_isDarkMode
            ? IM_COL32(70, 70, 80, (int)(180 * slideProgress))
            : IM_COL32(200, 205, 215, (int)(200 * slideProgress));

        dl->AddRectFilled(rectMin, rectMax, bgColor, cornerRadius);
        dl->AddRect(rectMin, rectMax, borderColor, cornerRadius, 0, 1.5f * scale);

        // Badge image (left side)
        float textX = rectMin.x + padding;
        if (n.textureId != ImTextureID_Invalid) {
            float badgeX = rectMin.x + badgeMargin;
            float badgeY = rectMin.y + (alertH - badgeSize) * 0.5f;

            float drawBadgeSize = badgeSize;
            float drawBadgeX = badgeX;
            float drawBadgeY = badgeY;

            // Make the general RA icon a bit smaller to fit visually better
            if (n.badge_name == "ra_icon") {
                drawBadgeSize = badgeSize * 0.70f;
                drawBadgeX += (badgeSize - drawBadgeSize) * 0.5f;
                drawBadgeY += (badgeSize - drawBadgeSize) * 0.5f;
            }

            ImVec2 bMin(drawBadgeX, drawBadgeY);
            ImVec2 bMax(drawBadgeX + drawBadgeSize, drawBadgeY + drawBadgeSize);
            ImU32 imgCol = IM_COL32(255, 255, 255, alpha);
            dl->AddImageRounded(n.textureId,
                bMin, bMax, ImVec2(0,0), ImVec2(1,1), imgCol, badgeRadius);
            
            textX = badgeX + badgeSize + badgeMargin;
        }

        // Description text
        ImU32 descColor = m_isDarkMode
            ? IM_COL32(185, 185, 195, alpha)
            : IM_COL32(80, 80, 95, alpha);
        float maxDescW = rectMax.x - textX - padding;

        ImU32 titleColor = m_isDarkMode
            ? IM_COL32(255, 255, 255, alpha)
            : IM_COL32(30, 30, 40, alpha);

        std::string desc = n.description;
        float maxDescH = descFontSize * 2.5f; // height for roughly 2 lines
        ImVec2 fullSize = descFont->CalcTextSizeA(descFontSize, FLT_MAX, maxDescW, desc.c_str());
        
        // If content goes through 2 lines, slice and add '...'
        if (fullSize.y > maxDescH) {
            desc += "...";
            while (desc.length() > 4) {
                ImVec2 testSize = descFont->CalcTextSizeA(descFontSize, FLT_MAX, maxDescW, desc.c_str());
                if (testSize.y <= maxDescH) break;
                desc.erase(desc.length() - 4, 1);
            }
        }

        std::string titleStr = n.title;
        ImVec2 titleSize = font->CalcTextSizeA(titleFontSize, FLT_MAX, 0.0f, titleStr.c_str());
        if (titleSize.x > maxDescW) {
            titleStr += "...";
            while (titleStr.length() > 4) {
                ImVec2 testSize = font->CalcTextSizeA(titleFontSize, FLT_MAX, 0.0f, titleStr.c_str());
                if (testSize.x <= maxDescW) break;
                titleStr.erase(titleStr.length() - 4, 1);
            }
            // Recalculate titleSize for accurate vertical centering
            titleSize = font->CalcTextSizeA(titleFontSize, FLT_MAX, 0.0f, titleStr.c_str());
        }

        ImVec2 descSize = descFont->CalcTextSizeA(descFontSize, FLT_MAX, maxDescW, desc.c_str());
        
        float textSpacing = 4.0f * scale;
        float totalTextH = titleSize.y + textSpacing + descSize.y;
        float titleY = rectMin.y + (alertH - totalTextH) * 0.5f;
        float descY = titleY + titleSize.y + textSpacing;

        dl->AddText(font, titleFontSize,
            ImVec2(textX + 1.0f, titleY + 1.0f),
            IM_COL32(0, 0, 0, (int)(80 * slideProgress)),
            titleStr.c_str());
        dl->AddText(font, titleFontSize,
            ImVec2(textX, titleY), titleColor, titleStr.c_str());

        dl->AddText(descFont, descFontSize, ImVec2(textX, descY), descColor, desc.c_str(), nullptr, maxDescW);
    }
}
void TicoOverlay::RenderSocialArea(ImDrawList *dl, ImVec2 displaySize) {
    if (m_animTimer <= 0.0f) return;
    float t = std::min(m_animTimer / 0.4f, 1.0f);
    float ease = 1.0f - std::pow(1.0f - t, 3.0f);
    if (ease < 0.01f) return;
    float scale = ImGui::GetIO().FontGlobalScale;
    float AVATAR_SIZE = 72.0f * scale, sideMargin = 32.0f * scale, topMargin = 32.0f * scale, barHeight = 50.0f * scale;
    float startOffset = 200.0f, currentOffset = startOffset * (1.0f - ease);
    ImVec2 avatarCenter(sideMargin + AVATAR_SIZE * 0.5f - currentOffset, topMargin + barHeight * 0.5f);
    float radius = AVATAR_SIZE * 0.5f;
    ImU32 baseCol = m_isDarkMode ? IM_COL32(45,45,45,(int)(255*ease)) : IM_COL32(245,247,250,(int)(200*ease));
    dl->AddCircleFilled(avatarCenter, radius, baseCol);
    if (m_avatarTexture != 0) {
        float imgRadius = radius - 4.0f;
        dl->AddImageRounded(m_avatarTexture,
            ImVec2(avatarCenter.x-imgRadius, avatarCenter.y-imgRadius),
            ImVec2(avatarCenter.x+imgRadius, avatarCenter.y+imgRadius),
            ImVec2(0,0), ImVec2(1,1), IM_COL32_WHITE, imgRadius);
        dl->AddCircle(avatarCenter, imgRadius, IM_COL32(255,255,255,60), 0, 1.0f);
    } else {
        dl->AddCircleFilled(avatarCenter, radius - 4.0f, IM_COL32(200,200,210,255));
    }
}

ImVec4 TicoOverlay::ComputeGameRect(ImVec2 displaySize, float aspectRatio) const {
    static constexpr int CORE_BASE_W = 256, CORE_BASE_H = 224;
    float dstWidth = displaySize.x, dstHeight = displaySize.y, offsetX = 0, offsetY = 0;
    if (m_displayMode == GambatteDisplayMode::Integer) {
        int scale;
        if (m_displaySize == GambatteDisplaySize::Auto) {
            int scaleX = (int)displaySize.x / CORE_BASE_W, scaleY = (int)displaySize.y / CORE_BASE_H;
            scale = std::min(scaleX, scaleY); if (scale < 1) scale = 1;
        } else { scale = (int)m_displaySize - 3; if (scale < 1) scale = 1; }
        dstWidth = CORE_BASE_W * scale; dstHeight = CORE_BASE_H * scale;
        if (dstWidth > displaySize.x) dstWidth = displaySize.x;
        if (dstHeight > displaySize.y) dstHeight = displaySize.y;
    } else {
        switch (m_displaySize) {
        case GambatteDisplaySize::Stretch: dstWidth = displaySize.x; dstHeight = displaySize.y; break;
        case GambatteDisplaySize::_4_3: {
            float ar = 4.0f/3.0f, da = displaySize.x/displaySize.y;
            if (ar > da) { dstWidth = displaySize.x; dstHeight = displaySize.x/ar; }
            else { dstHeight = displaySize.y; dstWidth = displaySize.y*ar; } break;
        }
        case GambatteDisplaySize::_16_9: {
            float ar = 16.0f/9.0f, da = displaySize.x/displaySize.y;
            if (ar > da) { dstWidth = displaySize.x; dstHeight = displaySize.x/ar; }
            else { dstHeight = displaySize.y; dstWidth = displaySize.y*ar; } break;
        }
        default: {
            float da = displaySize.x/displaySize.y;
            if (aspectRatio > da) { dstWidth = displaySize.x; dstHeight = displaySize.x/aspectRatio; }
            else { dstHeight = displaySize.y; dstWidth = displaySize.y*aspectRatio; } break;
        }}
    }
    
    // Ensure all dimensions are floored to integers to avoid fractional rendering fringes
    dstWidth = std::floor(dstWidth);
    dstHeight = std::floor(dstHeight);
    offsetX = std::floor((displaySize.x - dstWidth) / 2.0f); 
    offsetY = std::floor((displaySize.y - dstHeight) / 2.0f);
    return ImVec4(offsetX, offsetY, dstWidth, dstHeight);
}

void TicoOverlay::RenderGame(ImDrawList *dl, ImVec2 displaySize, ImTextureID texture, ImVec4 rect) {
    dl->AddRectFilled(ImVec2(0,0), displaySize, IM_COL32(0,0,0,255));
    if (texture == ImTextureID_Invalid) return;
    // The shader chain rendered at exactly this size, so this is a 1:1 copy.
    dl->AddImage(texture, ImVec2(rect.x, rect.y), ImVec2(rect.x + rect.z, rect.y + rect.w));
}

void TicoOverlay::RenderOverlayBackground(ImDrawList *dl, ImVec2 displaySize) {
    float t = std::min(m_animTimer / 0.4f, 1.0f);
    float ease = 1.0f - std::pow(1.0f - t, 3.0f);
    int baseAlpha = (int)(200 * ease), maxAlpha = (int)(250 * ease);
    if (baseAlpha > 0) {
        float topH = displaySize.y * 0.20f, botH = displaySize.y * 0.20f;
        ImU32 colMax = IM_COL32(0,0,0,maxAlpha), colBase = IM_COL32(0,0,0,baseAlpha);
        dl->AddRectFilledMultiColor(ImVec2(0,0), ImVec2(displaySize.x,topH), colMax, colMax, colBase, colBase);
        dl->AddRectFilled(ImVec2(0,topH), ImVec2(displaySize.x,displaySize.y-botH), colBase);
        dl->AddRectFilledMultiColor(ImVec2(0,displaySize.y-botH), ImVec2(displaySize.x,displaySize.y), colBase, colBase, colMax, colMax);
    }
}

void TicoOverlay::RenderTitleCard(ImDrawList *dl, ImVec2 displaySize) {
    if (m_animTimer <= 0.0f) return;
    std::string titleStr = m_gameTitle;
    if (m_currentMenu == OverlayMenu::SaveStates) titleStr = m_isSaveMode ? tr("emulator_save_state") : tr("emulator_load_state");
    else if (m_currentMenu == OverlayMenu::Settings) titleStr = tr("emulator_settings");
    titleStr.erase(titleStr.find_last_not_of(" \n\r\t") + 1);
    if (titleStr.length() > 50) titleStr = titleStr.substr(0, 47) + "...";
    float scale = ImGui::GetIO().FontGlobalScale;
    float TITLE_HEIGHT = 72.0f * scale, AVAILABLE_TOP = 110.0f * scale;
    float cardWidth = displaySize.x * 0.4f, cardX = (displaySize.x - cardWidth) * 0.5f, cardY = (AVAILABLE_TOP - TITLE_HEIGHT) * 0.5f;
    float t = std::min(m_animTimer / 0.4f, 1.0f);
    float easeOut = 1.0f - std::pow(1.0f - t, 3.0f);
    float startY = -150.0f * scale, currentY = startY + (cardY - startY) * easeOut;
    ImVec2 textSize = ImGui::CalcTextSize(titleStr.c_str());
    float textX = cardX + (cardWidth - textSize.x) * 0.5f, textY = currentY + (TITLE_HEIGHT - textSize.y) * 0.5f;
    UIStyle::DrawTextWithShadow(dl, ImVec2(textX, textY), IM_COL32(200,200,200,255), titleStr.c_str());
}

/// @brief Render an animated menu container with rounded corners
static void RenderMenuContainer(ImDrawList *dl, ImVec2 displaySize, float menuWidth, int numItems, float itemHeight, float animTimer, bool isDark,
                                ImVec2 &menuPos, ImVec2 &menuSize, float &easeOut, float &cornerRadius) {
    float scale = ImGui::GetIO().FontGlobalScale;
    menuSize = ImVec2(menuWidth, numItems * itemHeight);
    float t = std::min(animTimer / 0.4f, 1.0f);
    easeOut = 1.0f - std::pow(1.0f - t, 3.0f);
    float targetY = (displaySize.y - menuSize.y) / 2.0f, startY = displaySize.y + 100.0f * scale;
    menuPos = ImVec2((displaySize.x - menuSize.x) / 2, startY + (targetY - startY) * easeOut);
    cornerRadius = 16.0f * scale;
    ImU32 containerColor = isDark ? IM_COL32(45,45,45,(int)(255*easeOut)) : IM_COL32(242,245,248,(int)(255*easeOut));
    dl->AddRectFilled(menuPos, ImVec2(menuPos.x + menuSize.x, menuPos.y + menuSize.y), containerColor, cornerRadius);
}

static void RenderMenuItem(ImDrawList *dl, ImVec2 menuPos, ImVec2 menuSize, int i, int numItems, float itemHeight,
                           bool isSelected, float cornerRadius, float easeOut, bool isDark, ImFont *font, float fontSize, const char *text) {
    float scale = ImGui::GetIO().FontGlobalScale;
    float itemY = menuPos.y + i * itemHeight;
    ImVec2 itemMin(menuPos.x, itemY), itemMax(menuPos.x + menuSize.x, itemY + itemHeight);
    if (isSelected) UIStyle::DrawSelection(dl, itemMin, itemMax, cornerRadius, easeOut, isDark);
    ImU32 textColor;
    if (isDark) textColor = isSelected ? IM_COL32(255,255,255,(int)(255*easeOut)) : IM_COL32(200,200,200,(int)(255*easeOut));
    else textColor = isSelected ? IM_COL32(60,60,70,(int)(255*easeOut)) : IM_COL32(90,90,100,(int)(255*easeOut));
    ImVec2 sz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text);
    dl->AddText(font, fontSize, ImVec2(itemMin.x + 20.0f * scale, itemMin.y + (itemHeight - sz.y) / 2), textColor, text);
}

void TicoOverlay::RenderQuickMenu(ImDrawList *dl, ImVec2 displaySize) {
    float scale = ImGui::GetIO().FontGlobalScale;
    std::string items[] = {tr("emulator_save_state"), tr("emulator_load_state"), tr("emulator_settings"), tr("emulator_exit_game")};
    const int N = 4; float itemH = 64.0f * scale;
    ImVec2 menuPos, menuSize; float easeOut, cornerRadius;
    RenderMenuContainer(dl, displaySize, 400.0f*scale, N, itemH, m_animTimer, m_isDarkMode, menuPos, menuSize, easeOut, cornerRadius);
    ImFont *font = ImGui::GetFont(); float fs = ImGui::GetFontSize() * 0.85f;
    for (int i = 0; i < N; i++) RenderMenuItem(dl, menuPos, menuSize, i, N, itemH, m_quickMenuSelection==i, cornerRadius, easeOut, m_isDarkMode, font, fs, items[i].c_str());
}

void TicoOverlay::RenderSaveStatesMenu(ImDrawList *dl, ImVec2 displaySize) {
    float scale = ImGui::GetIO().FontGlobalScale;
    const int N = 4; float itemH = 64.0f * scale;
    ImVec2 menuPos, menuSize; float easeOut, cornerRadius;
    RenderMenuContainer(dl, displaySize, 400.0f*scale, N, itemH, m_animTimer, m_isDarkMode, menuPos, menuSize, easeOut, cornerRadius);
    ImFont *font = ImGui::GetFont(); float fs = ImGui::GetFontSize() * 0.85f;
    for (int i = 0; i < N; i++) {
        bool exists = false;
        if (m_core && m_core->IsGameLoaded()) { struct stat buffer; exists = (stat(GetStatePath(m_core, i).c_str(), &buffer) == 0); }
        char slotText[128];
        snprintf(slotText, sizeof(slotText), tr("emulator_slot").c_str(), i+1, exists ? tr("emulator_in_use").c_str() : tr("emulator_empty").c_str());
        RenderMenuItem(dl, menuPos, menuSize, i, N, itemH, m_saveStateSlot==i, cornerRadius, easeOut, m_isDarkMode, font, fs, slotText);
    }
}

void TicoOverlay::RenderSettingsMenu(ImDrawList *dl, ImVec2 displaySize) {
    float scale = ImGui::GetIO().FontGlobalScale;
    const int N = 4; float itemH = 64.0f * scale;
    ImVec2 menuPos, menuSize; float easeOut, cornerRadius;
    RenderMenuContainer(dl, displaySize, 480.0f*scale, N, itemH, m_animTimer, m_isDarkMode, menuPos, menuSize, easeOut, cornerRadius);
    ImFont *font = ImGui::GetFont(); float fs = ImGui::GetFontSize() * 0.85f;
    for (int i = 0; i < N; i++) {
        bool isSelected = (m_settingsSelection == i);
        float itemY = menuPos.y + i * itemH;
        ImVec2 itemMin(menuPos.x, itemY), itemMax(menuPos.x + menuSize.x, itemY + itemH);
        if (isSelected) UIStyle::DrawSelection(dl, itemMin, itemMax, cornerRadius, easeOut, m_isDarkMode);
        std::string label, value;
        if (i == 0) { label = tr("emulator_display_mode"); value = (m_displayMode == GambatteDisplayMode::Integer) ? tr("emulator_integer") : tr("emulator_display"); }
        else if (i == 1) {
            label = tr("emulator_size");
            if (m_displayMode == GambatteDisplayMode::Integer) {
                switch (m_displaySize) { case GambatteDisplaySize::_1x: value="1x"; break; case GambatteDisplaySize::_2x: value="2x"; break; default: value=tr("emulator_auto"); break; }
            } else {
                switch (m_displaySize) { case GambatteDisplaySize::Stretch: value=tr("emulator_stretch"); break; case GambatteDisplaySize::_4_3: value="4:3"; break; case GambatteDisplaySize::_16_9: value="16:9"; break; default: value=tr("emulator_original"); break; }
            }
        } else if (i == 2) {
            label = tr("emulator_shader");
            value = ShaderPresetLabel();
        } else if (i == 3) {
            label = tr("emulator_shader_parameters");
            size_t n = m_chain ? m_chain->Parameters().size() : 0;
            value = n ? std::to_string(n) : tr("emulator_no_parameters");
        }
        ImU32 textColor;
        if (m_isDarkMode) textColor = isSelected ? IM_COL32(255,255,255,(int)(255*easeOut)) : IM_COL32(200,200,200,(int)(255*easeOut));
        else textColor = isSelected ? IM_COL32(60,60,70,(int)(255*easeOut)) : IM_COL32(90,90,100,(int)(255*easeOut));
        float textX = itemMin.x + 20.0f * scale;
        ImVec2 labelSize = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, label.c_str());
        dl->AddText(font, fs, ImVec2(textX, itemMin.y + (itemH - labelSize.y)/2), textColor, label.c_str());
        ImVec2 valueSize = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, value.c_str());
        float valueX = itemMax.x - valueSize.x - 40.0f * scale;
        dl->AddText(font, fs, ImVec2(valueX, itemMin.y + (itemH - labelSize.y)/2), textColor, value.c_str());
        if (isSelected && i < 3) {
            float arrowSize = 12.0f * scale, arrowY = itemMin.y + (itemH - arrowSize)/2;
            float lx = valueX - arrowSize - 12.0f*scale;
            dl->AddTriangleFilled(ImVec2(lx,arrowY+arrowSize/2), ImVec2(lx+arrowSize,arrowY), ImVec2(lx+arrowSize,arrowY+arrowSize), textColor);
            float rx = valueX + valueSize.x + 12.0f*scale;
            dl->AddTriangleFilled(ImVec2(rx+arrowSize,arrowY+arrowSize/2), ImVec2(rx,arrowY), ImVec2(rx,arrowY+arrowSize), textColor);
        }
    }
}

void TicoOverlay::RenderHelpersBar(ImDrawList *dl, ImVec2 displaySize) {
    float t = std::min(m_animTimer / 0.4f, 1.0f);
    float easeOut = 1.0f - std::pow(1.0f - t, 3.0f);
    float scale = ImGui::GetIO().FontGlobalScale;
    float BAR_HEIGHT = 48.0f*scale, MARGIN_BOTTOM = 24.0f*scale, PADDING = 16.0f*scale, BUTTON_SIZE = 22.0f*scale, ITEM_SPACING = 12.0f*scale;
    ImFont *font = ImGui::GetFont(); float fontSize = ImGui::GetFontSize() * 0.78f;
    struct Helper { const char *btn; std::string desc; };
    std::vector<Helper> helpers;
    if (m_currentMenu == OverlayMenu::QuickMenu) helpers.push_back({"-", tr("emulator_reset")});
    else if (m_currentMenu == OverlayMenu::Settings || m_currentMenu == OverlayMenu::ShaderParams) helpers.push_back({"DPAD", tr("emulator_change")});
    helpers.push_back({"B", tr("emulator_back")}); helpers.push_back({"A", tr("emulator_select")});
    float totalWidth = PADDING * 2;
    for (size_t i = 0; i < helpers.size(); i++) {
        totalWidth += BUTTON_SIZE + 8.0f*scale + font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, helpers[i].desc.c_str()).x;
        if (i < helpers.size()-1) totalWidth += ITEM_SPACING;
    }
    float startOffset = 400.0f*scale, currentOffset = startOffset*(1.0f-easeOut);
    float barX = displaySize.x - totalWidth - 20.0f + currentOffset, barY = displaySize.y - MARGIN_BOTTOM - BAR_HEIGHT;
    float cursorX = barX + PADDING, centerY = barY + BAR_HEIGHT * 0.5f;
    for (const auto &h : helpers) {
        UIStyle::DrawSwitchButton(dl, font, fontSize, ImVec2(cursorX+BUTTON_SIZE*0.5f, centerY), BUTTON_SIZE, h.btn, easeOut, true);
        cursorX += BUTTON_SIZE + 8.0f*scale;
        ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, h.desc.c_str());
        dl->AddText(font, fontSize, ImVec2(cursorX, centerY - textSize.y*0.5f), IM_COL32(200,200,200,(int)(255*easeOut)), h.desc.c_str());
        cursorX += textSize.x + ITEM_SPACING;
    }
}

bool TicoOverlay::HandleInput(SDL_GameController *controller) {
    if (!controller) return false;
    uint32_t now = SDL_GetTicks(); bool debounced = (now - m_lastInputTime) > DEBOUNCE_MS;
    bool start = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_START);
    bool select = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_BACK);
    bool guide = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_GUIDE);
    bool togglePressed = guide || (start && select);
    if (togglePressed && !m_toggleHeld && debounced) {
        m_toggleHeld = true; m_lastInputTime = now;
        if (m_currentMenu == OverlayMenu::None) Show();
        else if (m_currentMenu != OverlayMenu::QuickMenu) {
            if (m_currentMenu == OverlayMenu::ShaderParams) LeaveShaderParams();
            m_currentMenu = OverlayMenu::QuickMenu; m_animTimer = 0.4f;
        }
        else Hide();
        return true;
    }
    if (!togglePressed) m_toggleHeld = false;
    if (m_currentMenu == OverlayMenu::None) return false;

    bool up = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_UP);
    bool down = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    bool left = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    bool right = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    bool confirm = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_B);
    bool back = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_A);
    Sint16 axisY = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY);
    Sint16 axisX = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX);
    if (axisY < -16000) up = true; if (axisY > 16000) down = true;
    if (axisX < -16000) left = true; if (axisX > 16000) right = true;

    if (select && !start && debounced && m_currentMenu == OverlayMenu::QuickMenu) { m_shouldReset = true; Hide(); m_lastInputTime = now; return true; }

    if (up && !m_upHeld && debounced) {
        m_upHeld = true; m_lastInputTime = now;
        if (m_currentMenu == OverlayMenu::QuickMenu) m_quickMenuSelection = (m_quickMenuSelection + 3) % 4;
        else if (m_currentMenu == OverlayMenu::SaveStates) m_saveStateSlot = (m_saveStateSlot + 3) % 4;
        else if (m_currentMenu == OverlayMenu::Settings) m_settingsSelection = (m_settingsSelection + 3) % 4;
        else if (m_currentMenu == OverlayMenu::ShaderBrowser && !m_browseEntries.empty())
            m_browseSel = (m_browseSel + (int)m_browseEntries.size() - 1) % (int)m_browseEntries.size();
        else if (m_currentMenu == OverlayMenu::ShaderParams && m_chain)
            m_paramSel = (m_paramSel + (int)m_chain->Parameters().size()) % ((int)m_chain->Parameters().size() + 1);
    }
    if (!up) m_upHeld = false;
    if (down && !m_downHeld && debounced) {
        m_downHeld = true; m_lastInputTime = now;
        if (m_currentMenu == OverlayMenu::QuickMenu) m_quickMenuSelection = (m_quickMenuSelection + 1) % 4;
        else if (m_currentMenu == OverlayMenu::SaveStates) m_saveStateSlot = (m_saveStateSlot + 1) % 4;
        else if (m_currentMenu == OverlayMenu::Settings) m_settingsSelection = (m_settingsSelection + 1) % 4;
        else if (m_currentMenu == OverlayMenu::ShaderBrowser && !m_browseEntries.empty())
            m_browseSel = (m_browseSel + 1) % (int)m_browseEntries.size();
        else if (m_currentMenu == OverlayMenu::ShaderParams && m_chain)
            m_paramSel = (m_paramSel + 1) % ((int)m_chain->Parameters().size() + 1);
    }
    if (!down) m_downHeld = false;

    bool dirChanged = false; int dir = 0;
    if (left && !m_leftHeld && debounced) { m_leftHeld = true; dir = -1; dirChanged = true; m_lastInputTime = now; }
    if (!left) m_leftHeld = false;
    if (right && !m_rightHeld && debounced) { m_rightHeld = true; dir = 1; dirChanged = true; m_lastInputTime = now; }
    if (!right) m_rightHeld = false;

    if (dirChanged && m_currentMenu == OverlayMenu::Settings) {
        if (m_settingsSelection == 0) {
            m_displayMode = (m_displayMode == GambatteDisplayMode::Display) ? GambatteDisplayMode::Integer : GambatteDisplayMode::Display;
            m_displaySize = (m_displayMode == GambatteDisplayMode::Integer) ? GambatteDisplaySize::Auto : GambatteDisplaySize::_4_3;
            ApplyScalingSettings(true);
        } else if (m_settingsSelection == 1) {
            if (m_displayMode == GambatteDisplayMode::Integer) {
                int s = (int)m_displaySize + dir; if (s < 4) s = 6; if (s > 6) s = 4;
                m_displaySize = (GambatteDisplaySize)s;
            } else {
                int s = (int)m_displaySize + dir; if (s < 0) s = 3; if (s > 3) s = 0;
                m_displaySize = (GambatteDisplaySize)s;
            }
            ApplyScalingSettings(true);
        } else if (m_settingsSelection == 2) {
            CycleShaderPreset(dir);
            ApplyScalingSettings(true);
        }
    }
    if (dirChanged && m_currentMenu == OverlayMenu::ShaderParams) AdjustShaderParam(dir);

    if (confirm && !m_confirmHeld && debounced) {
        m_confirmHeld = true; m_lastInputTime = now;
        if (m_currentMenu == OverlayMenu::QuickMenu) {
            switch (m_quickMenuSelection) {
            case 0: m_isSaveMode = true; m_currentMenu = OverlayMenu::SaveStates; break;
            case 1: m_isSaveMode = false; m_currentMenu = OverlayMenu::SaveStates; break;
            case 2: m_currentMenu = OverlayMenu::Settings; m_settingsSelection = 0; break;
            case 3: m_shouldExit = true; break;
            }
        } else if (m_currentMenu == OverlayMenu::SaveStates) {
            if (m_core) {
                std::string sp = GetStatePath(m_core, m_saveStateSlot);
                if (m_isSaveMode) { m_core->SaveState(sp); m_currentMenu = OverlayMenu::QuickMenu; }
                else { m_core->LoadState(sp); Hide(); m_animTimer = 0.4f; return true; }
            } else m_currentMenu = OverlayMenu::QuickMenu;
        } else if (m_currentMenu == OverlayMenu::Settings) {
            if (m_settingsSelection == 0) {
                m_displayMode = (m_displayMode == GambatteDisplayMode::Display) ? GambatteDisplayMode::Integer : GambatteDisplayMode::Display;
                m_displaySize = (m_displayMode == GambatteDisplayMode::Integer) ? GambatteDisplaySize::Auto : GambatteDisplaySize::_4_3;
                ApplyScalingSettings(true);
            } else if (m_settingsSelection == 1) {
                if (m_displayMode == GambatteDisplayMode::Integer) { int s = (int)m_displaySize; s = (s >= 6) ? 4 : s+1; m_displaySize = (GambatteDisplaySize)s; }
                else { int s = (int)m_displaySize; s = (s >= 3) ? 0 : s+1; m_displaySize = (GambatteDisplaySize)s; }
                ApplyScalingSettings(true);
            } else if (m_settingsSelection == 2) {
                OpenShaderBrowser(m_browseDir.empty() ? kUserShaderDir : m_browseDir);
                m_currentMenu = OverlayMenu::ShaderBrowser;
            } else if (m_settingsSelection == 3) {
                if (m_chain && !m_chain->Parameters().empty()) {
                    m_paramSel = 0; m_paramScroll = 0;
                    m_currentMenu = OverlayMenu::ShaderParams;
                }
            }
        } else if (m_currentMenu == OverlayMenu::ShaderBrowser) {
            ActivateBrowseEntry();
        } else if (m_currentMenu == OverlayMenu::ShaderParams) {
            if (m_chain && m_paramSel == (int)m_chain->Parameters().size()) ResetShaderParams();
        }
    }
    if (!confirm) m_confirmHeld = false;

    if (back && !m_backHeld && debounced) {
        m_backHeld = true; m_lastInputTime = now;
        if (m_currentMenu == OverlayMenu::QuickMenu) Hide();
        else if (m_currentMenu == OverlayMenu::ShaderBrowser) {
            if (m_browseDir == kUserShaderDir) m_currentMenu = OverlayMenu::Settings;
            else if (!m_browseEntries.empty() && m_browseEntries[0].label == "..") { m_browseSel = 0; ActivateBrowseEntry(); }
            else m_currentMenu = OverlayMenu::Settings;
        }
        else if (m_currentMenu == OverlayMenu::ShaderParams) { LeaveShaderParams(); m_currentMenu = OverlayMenu::Settings; }
        else m_currentMenu = OverlayMenu::QuickMenu;
    }
    if (!back) m_backHeld = false;
    return true;
}

void TicoOverlay::LoadCoreSettings() {
#ifdef __SWITCH__
    std::string configPath = "sdmc:/tico/config/cores/snes9x.jsonc";
#else
    std::string configPath = "tico/config/cores/snes9x.jsonc";
#endif
    std::ifstream file(configPath);
    if (file.is_open()) {
        auto j = nlohmann::json::parse(file, nullptr, false, true); file.close();
        if (!j.is_discarded()) {
            if (j.contains("display_mode") && j["display_mode"].is_string()) {
                m_displayMode = (j["display_mode"].get<std::string>() == "Integer") ? GambatteDisplayMode::Integer : GambatteDisplayMode::Display;
            } else m_displayMode = GambatteDisplayMode::Display;
            if (j.contains("display_size") && j["display_size"].is_string()) {
                std::string v = j["display_size"].get<std::string>();
                if (v=="Stretch") m_displaySize = GambatteDisplaySize::Stretch;
                else if (v=="16:9") m_displaySize = GambatteDisplaySize::_16_9;
                else if (v=="Original") m_displaySize = GambatteDisplaySize::Original;
                else if (v=="1x") m_displaySize = GambatteDisplaySize::_1x;
                else if (v=="2x") m_displaySize = GambatteDisplaySize::_2x;
                else if (v=="Auto") m_displaySize = GambatteDisplaySize::Auto;
                else m_displaySize = GambatteDisplaySize::_4_3;
            } else m_displaySize = GambatteDisplaySize::_4_3;
            m_shaderParams.clear();
            if (j.contains("shader_parameters") && j["shader_parameters"].is_object()) {
                for (auto &preset : j["shader_parameters"].items()) {
                    if (!preset.value().is_object()) continue;
                    for (auto &param : preset.value().items())
                        if (param.value().is_number())
                            m_shaderParams[preset.key()][param.key()] = param.value().get<float>();
                }
            }
            m_shaderPreset.clear();
            if (j.contains("shader_preset") && j["shader_preset"].is_string()) {
                m_shaderPreset = j["shader_preset"].get<std::string>();
            } else if (j.contains("shader_type") && j["shader_type"].is_string()) {
                // Settings from before slang presets named one of three built-ins.
                std::string v = j["shader_type"].get<std::string>();
                if (v=="xBRZ") m_shaderPreset = kBuiltinShaderDir + std::string("xbrz.slangp");
                else if (v=="Eagle") m_shaderPreset = kBuiltinShaderDir + std::string("eagle.slangp");
                else if (v=="CrtEasyMode") m_shaderPreset = kBuiltinShaderDir + std::string("crt-easymode.slangp");
            }
        } else { m_displayMode = GambatteDisplayMode::Display; m_displaySize = GambatteDisplaySize::_4_3; }
    } else { m_displayMode = GambatteDisplayMode::Display; m_displaySize = GambatteDisplaySize::_4_3; }
    ApplyScalingSettings(false);
}

void TicoOverlay::SaveCoreSettings() {
#ifdef __SWITCH__
    std::string configPath = "sdmc:/tico/config/cores/snes9x.jsonc";
#else
    std::string configPath = "tico/config/cores/snes9x.jsonc";
#endif
    nlohmann::json j = nlohmann::json::object();
    { std::ifstream in(configPath); if (in.is_open()) { auto p = nlohmann::json::parse(in, nullptr, false, true); in.close(); if (!p.is_discarded()) j = p; } }
    j["display_mode"] = (m_displayMode == GambatteDisplayMode::Integer) ? "Integer" : "Display";
    const char *sizeStr = "4:3";
    switch (m_displaySize) {
    case GambatteDisplaySize::Stretch: sizeStr="Stretch"; break; case GambatteDisplaySize::_4_3: sizeStr="4:3"; break;
    case GambatteDisplaySize::_16_9: sizeStr="16:9"; break; case GambatteDisplaySize::Original: sizeStr="Original"; break;
    case GambatteDisplaySize::_1x: sizeStr="1x"; break; case GambatteDisplaySize::_2x: sizeStr="2x"; break;
    case GambatteDisplaySize::Auto: sizeStr="Auto"; break; default: break;
    }
    j["display_size"] = sizeStr;
    j["shader_preset"] = m_shaderPreset;
    j.erase("shader_type");
    nlohmann::json params = nlohmann::json::object();
    for (const auto &preset : m_shaderParams)
        if (!preset.second.empty())
            for (const auto &param : preset.second)
                params[preset.first][param.first] = param.second;
    j["shader_parameters"] = params;
    std::ofstream out(configPath); if (out.is_open()) { out << j.dump(4); out.close(); }
}

void TicoOverlay::ApplyScalingSettings(bool save) { if (save) SaveCoreSettings(); }

void TicoOverlay::ScanShaderPresets() {
    m_shaderPresets.clear();
    m_shaderPresets.push_back("");
    for (const char *name : {"xbrz.slangp", "eagle.slangp", "crt-easymode.slangp"})
        m_shaderPresets.push_back(kBuiltinShaderDir + std::string(name));
    std::vector<std::string> user;
    if (DIR *dir = opendir(kUserShaderDir)) {
        while (struct dirent *e = readdir(dir)) {
            std::string n = e->d_name;
            if (n.size() > 7 && n.compare(n.size() - 7, 7, ".slangp") == 0)
                user.push_back(kUserShaderDir + n);
        }
        closedir(dir);
    }
    std::sort(user.begin(), user.end());
    m_shaderPresets.insert(m_shaderPresets.end(), user.begin(), user.end());
}

void TicoOverlay::CycleShaderPreset(int dir) {
    ScanShaderPresets();
    auto it = std::find(m_shaderPresets.begin(), m_shaderPresets.end(), m_shaderPreset);
    int i = it == m_shaderPresets.end() ? 0 : (int)(it - m_shaderPresets.begin());
    int n = (int)m_shaderPresets.size();
    m_shaderPreset = m_shaderPresets[((i + dir) % n + n) % n];
}

static bool EndsWith(const std::string &s, const char *suffix) {
    size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

void TicoOverlay::OpenShaderBrowser(const std::string &dir) {
    m_browseDir = dir;
    if (m_browseDir.empty() || m_browseDir.back() != '/') m_browseDir += '/';
    m_browseEntries.clear();
    m_browseSel = 0; m_browseScroll = 0;

    if (m_browseDir == kUserShaderDir)
        m_browseEntries.push_back({tr("emulator_builtin_shaders"), kBuiltinShaderDir, true});
    else {
        std::string parent;
        if (m_browseDir == kBuiltinShaderDir) parent = kUserShaderDir;
        else {
            std::string d = m_browseDir.substr(0, m_browseDir.size() - 1);
            size_t slash = d.find_last_of('/');
            parent = slash == std::string::npos ? kUserShaderDir : d.substr(0, slash + 1);
        }
        m_browseEntries.push_back({"..", parent, true});
    }

    std::vector<BrowseEntry> dirs, files;
    if (DIR *d = opendir(m_browseDir.c_str())) {
        while (struct dirent *e = readdir(d)) {
            std::string name = e->d_name;
            if (name.empty() || name[0] == '.') continue;
            std::string path = m_browseDir + name;
            bool isDir = e->d_type == DT_DIR;
            if (e->d_type == DT_UNKNOWN) { struct stat st; isDir = stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
            if (isDir) dirs.push_back({name + "/", path + "/", true});
            else if (EndsWith(name, ".slangp")) files.push_back({name.substr(0, name.size() - 7), path, false});
        }
        closedir(d);
    }
    auto byName = [](const BrowseEntry &a, const BrowseEntry &b) {
        return strcasecmp(a.label.c_str(), b.label.c_str()) < 0;
    };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    m_browseEntries.insert(m_browseEntries.end(), dirs.begin(), dirs.end());
    m_browseEntries.insert(m_browseEntries.end(), files.begin(), files.end());

    // Start on the active preset when it is in this folder.
    for (size_t i = 0; i < m_browseEntries.size(); i++)
        if (!m_browseEntries[i].isDir && m_browseEntries[i].path == m_shaderPreset) m_browseSel = (int)i;
}

void TicoOverlay::ActivateBrowseEntry() {
    if (m_browseSel < 0 || m_browseSel >= (int)m_browseEntries.size()) return;
    BrowseEntry entry = m_browseEntries[m_browseSel];
    if (entry.isDir) {
        std::string from = m_browseDir;
        OpenShaderBrowser(entry.path);
        // Going up lands on the folder we came from.
        for (size_t i = 0; i < m_browseEntries.size(); i++)
            if (m_browseEntries[i].path == from) m_browseSel = (int)i;
        return;
    }
    m_shaderPreset = entry.path;
    ApplyScalingSettings(true);
    m_currentMenu = OverlayMenu::Settings;
}

void TicoOverlay::OnShaderLoaded() {
    if (!m_chain) return;
    m_chain->ResetParameters();
    auto it = m_shaderParams.find(m_shaderPreset);
    if (it == m_shaderParams.end()) return;
    for (const auto &param : it->second) m_chain->SetParameter(param.first, param.second);
}

void TicoOverlay::AdjustShaderParam(int dir) {
    if (!m_chain) return;
    const auto &params = m_chain->Parameters();
    if (m_paramSel < 0 || m_paramSel >= (int)params.size()) return;
    const TicoSlang::Parameter &p = params[m_paramSel];
    float step = p.step > 0.0f ? p.step : 0.01f;
    // Snap to the step grid so repeated presses don't accumulate float error.
    float v = p.minimum + std::round((p.value + dir * step - p.minimum) / step) * step;
    v = std::clamp(v, p.minimum, p.maximum);
    m_chain->SetParameter(p.id, v);
    if (std::fabs(v - p.initial) < step * 0.5f) m_shaderParams[m_shaderPreset].erase(p.id);
    else m_shaderParams[m_shaderPreset][p.id] = v;
    m_paramsDirty = true;
}

void TicoOverlay::ResetShaderParams() {
    if (m_chain) m_chain->ResetParameters();
    m_shaderParams.erase(m_shaderPreset);
    m_paramsDirty = true;
}

void TicoOverlay::LeaveShaderParams() {
    if (m_paramsDirty) SaveCoreSettings();
    m_paramsDirty = false;
}

void TicoOverlay::RenderScrollList(ImDrawList *dl, ImVec2 displaySize, const std::string &title,
                                   const std::vector<ListRow> &rows, int selection, int &scroll, bool arrows) {
    float scale = ImGui::GetIO().FontGlobalScale;
    const int visible = std::max(1, std::min((int)rows.size(), 8));
    if (selection < scroll) scroll = selection;
    if (selection >= scroll + visible) scroll = selection - visible + 1;
    scroll = std::clamp(scroll, 0, std::max(0, (int)rows.size() - visible));

    float itemH = 56.0f * scale;
    ImVec2 menuPos, menuSize; float easeOut, cornerRadius;
    RenderMenuContainer(dl, displaySize, 720.0f * scale, visible, itemH, m_animTimer, m_isDarkMode, menuPos, menuSize, easeOut, cornerRadius);
    ImFont *font = ImGui::GetFont(); float fs = ImGui::GetFontSize() * 0.8f;
    ImU32 titleCol = IM_COL32(230, 230, 230, (int)(255 * easeOut));
    ImVec2 titleSize = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, title.c_str());
    dl->AddText(font, fs, ImVec2(menuPos.x + 8.0f * scale, menuPos.y - titleSize.y - 10.0f * scale), titleCol, title.c_str());

    dl->PushClipRect(menuPos, menuPos + menuSize, true);
    for (int r = 0; r < visible; r++) {
        int i = scroll + r;
        if (i >= (int)rows.size()) break;
        bool isSelected = i == selection;
        ImVec2 itemMin(menuPos.x, menuPos.y + r * itemH), itemMax(menuPos.x + menuSize.x, menuPos.y + (r + 1) * itemH);
        if (isSelected) UIStyle::DrawSelection(dl, itemMin, itemMax, cornerRadius, easeOut, m_isDarkMode);
        ImU32 textColor = m_isDarkMode
            ? (isSelected ? IM_COL32(255,255,255,(int)(255*easeOut)) : IM_COL32(200,200,200,(int)(255*easeOut)))
            : (isSelected ? IM_COL32(60,60,70,(int)(255*easeOut)) : IM_COL32(90,90,100,(int)(255*easeOut)));
        const ListRow &row = rows[i];
        ImVec2 valueSize = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, row.value.c_str());
        float valueX = itemMax.x - valueSize.x - (arrows ? 40.0f : 20.0f) * scale;
        float textY = itemMin.y + (itemH - valueSize.y) / 2;
        dl->PushClipRect(itemMin, ImVec2(valueX - 16.0f * scale, itemMax.y), true);
        dl->AddText(font, fs, ImVec2(itemMin.x + 20.0f * scale, textY), textColor, row.label.c_str());
        dl->PopClipRect();
        dl->AddText(font, fs, ImVec2(valueX, textY), textColor, row.value.c_str());
        if (isSelected && arrows && !row.value.empty()) {
            float a = 10.0f * scale, ay = itemMin.y + (itemH - a) / 2;
            float lx = valueX - a - 10.0f * scale, rx = valueX + valueSize.x + 10.0f * scale;
            dl->AddTriangleFilled(ImVec2(lx, ay + a/2), ImVec2(lx + a, ay), ImVec2(lx + a, ay + a), textColor);
            dl->AddTriangleFilled(ImVec2(rx + a, ay + a/2), ImVec2(rx, ay), ImVec2(rx, ay + a), textColor);
        }
    }
    dl->PopClipRect();

    if ((int)rows.size() > visible) {
        float trackX = menuPos.x + menuSize.x + 8.0f * scale;
        float thumbH = menuSize.y * visible / rows.size();
        float thumbY = menuPos.y + (menuSize.y - thumbH) * scroll / (float)(rows.size() - visible);
        dl->AddRectFilled(ImVec2(trackX, thumbY), ImVec2(trackX + 4.0f * scale, thumbY + thumbH),
                          IM_COL32(200, 200, 200, (int)(160 * easeOut)), 2.0f * scale);
    }
}

void TicoOverlay::RenderShaderBrowser(ImDrawList *dl, ImVec2 displaySize) {
    std::vector<ListRow> rows;
    for (const BrowseEntry &e : m_browseEntries)
        rows.push_back({e.label, (!e.isDir && e.path == m_shaderPreset) ? "*" : ""});
    if (rows.size() <= 1 && m_browseDir == kUserShaderDir)
        rows.push_back({tr("emulator_no_shaders"), ""});
    std::string title = m_browseDir == kUserShaderDir ? tr("emulator_shader") : m_browseDir;
    RenderScrollList(dl, displaySize, title, rows, m_browseSel, m_browseScroll, false);
}

void TicoOverlay::RenderShaderParams(ImDrawList *dl, ImVec2 displaySize) {
    std::vector<ListRow> rows;
    if (m_chain) {
        for (const TicoSlang::Parameter &p : m_chain->Parameters()) {
            char buf[32];
            bool whole = p.step >= 1.0f && std::fabs(p.value - std::round(p.value)) < 1e-4f;
            snprintf(buf, sizeof(buf), whole ? "%.0f" : "%.2f", p.value);
            rows.push_back({p.description.empty() ? p.id : p.description, buf});
        }
    }
    rows.push_back({tr("emulator_reset_parameters"), ""});
    RenderScrollList(dl, displaySize, tr("emulator_shader_parameters"), rows, m_paramSel, m_paramScroll, true);
}

std::string TicoOverlay::ShaderPresetLabel() const {
    if (m_shaderPreset.empty()) return "None";
    if (m_shaderPreset == kBuiltinShaderDir + std::string("xbrz.slangp")) return "xBRZ";
    if (m_shaderPreset == kBuiltinShaderDir + std::string("eagle.slangp")) return "Eagle";
    if (m_shaderPreset == kBuiltinShaderDir + std::string("crt-easymode.slangp")) return "CRT Easy Mode";
    std::string name = m_shaderPreset;
    size_t slash = name.find_last_of('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    if (name.size() > 7) name = name.substr(0, name.size() - 7);
    return name;
}

void TicoOverlay::LoadSVGIcon() {
    const char *svgContent = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 448 512"><path fill="#FFFFFF" d="M338.8-9.9c11.9 8.6 16.3 24.2 10.9 37.8L271.3 224 416 224c13.5 0 25.5 8.4 30.1 21.1s.7 26.9-9.6 35.5l-288 240c-11.3 9.4-27.4 9.9-39.3 1.3s-16.3-24.2-10.9-37.8L176.7 288 32 288c-13.5 0-25.5-8.4-30.1-21.1s-.7-26.9 9.6-35.5l288-240c11.3-9.4 27.4-9.9 39.3-1.3z"/></svg>)";
    char *input = strdup(svgContent); if (!input) return;
    NSVGimage *image = nsvgParse(input, "px", 96); free(input); if (!image) return;
    float sc = 64.0f / image->height; int w = (int)(image->width * sc), h = (int)(image->height * sc);
    m_boltWidth = w; m_boltHeight = h;
    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) { nsvgDelete(image); return; }
    unsigned char *img = (unsigned char *)malloc(w * h * 4);
    if (!img) { nsvgDeleteRasterizer(rast); nsvgDelete(image); return; }
    nsvgRasterize(rast, image, 0, 0, sc, img, w, h, w * 4);
    TicoVulkan::DestroyTexture(m_boltTexture);
    m_boltTexture = TicoVulkan::CreateTextureRGBA(img, w, h);
    free(img); nsvgDeleteRasterizer(rast); nsvgDelete(image);
}

void TicoOverlay::RenderStatusBar(ImDrawList *dl, ImVec2 displaySize) {
    if (m_animTimer <= 0.0f) return;
    float t = std::min(m_animTimer / 0.4f, 1.0f);
    float ease = 1.0f - std::pow(1.0f - t, 3.0f), alpha = ease;
    float scale = ImGui::GetIO().FontGlobalScale;
    float BAR_HEIGHT = 50.0f*scale, TOP_MARGIN = 32.0f*scale, SIDE_MARGIN = 32.0f*scale, ITEM_SPACING = 20.0f*scale;
    ImFont *font = ImGui::GetFont(); float fontSize = ImGui::GetFontSize();
    std::time_t now = std::time(nullptr); std::tm *lt = std::localtime(&now);
    char timeStr[16], periodStr[16]; bool is24h = (m_hourFormat == "24h");
    float timeW = 0, periodFontSize = fontSize * 0.55f, periodW = 0;
    if (is24h) { std::strftime(timeStr, sizeof(timeStr), "%H:%M", lt); timeW = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, timeStr).x; periodStr[0]=0; }
    else { std::strftime(timeStr, sizeof(timeStr), "%I:%M", lt); std::strftime(periodStr, sizeof(periodStr), "%p", lt); timeW = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, timeStr).x; periodW = font->CalcTextSizeA(periodFontSize, FLT_MAX, 0.0f, periodStr).x; }
    float totalWidth = timeW; if (!is24h) totalWidth += 4.0f + periodW;
    totalWidth += ITEM_SPACING + 34.0f*scale; float PADDING = 20.0f*scale; totalWidth += PADDING*2;
    float offsetY = (1.0f - ease) * -20.0f;
    float barX = displaySize.x - totalWidth - SIDE_MARGIN, barY = TOP_MARGIN + offsetY;
    ImU32 textColor = IM_COL32(200,200,200,(int)(255*alpha));
    float cursorX = barX + PADDING, centerY = barY + BAR_HEIGHT * 0.5f;
    dl->AddText(font, fontSize, ImVec2(cursorX, centerY - fontSize*0.5f), textColor, timeStr); cursorX += timeW;
    if (!is24h) { cursorX += 4.0f*scale; dl->AddText(font, periodFontSize, ImVec2(cursorX, centerY - fontSize*0.5f + (fontSize-periodFontSize)*0.9f), textColor, periodStr); cursorX += periodW; }
    cursorX += ITEM_SPACING;
    float bodyW = 32.0f*scale, bodyH = 20.0f*scale, tipW = 4.0f*scale, tipH = 10.0f*scale;
    ImVec2 batteryPos(cursorX, centerY - bodyH*0.5f);
    ImVec2 bodyMin = batteryPos, bodyMax = bodyMin + ImVec2(bodyW, bodyH);
    dl->AddRect(bodyMin, bodyMax, textColor, 3.0f, 0, 2.0f);
    ImVec2 tipMin(bodyMax.x, batteryPos.y + (bodyH-tipH)*0.5f), tipMax = tipMin + ImVec2(tipW, tipH);
    dl->AddRectFilled(tipMin, tipMax, textColor, 2.0f, ImDrawFlags_RoundCornersRight);
    float pct = std::clamp(m_batteryLevel / 100.0f, 0.0f, 1.0f);
    float pad = 4.0f*scale, fillMaxW = bodyW - pad*2, currentFillW = fillMaxW * pct;
    if (currentFillW < 2.0f*scale && pct > 0) currentFillW = 2.0f*scale;
    if (currentFillW > 0) dl->AddRectFilled(bodyMin + ImVec2(pad,pad), bodyMin + ImVec2(pad+currentFillW, bodyH-pad), textColor, 1.0f);
    if (m_isCharging) {
        if (m_boltTexture == ImTextureID_Invalid) LoadSVGIcon();
        if (m_boltTexture != ImTextureID_Invalid) {
            float iconH = 16.0f*scale, iconW = iconH * ((float)m_boltWidth / (float)m_boltHeight);
            ImVec2 iconPos(tipMax.x + 6.0f*scale, batteryPos.y + (bodyH - iconH)*0.5f);
            float fadeProgress = std::max(0.0f, (m_chargingStateProgress - 0.5f) * 2.0f);
            int alphaBolt = (int)(255 * fadeProgress * ease);
            if (alphaBolt > 0) dl->AddImage(m_boltTexture, iconPos, iconPos + ImVec2(iconW,iconH), ImVec2(0,0), ImVec2(1,1), IM_COL32(235,235,235,alphaBolt));
        }
    }
}
