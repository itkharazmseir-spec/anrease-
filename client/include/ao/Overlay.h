#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ao {

// A lightweight, always-on-top overlay window, independent of the game's own
// renderer. We have no verified hook into the game's drawing code on this
// RGL build (see DebugOverlay.h/.cpp - it needs drawingEvent, which isn't
// found yet), so this uses plain Win32 + GDI on its own thread instead. It
// works regardless of whether a drawingEvent address is ever found, and
// needs zero game-engine addresses.
//
// Controls (polled globally with GetAsyncKeyState, since the window itself
// is WS_EX_TRANSPARENT/no-activate and deliberately never takes keyboard
// focus away from the game):
//   F10     - show/hide the panel
//   T       - open the chat input line
//   Enter   - send the typed line (sent via the callback set with
//             SetSendCallback; the line itself is not echoed locally - it
//             shows up once the server broadcasts it back, same as for
//             every other player)
//   Escape  - cancel typing without sending
//   Backspace, letters, digits, space - edit the typed line
class Overlay {
public:
    Overlay();
    ~Overlay();

    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    // Spawns the window + its message-loop thread. Safe to call only once.
    // Never fatal if it fails for some reason (missing desktop, etc.) - it
    // logs and the overlay is simply never visible; nothing else in the
    // plugin depends on it.
    void Start();
    void Stop();

    // Thread-safe: called from the network polling thread.
    void SetStatus(const std::string& line);
    void SetPlayers(std::vector<std::pair<uint32_t, std::string>> players);
    void AddChatLine(const std::string& line);

    // Invoked on the overlay's own thread when the user presses Enter with a
    // non-empty typed line. Set this before Start().
    void SetSendCallback(std::function<void(const std::string&)> cb);

    // Implementation detail (Pimpl), public only so the free functions that
    // implement the window thread in Overlay.cpp can name the type - nothing
    // outside Overlay.cpp should ever reference ao::Overlay::Impl.
    struct Impl;

private:
    Impl* m_impl = nullptr;
};

} // namespace ao
