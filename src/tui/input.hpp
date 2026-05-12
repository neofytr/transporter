// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TRANSPORTER_TUI_INPUT_HPP
#define TRANSPORTER_TUI_INPUT_HPP

#include <cstdint>
#include <string>

struct ncinput;

namespace transporter::tui {

enum class PageId : int;

enum class Mode : std::uint8_t {
    Normal,
    Command,   // ':' prefix → command palette
    Search,    // '/' prefix → search overlay
};

enum class Command : std::uint8_t {
    None,
    Quit,
    Help,
    PageGoto,           // payload.page_target
    PageNext,
    PagePrev,
    MoveUp,             // payload.count
    MoveDown,
    MoveLeft,
    MoveRight,
    GotoTop,            // gg
    GotoBottom,         // G
    PlayPause,
    Activate,           // Enter on body
    OpenCommandPalette,
    OpenSearch,
    Cancel,             // ESC: close transient mode
    SubmitInput,        // Enter in transient mode
    InputChar,          // payload.utf8 byte appended in transient mode
    InputBackspace,
    ScrollUp,
    ScrollDown,
    MouseClick,
    QueueAppend,        // 'a' — append selected to queue
    QueuePlayNext,      // 'A' — play selected next (after current)
    NextTrack,          // 'n' — next track in queue
    PrevTrack,          // ',' / '<' — prev track in queue
};

struct CommandResult {
    Command cmd = Command::None;
    int count = 1;                 // motion repeat count
    PageId page_target = static_cast<PageId>(0);
    char utf8[5] = {0, 0, 0, 0, 0};  // for InputChar
    int y = -1;                    // mouse y (cells)
    int x = -1;                    // mouse x (cells)
};

// Stateful keymap. Holds the count-prefix accumulator, the pending 'g'
// (for 'gg'), and the current Mode + transient buffer (for ':' / '/').
class InputMap {
public:
    Mode mode() const noexcept { return mode_; }
    const std::string& buffer() const noexcept { return buffer_; }

    // Consume one ncinput. Returns the command to dispatch (Command::None
    // when the key was absorbed into state — e.g. building a count prefix
    // or the first 'g' of 'gg').
    CommandResult handle(const ncinput& ni);

    // Reset transient mode (used when a command was just executed).
    void clear_transient();

private:
    Mode mode_ = Mode::Normal;
    int pending_count_ = 0;       // numeric prefix accumulator
    bool pending_g_ = false;      // saw first 'g' of 'gg'
    std::string buffer_;          // : or / input buffer

    CommandResult handle_normal(const ncinput& ni);
    CommandResult handle_transient(const ncinput& ni);
};

} // namespace transporter::tui

#endif
