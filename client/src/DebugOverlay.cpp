#include "DebugOverlay.h"

#include "ao/Version.h"

#include "plugin.h"
#include "CFont.h"
#include "CMenuManager.h"   // FrontEndMenuManager

#include <cmath>
#include <cstdio>

namespace ao {
namespace {

constexpr unsigned int kToggleKey = VK_F9;

// Layout is expressed in the game's classic 640x448 virtual screen and scaled
// to the real resolution, like the original HUD does.
constexpr float kBaseW      = 640.0f;
constexpr float kBaseH      = 448.0f;
constexpr float kLeft       = 8.0f;
constexpr float kTop        = 140.0f;
constexpr float kLineHeight = 11.0f;
constexpr float kScaleX     = 0.24f;
constexpr float kScaleY     = 0.80f;

constexpr float kRadToDeg = 57.29577951308232f;

void SetupFont() {
    const float sx = SCREEN_WIDTH  / kBaseW;
    const float sy = SCREEN_HEIGHT / kBaseH;

    CFont::SetFontStyle(FONT_SUBTITLES);
    CFont::SetOrientation(ALIGN_LEFT);
    CFont::SetProportional(true);
    CFont::SetJustify(false);
    CFont::SetBackground(false, false);
    CFont::SetWrapx(SCREEN_WIDTH);
    CFont::SetScale(kScaleX * sx, kScaleY * sy);
    CFont::SetEdge(1);
    CFont::SetDropColor(CRGBA(0, 0, 0, 255));
    CFont::SetColor(CRGBA(255, 255, 255, 255));
}

void Line(int index, const char* text, CRGBA color = CRGBA(255, 255, 255, 255)) {
    const float sx = SCREEN_WIDTH  / kBaseW;
    const float sy = SCREEN_HEIGHT / kBaseH;
    CFont::SetColor(color);
    CFont::PrintString(kLeft * sx, (kTop + index * kLineHeight) * sy, text);
}

} // namespace

void DebugOverlay::HandleInput() {
    // plugin::KeyPressed uses GetKeyState -> only reacts while the game window
    // has focus, which is what we want.
    const bool down = plugin::KeyPressed(kToggleKey);
    if (down && !m_keyWasDown)
        m_visible = !m_visible;
    m_keyWasDown = down;
}

void DebugOverlay::Draw(const PlayerSnapshot& snap) const {
    if (!m_visible || FrontEndMenuManager.m_bMenuActive)
        return;

    SetupFont();

    char buf[128];
    int row = 0;

    std::snprintf(buf, sizeof(buf), "%s %s  (proto %u)  [F9]",
                  kProductName, kVersionString, static_cast<unsigned>(kProtocolVersion));
    Line(row++, buf, CRGBA(255, 200, 60, 255));

    if (!snap.valid) {
        Line(row++, "player: not available", CRGBA(255, 80, 80, 255));
        return;
    }

    std::snprintf(buf, sizeof(buf), "X: %.3f", snap.x);  Line(row++, buf);
    std::snprintf(buf, sizeof(buf), "Y: %.3f", snap.y);  Line(row++, buf);
    std::snprintf(buf, sizeof(buf), "Z: %.3f", snap.z);  Line(row++, buf);

    float deg = snap.headingRad * kRadToDeg;
    deg = std::fmod(deg + 360.0f, 360.0f);
    std::snprintf(buf, sizeof(buf), "Heading: %.1f deg", deg);
    Line(row++, buf);

    std::snprintf(buf, sizeof(buf), "Interior: %d", snap.interior);
    Line(row++, buf);

    if (snap.inVehicle)
        std::snprintf(buf, sizeof(buf), "In vehicle: yes (model %d)", snap.vehicleModel);
    else
        std::snprintf(buf, sizeof(buf), "In vehicle: no");
    Line(row++, buf);

    std::snprintf(buf, sizeof(buf), "HP: %.0f  Armour: %.0f", snap.health, snap.armour);
    Line(row++, buf);
}

} // namespace ao
