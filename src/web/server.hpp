// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The web server's public interface lives with the other module headers under
// include/transporter/. This shim keeps src/web/ self-contained for builds
// that include it by relative path.
#include <transporter/web/server.hpp>
