// SPDX-License-Identifier: LGPL-2.1-only
/// @file dxbc_to_spirv_test_spirv.h
/// @brief SPIR-V word-stream scanners shared by the test_dxbc_to_spirv*.cpp suites.
#pragma once

#include "vitreo/types.hpp"

#include <vector>

namespace vitreo::graphics::dxbc_spirv_test {

inline bool spirv_contains_opcode(const std::vector<u32>& spirv, u32 opcode) {
    for (usize offset = 5; offset < spirv.size();) {
        const u32 instruction = spirv[offset];
        const u32 word_count = instruction >> 16;
        if ((instruction & 0xFFFFu) == opcode) return true;
        if (word_count == 0 || offset + word_count > spirv.size()) return false;
        offset += word_count;
    }
    return false;
}

inline bool spirv_contains_decoration(const std::vector<u32>& spirv,
                               u32 decoration) {
    for (usize offset = 5; offset < spirv.size();) {
        const u32 instruction = spirv[offset];
        const u32 word_count = instruction >> 16;
        if ((instruction & 0xFFFFu) == 71u && word_count >= 3u &&
            spirv[offset + 2u] == decoration) {
            return true;
        }
        if (word_count == 0 || offset + word_count > spirv.size()) return false;
        offset += word_count;
    }
    return false;
}

inline bool spirv_contains_capability(const std::vector<u32>& spirv,
                               u32 capability) {
    for (usize offset = 5; offset < spirv.size();) {
        const u32 instruction = spirv[offset];
        const u32 word_count = instruction >> 16;
        if ((instruction & 0xFFFFu) == 17u && word_count == 2u &&
            spirv[offset + 1u] == capability) {
            return true;
        }
        if (word_count == 0 || offset + word_count > spirv.size()) return false;
        offset += word_count;
    }
    return false;
}

inline bool spirv_contains_execution_mode(const std::vector<u32>& spirv, u32 mode,
                                   u32 literal = 0xFFFFFFFFu) {
    for (usize offset = 5; offset < spirv.size();) {
        const u32 instruction = spirv[offset];
        const u32 word_count = instruction >> 16;
        if ((instruction & 0xFFFFu) == 16u && word_count >= 3u &&
            spirv[offset + 2u] == mode &&
            (literal == 0xFFFFFFFFu ||
             (word_count >= 4u && spirv[offset + 3u] == literal))) {
            return true;
        }
        if (word_count == 0 || offset + word_count > spirv.size()) return false;
        offset += word_count;
    }
    return false;
}

inline bool spirv_contains_execution_mode_xyz(const std::vector<u32>& spirv,
                                       u32 mode, u32 x, u32 y, u32 z) {
    for (usize offset = 5; offset < spirv.size();) {
        const u32 instruction = spirv[offset];
        const u32 word_count = instruction >> 16;
        if ((instruction & 0xFFFFu) == 16u && word_count >= 6u &&
            spirv[offset + 2u] == mode && spirv[offset + 3u] == x &&
            spirv[offset + 4u] == y && spirv[offset + 5u] == z) {
            return true;
        }
        if (word_count == 0 || offset + word_count > spirv.size()) return false;
        offset += word_count;
    }
    return false;
}

inline bool spirv_contains_ext_inst_instruction(const std::vector<u32>& spirv,
                                         u32 instruction_number) {
    for (usize offset = 5; offset < spirv.size();) {
        const u32 instruction = spirv[offset];
        const u32 word_count = instruction >> 16;
        if ((instruction & 0xFFFFu) == 12u && word_count >= 5u &&
            spirv[offset + 4u] == instruction_number) {
            return true;
        }
        if (word_count == 0 || offset + word_count > spirv.size()) return false;
        offset += word_count;
    }
    return false;
}

inline bool spirv_contains_builtin(const std::vector<u32>& spirv, u32 builtin) {
    for (usize offset = 5; offset < spirv.size();) {
        const u32 instruction = spirv[offset];
        const u32 word_count = instruction >> 16;
        // OpDecorate target BuiltIn builtin
        if ((instruction & 0xFFFFu) == 71u && word_count == 4u &&
            spirv[offset + 2] == 11u && spirv[offset + 3] == builtin) {
            return true;
        }
        if (word_count == 0 || offset + word_count > spirv.size()) return false;
        offset += word_count;
    }
    return false;
}

inline bool spirv_contains_gradient_sample(const std::vector<u32>& spirv) {
    for (usize offset = 5; offset < spirv.size();) {
        const u32 instruction = spirv[offset];
        const u32 word_count = instruction >> 16;
        // OpImageSampleExplicitLod operands are result type, result, sampled
        // image, coordinate, image-operands mask, then the Grad dx/dy IDs.
        if ((instruction & 0xFFFFu) == 88u && word_count == 8u &&
            spirv[offset + 5] == 0x4u) {
            return true;
        }
        if (word_count == 0 || offset + word_count > spirv.size()) return false;
        offset += word_count;
    }
    return false;
}

inline bool spirv_contains_fetch_const_offset(const std::vector<u32>& spirv) {
    for (usize offset = 5; offset < spirv.size();) {
        const u32 instruction = spirv[offset];
        const u32 word_count = instruction >> 16;
        if ((instruction & 0xFFFFu) == 95u && word_count == 8u &&
            spirv[offset + 5] == 0xAu) {
            return true;
        }
        if (word_count == 0 || offset + word_count > spirv.size()) return false;
        offset += word_count;
    }
    return false;
}

inline bool spirv_contains_multisample_fetch(const std::vector<u32>& spirv) {
    for (usize offset = 5; offset < spirv.size();) {
        const u32 instruction = spirv[offset];
        const u32 word_count = instruction >> 16;
        if ((instruction & 0xFFFFu) == 95u && word_count == 7u &&
            spirv[offset + 5] == 0x40u) {
            return true;
        }
        if (word_count == 0 || offset + word_count > spirv.size()) return false;
        offset += word_count;
    }
    return false;
}

inline bool spirv_contains_multisample_fetch_const_offset(
    const std::vector<u32>& spirv) {
    for (usize offset = 5; offset < spirv.size();) {
        const u32 instruction = spirv[offset];
        const u32 word_count = instruction >> 16;
        // ConstOffset (0x8) precedes Sample (0x40) in operand-bit order.
        if ((instruction & 0xFFFFu) == 95u && word_count == 8u &&
            spirv[offset + 5] == 0x48u) {
            return true;
        }
        if (word_count == 0 || offset + word_count > spirv.size()) return false;
        offset += word_count;
    }
    return false;
}

inline bool spirv_contains_arrayed_2d_image(const std::vector<u32>& spirv) {
    for (usize offset = 5; offset < spirv.size();) {
        const u32 instruction = spirv[offset];
        const u32 word_count = instruction >> 16;
        if ((instruction & 0xFFFFu) == 25u && word_count >= 9u &&
            spirv[offset + 3] == 1u && spirv[offset + 5] == 1u) {
            return true;
        }
        if (word_count == 0 || offset + word_count > spirv.size()) return false;
        offset += word_count;
    }
    return false;
}

inline bool spirv_contains_arrayed_cube_image(const std::vector<u32>& spirv) {
    for (usize offset = 5; offset < spirv.size();) {
        const u32 instruction = spirv[offset];
        const u32 word_count = instruction >> 16;
        if ((instruction & 0xFFFFu) == 25u && word_count >= 9u &&
            spirv[offset + 3] == 3u && spirv[offset + 5] == 1u) {
            return true;
        }
        if (word_count == 0 || offset + word_count > spirv.size()) return false;
        offset += word_count;
    }
    return false;
}

}  // namespace vitreo::graphics::dxbc_spirv_test
