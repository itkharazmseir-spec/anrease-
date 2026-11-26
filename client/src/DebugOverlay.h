#pragma once
#include "LocalPlayer.h"

namespace ao {

// On-screen debug text drawn with the game's own font renderer (CFont).
// Toggle with F9.
class DebugOverlay {
public:
    void HandleInput();                          // game thread, once per frame
    void Draw(const PlayerSnapshot& snap) const; // inside Events::drawingEvent

    bool IsVisible() const { return m_visible; }

private:
    bool m_visible     = true;
    bool m_keyWasDown  = false;
};

} // namespace ao
