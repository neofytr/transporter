// SPDX-License-Identifier: GPL-3.0-or-later
//
// Test-only seam. Lets unit tests construct an Engine over a mock IDevice
// without exposing IDevice on the public API. Not installed.

#ifndef TRANSPORTER_ENGINE_TEST_ACCESS_HPP
#define TRANSPORTER_ENGINE_TEST_ACCESS_HPP

#include "output_iface.hpp"

#include <transporter/engine/engine.hpp>
#include <transporter/engine/error.hpp>

#include <expected>
#include <memory>

namespace transporter::engine {

struct EngineTestHooks {
    static std::expected<std::unique_ptr<Engine>, Error>
    create_with_device(EngineConfig cfg, std::unique_ptr<detail::IDevice> dev);
};

} // namespace transporter::engine

#endif
