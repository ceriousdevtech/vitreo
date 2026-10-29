// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file d3d11_test_common.hpp
/// @brief Includes shared by the D3D11 frontend tests
///        (test_d3d11_*.cpp and the d3d11_test_*.hpp fixtures).

#include <gtest/gtest.h>
#include "winapi/core/com_direct.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/delta_record.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/stream_out_repack.hpp"
#include "graphics/shaders/ia_resolve.hpp"
#include "graphics/shaders/tessellator.hpp"
#include "winapi/graphics/common/gpu_event_signaler.hpp"
#include "winapi/graphics/common/shared_keyed_mutex.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/common/live_diag.hpp"
#include "winapi/graphics/d3d11/d3d11_draw_dump.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_in_chain.hpp"
#include "winapi/graphics/d3d11/d3d11_stdcall.hpp"
#include "winapi/graphics/d3d11/d3d11_texel_encode.hpp"
#include "winapi/graphics/d3d11/d3d11_vertex_formats.hpp"
#include "winapi/graphics/dcomp/dcomp.hpp"
#include "winapi/graphics/dxgi/dxgi.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/shared_resource_handle.hpp"
#include "winapi/kernel32/named_object_directory.hpp"
#include "winapi/core/marshaler.hpp"

#include "winapi/graphics/d3d10/d3d10.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include <algorithm>
#include <atomic>
#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <functional>
#include <memory>
#include <limits>
#include <map>
#include <tuple>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>
