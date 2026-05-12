// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TRANSPORTER_TUI_COMPONENTS_TOAST_HPP
#define TRANSPORTER_TUI_COMPONENTS_TOAST_HPP

#include <chrono>
#include <string>

struct ncplane;

namespace transporter::tui::components {

// Ephemeral bottom banner that fades after a short visibility window.
// Used on track change to surface a "▶ Now playing: <title> — <artist>"
// line just above the persistent player bar.
class Toast {
public:
    static constexpr std::chrono::milliseconds kHold{1500};
    static constexpr std::chrono::milliseconds kFade{500};
    static constexpr std::chrono::milliseconds kTotal = kHold + kFade;

    // Trigger the toast with `text`. `now` is the steady-clock moment used
    // to time the fade.
    void trigger(std::string text, std::chrono::steady_clock::time_point now);

    // True while the toast is still within its display window (hold + fade).
    bool visible(std::chrono::steady_clock::time_point now) const noexcept;

    // Draw at row `y` across `cols` cells. `now` drives the dimming during
    // the fade portion. A no-op when not visible.
    void draw(struct ncplane* host, int y, int cols,
              std::chrono::steady_clock::time_point now) const;

    const std::string& text() const noexcept { return text_; }

private:
    std::string                                text_;
    std::chrono::steady_clock::time_point      triggered_{};
};

} // namespace transporter::tui::components

#endif
