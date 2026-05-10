// SPDX-License-Identifier: GPL-3.0-or-later
//
// Integration smoke test for transporter::engine::list_playback_devices.
// Prints every device, its fingerprint, and capability matrix. Exit 0 on
// success; non-zero with a stderr message on enumeration failure.

#include <transporter/engine/device.hpp>
#include <transporter/engine/error.hpp>
#include <transporter/engine/format.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace tp = transporter::engine;

namespace {

const char* fmt_short(tp::SampleFormat f) {
    switch (f) {
    case tp::SampleFormat::S16_LE:
        return "16";
    case tp::SampleFormat::S24_LE:
        return "24";
    case tp::SampleFormat::S24_3LE:
        return "24p";
    case tp::SampleFormat::S32_LE:
        return "32";
    case tp::SampleFormat::FLOAT_LE:
        return "f32";
    }
    return "?";
}

void print_rates(const std::vector<std::uint32_t>& rates) {
    std::printf("[");
    bool first = true;
    for (const auto r : rates) {
        if (!first) {
            std::printf(" ");
        }
        first = false;
        if (r % 1000 == 0) {
            std::printf("%u", r / 1000);
        } else {
            // Display 44100 as "44.1", 88200 as "88.2", etc.
            const unsigned int khz = r / 1000;
            const unsigned int frac = (r % 1000) / 100;
            std::printf("%u.%u", khz, frac);
        }
    }
    std::printf("]");
}

} // namespace

int main() {
    auto devs = tp::list_playback_devices();
    if (!devs) {
        std::fprintf(stderr, "list_playback_devices failed [%.*s]: %s\n",
                     static_cast<int>(tp::error_code_name(devs.error().code).size()),
                     tp::error_code_name(devs.error().code).data(),
                     devs.error().message.c_str());
        return 1;
    }

    if (devs->empty()) {
        std::printf("(no playback devices found)\n");
        return 0;
    }

    for (const auto& d : *devs) {
        std::printf("device: %s\n", d.id.c_str());
        std::printf("  hw       : %s (card %d, dev %d)\n", d.alsa_hw_string.c_str(),
                    d.alsa_card_index, d.alsa_device_index);
        std::printf("  alsa     : %s | %s\n", d.fingerprint.alsa_card_name.c_str(),
                    d.fingerprint.alsa_card_longname.c_str());
        if (d.fingerprint.is_usb) {
            const std::string& s = d.fingerprint.usb_serial;
            std::printf("  usb      : %s:%s:%s\n", d.fingerprint.usb_vendor_id.c_str(),
                        d.fingerprint.usb_product_id.c_str(),
                        s.empty() ? "(no serial)" : s.c_str());
        } else {
            std::printf("  usb      : not USB\n");
        }
        if (d.caps.caps_probe_failed) {
            std::printf("  caps     : probe failed: %s\n",
                        d.caps.probe_failure_reason.c_str());
        } else {
            // Format-major matrix: each format gets its own bracketed list of
            // accepted rates (in kHz). The current probe shares one rate set
            // across all formats, so per-format brackets are equal — kept
            // this shape so per-format probing in a future revision drops in
            // without changing the readout.
            std::printf("  matrix   :");
            for (const auto f : d.caps.formats) {
                std::printf(" %s:", fmt_short(f));
                print_rates(d.caps.sample_rates);
            }
            std::printf("\n");
            std::printf("  channels : min=%u max=%u\n",
                        unsigned{d.caps.channels_min}, unsigned{d.caps.channels_max});
            if (d.caps.hw_volume.present) {
                if (d.caps.hw_volume.has_db_scale) {
                    const double minf =
                        static_cast<double>(d.caps.hw_volume.min_db_x100) / 100.0;
                    const double maxf =
                        static_cast<double>(d.caps.hw_volume.max_db_x100) / 100.0;
                    std::printf("  hw vol   : %s [%.2f .. %.2f dB]\n",
                                d.caps.hw_volume.element_name.c_str(), minf, maxf);
                } else {
                    std::printf("  hw vol   : %s (no dB scale)\n",
                                d.caps.hw_volume.element_name.c_str());
                }
            } else {
                std::printf("  hw vol   : no HW volume\n");
            }
        }
        std::printf("\n");
    }
    return 0;
}
