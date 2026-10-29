// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file com_test_common.hpp
/// @brief Shared includes and using-directives for the test_com_*.cpp files that
/// were split out of test_com_core.cpp. Test-local; include first.

#include <gtest/gtest.h>
#include <capsule/registry/registry.hpp>
#include <capsule/vfs/host_filesystem.hpp>
#include <capsule/vfs/vfs.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/synchronization/sync.hpp>
#include <winapi/kernel32/memory/heap.hpp>
#include <winapi/kernel32/memory/memory.hpp>
#include <winapi/kernel32/memory/global_local.hpp>
#include <winapi/com/ole32/com_core.hpp>
#include <winapi/com/ole32/task_memory.hpp>
#include <winapi/system/shlwapi/shlwapi.hpp>
#include <winapi/core/thunk_registry.hpp>
#include <winapi/kernel32/memory/remote_memory.hpp>
#include <winapi/kernel32/module/module.hpp>
#include <winapi/kernel32/thread/thread.hpp>
#include "../fake_audio_capture.hpp"
#include <winapi/media/dsound/dsound.hpp>
#include <winapi/media/winmm/winmm.hpp>
#include <windowing/contracts/wave_output.hpp>
#include <audio/contracts/audio_capture.hpp>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <limits>
#include <chrono>
#include <cstring>
#include <functional>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <unistd.h>

using namespace vitreo;
using namespace vitreo::winapi;
