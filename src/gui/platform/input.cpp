// SPDX-License-Identifier: GPL-3.0-or-later
//
// wl_seat → ImGui input bridge. Pointer position + buttons + scroll;
// keyboard via xkbcommon for keymap / state. Modifier mask is mirrored
// onto ImGuiIO::AddKey* events.

#include "platform_internal.hpp"

#include <imgui.h>

#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstring>

namespace transporter::gui::platform {

namespace {

ImGuiKey xkb_to_imgui(xkb_keysym_t sym) {
    switch (sym) {
    case XKB_KEY_Tab:        return ImGuiKey_Tab;
    case XKB_KEY_Left:       return ImGuiKey_LeftArrow;
    case XKB_KEY_Right:      return ImGuiKey_RightArrow;
    case XKB_KEY_Up:         return ImGuiKey_UpArrow;
    case XKB_KEY_Down:       return ImGuiKey_DownArrow;
    case XKB_KEY_Page_Up:    return ImGuiKey_PageUp;
    case XKB_KEY_Page_Down:  return ImGuiKey_PageDown;
    case XKB_KEY_Home:       return ImGuiKey_Home;
    case XKB_KEY_End:        return ImGuiKey_End;
    case XKB_KEY_Insert:     return ImGuiKey_Insert;
    case XKB_KEY_Delete:     return ImGuiKey_Delete;
    case XKB_KEY_BackSpace:  return ImGuiKey_Backspace;
    case XKB_KEY_space:      return ImGuiKey_Space;
    case XKB_KEY_Return:     return ImGuiKey_Enter;
    case XKB_KEY_Escape:     return ImGuiKey_Escape;
    case XKB_KEY_F1:         return ImGuiKey_F1;
    case XKB_KEY_F2:         return ImGuiKey_F2;
    case XKB_KEY_F3:         return ImGuiKey_F3;
    case XKB_KEY_F4:         return ImGuiKey_F4;
    case XKB_KEY_F5:         return ImGuiKey_F5;
    case XKB_KEY_F6:         return ImGuiKey_F6;
    case XKB_KEY_Shift_L:
    case XKB_KEY_Shift_R:    return ImGuiKey_LeftShift;
    case XKB_KEY_Control_L:
    case XKB_KEY_Control_R:  return ImGuiKey_LeftCtrl;
    case XKB_KEY_Alt_L:
    case XKB_KEY_Alt_R:      return ImGuiKey_LeftAlt;
    default:                 break;
    }
    if (sym >= XKB_KEY_a && sym <= XKB_KEY_z) {
        return static_cast<ImGuiKey>(ImGuiKey_A + (sym - XKB_KEY_a));
    }
    if (sym >= XKB_KEY_A && sym <= XKB_KEY_Z) {
        return static_cast<ImGuiKey>(ImGuiKey_A + (sym - XKB_KEY_A));
    }
    if (sym >= XKB_KEY_0 && sym <= XKB_KEY_9) {
        return static_cast<ImGuiKey>(ImGuiKey_0 + (sym - XKB_KEY_0));
    }
    return ImGuiKey_None;
}

// pointer
void pointer_enter(void* data, wl_pointer* /*p*/, uint32_t /*serial*/,
                   wl_surface* /*surface*/, wl_fixed_t sx, wl_fixed_t sy) {
    auto& w = *static_cast<WindowImpl*>(data);
    w.mouse_in = true;
    w.mouse_x = wl_fixed_to_double(sx);
    w.mouse_y = wl_fixed_to_double(sy);
}

void pointer_leave(void* data, wl_pointer* /*p*/, uint32_t /*serial*/,
                   wl_surface* /*surface*/) {
    auto& w = *static_cast<WindowImpl*>(data);
    w.mouse_in = false;
}

void pointer_motion(void* data, wl_pointer* /*p*/, uint32_t /*time*/,
                    wl_fixed_t sx, wl_fixed_t sy) {
    auto& w = *static_cast<WindowImpl*>(data);
    w.mouse_x = wl_fixed_to_double(sx);
    w.mouse_y = wl_fixed_to_double(sy);
}

void pointer_button(void* data, wl_pointer* /*p*/, uint32_t /*serial*/,
                    uint32_t /*time*/, uint32_t button, uint32_t state) {
    auto& w = *static_cast<WindowImpl*>(data);
    int idx = -1;
    switch (button) {
    case BTN_LEFT:   idx = 0; break;
    case BTN_RIGHT:  idx = 1; break;
    case BTN_MIDDLE: idx = 2; break;
    default: return;
    }
    w.mouse_buttons[idx] = (state == WL_POINTER_BUTTON_STATE_PRESSED);
    if (idx >= 0) {
        ImGui::GetIO().AddMouseButtonEvent(idx, w.mouse_buttons[idx]);
    }
}

void pointer_axis(void* /*data*/, wl_pointer* /*p*/, uint32_t /*time*/,
                  uint32_t axis, wl_fixed_t value) {
    const float v = static_cast<float>(wl_fixed_to_double(value)) / 10.0f;
    ImGuiIO& io = ImGui::GetIO();
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        io.AddMouseWheelEvent(0.0f, -v);
    } else {
        io.AddMouseWheelEvent(-v, 0.0f);
    }
}

void pointer_frame(void* /*data*/, wl_pointer* /*p*/) {}
void pointer_axis_source(void* /*data*/, wl_pointer* /*p*/, uint32_t /*src*/) {}
void pointer_axis_stop(void* /*data*/, wl_pointer* /*p*/, uint32_t /*time*/, uint32_t /*axis*/) {}
void pointer_axis_discrete(void* /*data*/, wl_pointer* /*p*/, uint32_t /*axis*/, int32_t /*disc*/) {}

constexpr wl_pointer_listener kPointerListener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

// keyboard
void keyboard_keymap(void* data, wl_keyboard* /*k*/, uint32_t format,
                     int32_t fd, uint32_t size) {
    auto& w = *static_cast<WindowImpl*>(data);
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        ::close(fd);
        return;
    }
    void* mapped = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        ::close(fd);
        return;
    }
    if (w.keymap != nullptr) {
        xkb_keymap_unref(w.keymap);
    }
    if (w.kbd_state != nullptr) {
        xkb_state_unref(w.kbd_state);
    }
    w.keymap = xkb_keymap_new_from_string(
        w.xkb, static_cast<const char*>(mapped),
        XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    ::munmap(mapped, size);
    ::close(fd);
    if (w.keymap != nullptr) {
        w.kbd_state = xkb_state_new(w.keymap);
    }
}

void keyboard_enter(void* /*data*/, wl_keyboard* /*k*/, uint32_t /*serial*/,
                    wl_surface* /*surf*/, wl_array* /*keys*/) {}

void keyboard_leave(void* /*data*/, wl_keyboard* /*k*/, uint32_t /*serial*/,
                    wl_surface* /*surf*/) {
    ImGuiIO& io = ImGui::GetIO();
    io.ClearInputKeys();
}

void keyboard_key(void* data, wl_keyboard* /*k*/, uint32_t /*serial*/,
                  uint32_t /*time*/, uint32_t key, uint32_t state) {
    auto& w = *static_cast<WindowImpl*>(data);
    if (w.kbd_state == nullptr) {
        return;
    }
    const xkb_keycode_t kc = key + 8;  // evdev → xkb
    const xkb_keysym_t sym = xkb_state_key_get_one_sym(w.kbd_state, kc);
    const ImGuiKey ik = xkb_to_imgui(sym);
    const bool pressed = (state == WL_KEYBOARD_KEY_STATE_PRESSED);
    ImGuiIO& io = ImGui::GetIO();
    if (ik != ImGuiKey_None) {
        io.AddKeyEvent(ik, pressed);
    }
    if (pressed) {
        char buf[32]{};
        const int n = xkb_state_key_get_utf8(w.kbd_state, kc, buf, sizeof(buf));
        if (n > 0 && static_cast<unsigned char>(buf[0]) >= 0x20
            && buf[0] != 0x7F) {
            io.AddInputCharactersUTF8(buf);
        }
    }
}

void keyboard_modifiers(void* data, wl_keyboard* /*k*/, uint32_t /*serial*/,
                        uint32_t depressed, uint32_t latched, uint32_t locked,
                        uint32_t group) {
    auto& w = *static_cast<WindowImpl*>(data);
    if (w.kbd_state == nullptr) {
        return;
    }
    xkb_state_update_mask(w.kbd_state, depressed, latched, locked, 0, 0, group);
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl,
        xkb_state_mod_name_is_active(w.kbd_state, XKB_MOD_NAME_CTRL,
                                     XKB_STATE_MODS_EFFECTIVE) > 0);
    io.AddKeyEvent(ImGuiMod_Shift,
        xkb_state_mod_name_is_active(w.kbd_state, XKB_MOD_NAME_SHIFT,
                                     XKB_STATE_MODS_EFFECTIVE) > 0);
    io.AddKeyEvent(ImGuiMod_Alt,
        xkb_state_mod_name_is_active(w.kbd_state, XKB_MOD_NAME_ALT,
                                     XKB_STATE_MODS_EFFECTIVE) > 0);
    io.AddKeyEvent(ImGuiMod_Super,
        xkb_state_mod_name_is_active(w.kbd_state, XKB_MOD_NAME_LOGO,
                                     XKB_STATE_MODS_EFFECTIVE) > 0);
}

void keyboard_repeat_info(void* data, wl_keyboard* /*k*/, int32_t rate, int32_t delay) {
    auto& w = *static_cast<WindowImpl*>(data);
    w.repeat_rate = rate;
    w.repeat_delay_ms = delay;
}

constexpr wl_keyboard_listener kKeyboardListener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

void seat_capabilities(void* data, wl_seat* seat, uint32_t caps) {
    auto& w = *static_cast<WindowImpl*>(data);
    const bool has_kbd = (caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
    const bool has_ptr = (caps & WL_SEAT_CAPABILITY_POINTER) != 0;

    if (has_kbd && w.keyboard == nullptr) {
        w.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(w.keyboard, &kKeyboardListener, &w);
    } else if (!has_kbd && w.keyboard != nullptr) {
        wl_keyboard_release(w.keyboard);
        w.keyboard = nullptr;
    }

    if (has_ptr && w.pointer == nullptr) {
        w.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(w.pointer, &kPointerListener, &w);
    } else if (!has_ptr && w.pointer != nullptr) {
        wl_pointer_release(w.pointer);
        w.pointer = nullptr;
    }
}

void seat_name(void* /*data*/, wl_seat* /*seat*/, const char* /*name*/) {}

constexpr wl_seat_listener kSeatListener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

} // namespace

void register_seat(WindowImpl& w) {
    w.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (w.seat != nullptr) {
        wl_seat_add_listener(w.seat, &kSeatListener, &w);
    }
}

void destroy_input(WindowImpl& w) {
    if (w.keyboard != nullptr) {
        wl_keyboard_release(w.keyboard);
        w.keyboard = nullptr;
    }
    if (w.pointer != nullptr) {
        wl_pointer_release(w.pointer);
        w.pointer = nullptr;
    }
    if (w.seat != nullptr) {
        wl_seat_destroy(w.seat);
        w.seat = nullptr;
    }
    if (w.kbd_state != nullptr) {
        xkb_state_unref(w.kbd_state);
        w.kbd_state = nullptr;
    }
    if (w.keymap != nullptr) {
        xkb_keymap_unref(w.keymap);
        w.keymap = nullptr;
    }
    if (w.xkb != nullptr) {
        xkb_context_unref(w.xkb);
        w.xkb = nullptr;
    }
}

void input_pump_imgui(WindowImpl& w) {
    ImGuiIO& io = ImGui::GetIO();
    if (w.mouse_in) {
        io.AddMousePosEvent(static_cast<float>(w.mouse_x),
                            static_cast<float>(w.mouse_y));
    }
}

} // namespace transporter::gui::platform
