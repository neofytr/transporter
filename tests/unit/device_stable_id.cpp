// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for make_stable_id. Synthetic DeviceFingerprints; no ALSA, no
// udev. Asserts the documented format and the [a-zA-Z0-9._-] sanitization.

#include <transporter/engine/device.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace tp = transporter::engine;

namespace {

int failed = 0;

void check_eq(const char* label, const std::string& got, const std::string& want) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s\n  got:  %s\n  want: %s\n", label,
                     got.c_str(), want.c_str());
        ++failed;
    }
}

} // namespace

int main() {
    {
        // USB DAC with serial -> "usb:<vid>:<pid>:<serial>"
        tp::DeviceFingerprint fp;
        fp.is_usb = true;
        fp.usb_vendor_id = "152a";
        fp.usb_product_id = "8750";
        fp.usb_serial = "TPE7-001234";
        fp.alsa_card_name = "E70";
        fp.alsa_card_longname = "Topping E70 Velvet at usb-...";
        check_eq("usb_with_serial", tp::make_stable_id(fp),
                 "usb:152a:8750:TPE7-001234");
    }
    {
        // USB DAC, no serial -> stable but tagged with sanitized longname
        tp::DeviceFingerprint fp;
        fp.is_usb = true;
        fp.usb_vendor_id = "0bda";
        fp.usb_product_id = "4014";
        fp.usb_serial = "";
        fp.alsa_card_name = "Generic";
        fp.alsa_card_longname = "Generic USB Audio at usb-0000:00:14.0-1, full speed";
        check_eq("usb_no_serial", tp::make_stable_id(fp),
                 "usb:0bda:4014:noserial:Generic_USB_Audio_at_usb-0000_00_14.0-1__full_speed");
    }
    {
        // Non-USB -> "card:<sanitized longname>"
        tp::DeviceFingerprint fp;
        fp.is_usb = false;
        fp.alsa_card_name = "PCH";
        fp.alsa_card_longname = "HDA Intel PCH at 0xf7e30000 irq 134";
        check_eq("non_usb", tp::make_stable_id(fp),
                 "card:HDA_Intel_PCH_at_0xf7e30000_irq_134");
    }
    {
        // Non-USB with empty longname falls back to card name
        tp::DeviceFingerprint fp;
        fp.is_usb = false;
        fp.alsa_card_name = "Loopback";
        fp.alsa_card_longname = "";
        check_eq("non_usb_no_longname", tp::make_stable_id(fp),
                 "card:Loopback");
    }
    {
        // Sanitization: punctuation, slashes, parentheses all become '_'.
        // Allowed chars [a-zA-Z0-9._-] pass through.
        tp::DeviceFingerprint fp;
        fp.is_usb = false;
        fp.alsa_card_name = "x";
        fp.alsa_card_longname = "Foo/Bar (rev 2.1)+baz_q-w";
        check_eq("sanitization", tp::make_stable_id(fp),
                 "card:Foo_Bar__rev_2.1__baz_q-w");
    }
    {
        // USB with serial that contains punctuation -> serial sanitized too.
        tp::DeviceFingerprint fp;
        fp.is_usb = true;
        fp.usb_vendor_id = "abcd";
        fp.usb_product_id = "1234";
        fp.usb_serial = "ABC/DEF 12";
        check_eq("usb_serial_sanitization", tp::make_stable_id(fp),
                 "usb:abcd:1234:ABC_DEF_12");
    }

    if (failed != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failed);
        return 1;
    }
    return 0;
}
