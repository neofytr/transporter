// SPDX-License-Identifier: GPL-3.0-or-later

#include "input.hpp"

#include "app.hpp"

#include <notcurses/notcurses.h>

#include <cstring>

namespace transporter::tui {

namespace {

bool is_digit_1_to_9(std::uint32_t k) {
    return k >= '1' && k <= '9';
}

bool is_digit_any(std::uint32_t k) {
    return k >= '0' && k <= '9';
}

bool is_mouse(std::uint32_t k) {
    return k >= NCKEY_MOTION && k <= NCKEY_BUTTON11;
}

} // namespace

CommandResult InputMap::handle(const ncinput& ni) {
    if (mode_ == Mode::Normal) {
        return handle_normal(ni);
    }
    return handle_transient(ni);
}

CommandResult InputMap::handle_normal(const ncinput& ni) {
    CommandResult r;
    const std::uint32_t id = ni.id;

    // Mouse pass-through. Scroll wheel maps to ScrollUp/Down; button1 click
    // becomes MouseClick with coordinates. Other buttons are no-ops here.
    if (is_mouse(id)) {
        r.y = ni.y;
        r.x = ni.x;
        if (id == NCKEY_BUTTON4) {
            r.cmd = Command::ScrollUp;
            return r;
        }
        if (id == NCKEY_BUTTON5) {
            r.cmd = Command::ScrollDown;
            return r;
        }
        if (id == NCKEY_BUTTON1 && ni.evtype != NCTYPE_RELEASE) {
            r.cmd = Command::MouseClick;
            return r;
        }
        return r;
    }

    // Ctrl-C quits regardless of count / pending state.
    if (id == 'c' && (ni.modifiers & NCKEY_MOD_CTRL) != 0) {
        r.cmd = Command::Quit;
        return r;
    }

    // Page-number switches. '1'-'5' goto, but only when no count is being
    // built (i.e. first digit, and 6-9 / 0 are treated as count digits).
    if (id >= '1' && id <= '5' && pending_count_ == 0) {
        r.cmd = Command::PageGoto;
        r.page_target = static_cast<PageId>(static_cast<int>(id - '0'));
        pending_g_ = false;
        return r;
    }

    // Count prefix: digits 0-9 accumulate (leading 0 ignored to avoid 0j).
    if (is_digit_any(id) && (pending_count_ > 0 || is_digit_1_to_9(id))) {
        pending_count_ = pending_count_ * 10 + static_cast<int>(id - '0');
        if (pending_count_ > 9999) {
            pending_count_ = 9999;
        }
        pending_g_ = false;
        return r;
    }

    const int count = pending_count_ > 0 ? pending_count_ : 1;
    auto consume_count = [&]() {
        r.count = count;
        pending_count_ = 0;
        pending_g_ = false;
    };

    switch (id) {
    case NCKEY_TAB:
        r.cmd = Command::PageNext;
        consume_count();
        return r;
    case 'h':
    case NCKEY_LEFT:
        r.cmd = Command::MoveLeft;
        consume_count();
        return r;
    case 'l':
    case NCKEY_RIGHT:
        r.cmd = Command::MoveRight;
        consume_count();
        return r;
    case 'j':
    case NCKEY_DOWN:
        r.cmd = Command::MoveDown;
        consume_count();
        return r;
    case 'k':
    case NCKEY_UP:
        r.cmd = Command::MoveUp;
        consume_count();
        return r;
    case 'g':
        if (pending_g_) {
            r.cmd = Command::GotoTop;
            consume_count();
        } else {
            pending_g_ = true;
        }
        return r;
    case 'G':
        r.cmd = Command::GotoBottom;
        consume_count();
        return r;
    case ':':
        mode_ = Mode::Command;
        buffer_.clear();
        r.cmd = Command::OpenCommandPalette;
        pending_g_ = false;
        return r;
    case '/':
        mode_ = Mode::Search;
        buffer_.clear();
        r.cmd = Command::OpenSearch;
        pending_g_ = false;
        return r;
    case '?':
        r.cmd = Command::Help;
        pending_g_ = false;
        return r;
    case NCKEY_SPACE:
        r.cmd = Command::PlayPause;
        consume_count();
        return r;
    case NCKEY_ENTER:
        r.cmd = Command::Activate;
        pending_g_ = false;
        return r;
    case 'q':
    case 'Q':
        r.cmd = Command::Quit;
        return r;
    default:
        break;
    }

    // Shift-Tab. notcurses delivers Tab with the Shift modifier set.
    if (id == NCKEY_TAB && (ni.modifiers & NCKEY_MOD_SHIFT) != 0) {
        r.cmd = Command::PagePrev;
        consume_count();
        return r;
    }

    // Unrecognised key: drop any pending state to avoid sticky 'g' or count.
    pending_g_ = false;
    pending_count_ = 0;
    return r;
}

CommandResult InputMap::handle_transient(const ncinput& ni) {
    CommandResult r;
    const std::uint32_t id = ni.id;

    if (id == NCKEY_ESC
        || (id == 'c' && (ni.modifiers & NCKEY_MOD_CTRL) != 0)) {
        mode_ = Mode::Normal;
        buffer_.clear();
        r.cmd = Command::Cancel;
        return r;
    }
    if (id == NCKEY_ENTER) {
        r.cmd = Command::SubmitInput;
        return r;
    }
    if (id == NCKEY_BACKSPACE) {
        if (!buffer_.empty()) {
            // UTF-8 aware backspace.
            std::size_t cut = buffer_.size() - 1;
            while (cut > 0
                   && (static_cast<unsigned char>(buffer_[cut]) & 0xC0) == 0x80) {
                --cut;
            }
            buffer_.resize(cut);
        }
        r.cmd = Command::InputBackspace;
        return r;
    }
    // Append the UTF-8 representation. ncinput::utf8 is null-padded.
    if (ni.utf8[0] != '\0') {
        buffer_.append(ni.utf8);
        std::memcpy(r.utf8, ni.utf8, sizeof(r.utf8));
        r.cmd = Command::InputChar;
    }
    return r;
}

void InputMap::clear_transient() {
    mode_ = Mode::Normal;
    buffer_.clear();
}

} // namespace transporter::tui
