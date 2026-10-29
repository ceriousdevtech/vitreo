// SPDX-License-Identifier: LGPL-2.1-only
#include "winapi/graphics/dwrite/dwrite_internal.hpp"

#include "capsule/vfs/vfs.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/graphics/dwrite/font_backend.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace vitreo::winapi {
namespace {

void set_float_return(Marshaler& m, float value) {
    if (!m.has_cpu()) return;
    if (m.cpu_is_64bit()) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        m.cpu()->sse.xmm[0].u32_[0] = bits;
        m.cpu()->sse.xmm[0].u32_[1] = 0;
        m.cpu()->sse.xmm[0].u64_[1] = 0;
        return;
    }
    // MSVC x86 returns FLOAT COM results in ST(0).
    m.cpu()->fpu.push(cpu::Float80(static_cast<double>(value)));
}

} // namespace

void DWriteManager::impl_CreateRenderingParams(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr params_out = m.arg_ptr(1);
    if (params_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(params_out, 0);
    const auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const GuestAddr params = create_object(ObjectType::RenderingParams);
    if (params == 0 || m.write_ptr(params_out, params).is_err()) {
        if (params != 0) m_objects.erase(params);
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
                     "[dwrite-diag] CreateRenderingParams factory=0x%llx "
                     "-> S_OK params=0x%llx\n",
                     static_cast<unsigned long long>(self),
                     static_cast<unsigned long long>(params));
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_CreateMonitorRenderingParams(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    (void)m.arg(1); // HMONITOR — defaults match primary-monitor params.
    const GuestAddr params_out = m.arg_ptr(2);
    if (params_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(params_out, 0);
    const auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const GuestAddr params = create_object(ObjectType::RenderingParams);
    if (params == 0 || m.write_ptr(params_out, params).is_err()) {
        if (params != 0) m_objects.erase(params);
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m.set_return(S_OK);
}


void DWriteManager::impl_CreateTypography(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr typography_out = m.arg_ptr(1);
    if (typography_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(typography_out, 0);
    const auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const GuestAddr typography = create_object(ObjectType::Typography);
    if (typography == 0 || m.write_ptr(typography_out, typography).is_err()) {
        if (typography != 0) m_objects.erase(typography);
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
                     "[dwrite-diag] CreateTypography factory=0x%llx "
                     "-> S_OK typography=0x%llx\n",
                     static_cast<unsigned long long>(self),
                     static_cast<unsigned long long>(typography));
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_TypographyAddFontFeature(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    u32 name_tag = 0;
    u32 parameter = 0;
    if (m.cpu_is_64bit()) {
        // DWRITE_FONT_FEATURE is 8 bytes and passes in one integer register.
        const u64 packed = m.arg(1);
        name_tag = static_cast<u32>(packed);
        parameter = static_cast<u32>(packed >> 32);
    } else {
        name_tag = static_cast<u32>(m.arg(1));
        parameter = static_cast<u32>(m.arg(2));
    }
    const auto typography = m_objects.find(self);
    if (typography == m_objects.end() ||
        typography->second.type != ObjectType::Typography) {
        m.set_return(E_INVALIDARG);
        return;
    }
    typography->second.typography_features.emplace_back(name_tag, parameter);
    m.set_return(S_OK);
}

void DWriteManager::impl_TypographyGetFontFeatureCount(Marshaler& m) {
    const auto typography = m_objects.find(m.arg_ptr(0));
    if (typography == m_objects.end() ||
        typography->second.type != ObjectType::Typography) {
        m.set_return(0);
        return;
    }
    m.set_return(static_cast<u32>(
        typography->second.typography_features.size()));
}

void DWriteManager::impl_TypographyGetFontFeature(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const u32 index = static_cast<u32>(m.arg(1));
    const GuestAddr feature_out = m.arg_ptr(2);
    if (feature_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    const auto typography = m_objects.find(self);
    if (typography == m_objects.end() ||
        typography->second.type != ObjectType::Typography ||
        index >= typography->second.typography_features.size()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const auto& feature = typography->second.typography_features[index];
    if (m.write_u32(feature_out, feature.first).is_err() ||
        m.write_u32(feature_out + 4, feature.second).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_RenderingParamsGetGamma(Marshaler& m) {
    const auto params = m_objects.find(m.arg_ptr(0));
    if (params == m_objects.end() ||
        params->second.type != ObjectType::RenderingParams) {
        set_float_return(m, 0.0f);
        return;
    }
    set_float_return(m, params->second.gamma);
}

void DWriteManager::impl_RenderingParamsGetEnhancedContrast(Marshaler& m) {
    const auto params = m_objects.find(m.arg_ptr(0));
    if (params == m_objects.end() ||
        params->second.type != ObjectType::RenderingParams) {
        set_float_return(m, 0.0f);
        return;
    }
    set_float_return(m, params->second.enhanced_contrast);
}

void DWriteManager::impl_RenderingParamsGetClearTypeLevel(Marshaler& m) {
    const auto params = m_objects.find(m.arg_ptr(0));
    if (params == m_objects.end() ||
        params->second.type != ObjectType::RenderingParams) {
        set_float_return(m, 0.0f);
        return;
    }
    set_float_return(m, params->second.clear_type_level);
}

void DWriteManager::impl_RenderingParamsGetPixelGeometry(Marshaler& m) {
    const auto params = m_objects.find(m.arg_ptr(0));
    m.set_return(params != m_objects.end() &&
                         params->second.type == ObjectType::RenderingParams
                     ? params->second.pixel_geometry
                     : 0u);
}

void DWriteManager::impl_RenderingParamsGetRenderingMode(Marshaler& m) {
    const auto params = m_objects.find(m.arg_ptr(0));
    m.set_return(params != m_objects.end() &&
                         params->second.type == ObjectType::RenderingParams
                     ? params->second.rendering_mode
                     : 0u);
}

void DWriteManager::impl_CreateNumberSubstitution(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const u32 method = static_cast<u32>(m.arg(1));
    const GuestAddr locale_address = m.arg_ptr(2);
    const bool ignore_user_override = m.arg(3) != 0;
    const GuestAddr substitution_out = m.arg_ptr(4);
    if (substitution_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(substitution_out, 0);
    const auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory || method > 4 ||
        locale_address == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    auto locale = read_utf16_z(locale_address, 85);
    if (locale.is_err() || locale.value().empty()) {
        m.set_return(E_INVALIDARG);
        return;
    }

    const GuestAddr substitution =
        create_object(ObjectType::NumberSubstitution);
    if (substitution == 0) {
        m.set_return(DWRITE_E_FILEFORMAT);
        return;
    }
    auto& object = m_objects.at(substitution);
    object.number_method = method;
    object.ignore_user_override = ignore_user_override;
    object.locale = std::move(locale.value());
    if (m.write_ptr(substitution_out, substitution).is_err()) {
        m_objects.erase(substitution);
        m.set_return(E_POINTER);
        return;
    }
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
                     "[dwrite-diag] CreateNumberSubstitution factory=0x%llx "
                     "method=%u ignore_override=%u -> S_OK object=0x%llx\n",
                     static_cast<unsigned long long>(self), method,
                     ignore_user_override ? 1u : 0u,
                     static_cast<unsigned long long>(substitution));
    }
    m.set_return(S_OK);
}

} // namespace vitreo::winapi
