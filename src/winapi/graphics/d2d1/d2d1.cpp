// SPDX-License-Identifier: LGPL-2.1-only
#include "d2d1.hpp"
#include "core/memory/guest_memory.hpp"
#include "d2d1_contract.inc"
#include "winapi/core/marshaler.hpp"
#include <cstdio>
#include <cstring>
#include <iterator>
namespace vitreo::winapi {
using namespace d2d1_contract;
namespace {
constexpr usize object_region_size = GUEST_PAGE_SIZE;
}
D2DManager::D2DManager(GuestMemory &memory, bool wide, GdiManager *gdi)
    : m_memory(memory), m_is_64bit(wide), m_gdi(gdi) {}
D2DManager::~D2DManager() {
  for(const auto& [base,unused]:m_geometries)(void)m_memory.unmap_region(base,object_region_size);
  for (const auto &[base, unused] : m_resources)
    (void)m_memory.unmap_region(base, object_region_size);
  for (const auto &[base, unused] : m_factories)
    (void)m_memory.unmap_region(base, object_region_size);
}
bool D2DManager::set_backend(graphics::GraphicsBackendDevice *backend) {
  std::lock_guard lock(m_mutex);
  if (!m_resources.empty() || (m_backend != nullptr && backend != m_backend))
    return false;
  m_backend = backend;
  return true;
}
void D2DManager::set_guest_interface_call(GuestInterfaceCall call) {
  std::lock_guard lock(m_mutex);
  m_guest_interface_call = std::move(call);
}
void D2DManager::set_dpi_provider(
    std::function<std::pair<float, float>()> provider) {
  std::lock_guard lock(m_mutex);
  m_dpi = std::move(provider);
}
bool D2DManager::bind_thunks(const ThunkRegistry &registry) {
  std::lock_guard lock(m_mutex);
  m_bound = false;
  for (usize slot = 0; slot < std::size(factory_methods); ++slot) {
    const auto name =
        std::string("_D2D1_Factory_") + factory_methods[slot].name;
    auto address = registry.get_thunk_address("d2d1.dll", name, true);
    if (address.is_err())
      return false;
    m_methods[slot] = address.value();
  }
  for (usize slot = 0; slot < std::size(target_methods); ++slot) {
    auto address = registry.get_thunk_address(
        "d2d1.dll", std::string("_D2D1_Target_") + target_methods[slot].name,
        true);
    if (address.is_err())
      return false;
    m_target_methods[slot] = address.value();
  }
  for (usize slot = 0; slot < std::size(brush_methods); ++slot) {
    auto address = registry.get_thunk_address(
        "d2d1.dll", std::string("_D2D1_Brush_") + brush_methods[slot].name,
        true);
    if (address.is_err())
      return false;
    m_brush_methods[slot] = address.value();
  }
  for(usize slot=0;slot<std::size(bitmap_methods);++slot){
    auto address=registry.get_thunk_address("d2d1.dll",std::string("_D2D1_Bitmap_")+bitmap_methods[slot].name,true);
    if(address.is_err())return false;
    m_bitmap_methods[slot]=address.value();
  }
  for(bool sink:{false,true}){const usize count=sink?std::size(sink_methods):std::size(path_methods);for(usize slot=0;slot<count;++slot){auto address=registry.get_thunk_address("d2d1.dll",std::string(sink?"_D2D1_Sink_":"_D2D1_Path_")+(sink?sink_methods[slot].name:path_methods[slot].name),true);if(address.is_err())return false;if(sink)m_sink_methods[slot]=address.value();else m_path_methods[slot]=address.value();}}
  m_bound = true;
  return true;
}
bool D2DManager::write_pointer(Marshaler &marshaler, GuestAddr destination,
                               GuestAddr value) {
  const auto result =
      m_is_64bit ? m_memory.write_u64(destination, value)
                 : m_memory.write_u32(destination, static_cast<u32>(value));
  if (result.is_ok())
    return true;
  (void)marshaler.raise_access_violation(true, destination);
  return false;
}
bool D2DManager::read_iid(Marshaler &marshaler, GuestAddr address,
                          std::array<u8, 16> &iid) {
  if (m_memory
          .read_bytes(address, reinterpret_cast<Byte *>(iid.data()), iid.size())
          .is_ok())
    return true;
  (void)marshaler.raise_access_violation(false, address);
  return false;
}
void D2DManager::create_factory(Marshaler &m) {
  const u32 type = static_cast<u32>(m.arg(0));
  const GuestAddr iid_address = m.arg(1), options = m.arg(2), output = m.arg(3);
  if (!write_pointer(m, output, 0))
    return;
  std::array<u8, 16> iid{};
  if (!read_iid(m, iid_address, iid))
    return;
  if (type != D2D1_FACTORY_TYPE_SINGLE_THREADED &&
      type != D2D1_FACTORY_TYPE_MULTI_THREADED) {
    m.set_return(E_INVALIDARG);
    return;
  }
  if (options && m_memory.read_u32(options).is_err()) {
    (void)m.raise_access_violation(false, options);
    return;
  }
  // SDK: debugLevel is ignored when the optional debug-layer DLL is absent.
  if (iid != factory_iid && iid != unknown_iid) {
    m.set_return(E_NOINTERFACE);
    return;
  }
  std::lock_guard lock(m_mutex);
  if (!m_dpi || !m_bound) {
    if (!m_warned[0]) {
      m_warned[0] = true;
      std::fprintf(stderr, "[vitreo] UNIMPLEMENTED d2d1.dll!D2D1CreateFactory: "
                           "runtime binding unavailable\n");
    }
    m.set_return(E_NOTIMPL);
    return;
  }
  const auto dpi = m_dpi();
  const auto base = m_memory.map_first_fit(0x60000000u, object_region_size,
                                           MemoryProtection::ReadWrite);
  if (!base) {
    m.set_return(E_OUTOFMEMORY);
    return;
  }
  const usize width = m_is_64bit ? sizeof(u64) : sizeof(u32);
  const GuestAddr vtable = base + width;
  bool written = write_pointer(m, base, vtable);
  for (usize slot = 0; written && slot < m_methods.size(); ++slot)
    written = write_pointer(m, vtable + slot * width, m_methods[slot]);
  if (!written) {
    (void)m_memory.unmap_region(base, object_region_size);
    return;
  }
  m_factories.emplace(base, Factory{1, type, dpi.first, dpi.second});
  if (!write_pointer(m, output, base)) {
    m_factories.erase(base);
    (void)m_memory.unmap_region(base, object_region_size);
    return;
  }
  m.set_return(S_OK);
}
void D2DManager::factory_method(Marshaler &m, usize slot) {
  const GuestAddr self = m.arg(0);
  std::lock_guard lock(m_mutex);
  auto it = m_factories.find(self);
  if (it == m_factories.end()) {
    (void)m.raise_access_violation(false, self);
    return;
  }
  auto &object = it->second;
  if (slot == factory_QueryInterface) {
    if (m.arg(2) == 0) {
      m.set_return(E_POINTER);
      return;
    }
    const GuestAddr output = m.arg(2);
    if (!write_pointer(m, output, 0))
      return;
    std::array<u8, 16> iid{};
    if (!read_iid(m, m.arg(1), iid))
      return;
    if (iid != factory_iid && iid != unknown_iid) {
      m.set_return(E_NOINTERFACE);
      return;
    }
    if (!write_pointer(m, output, self))
      return;
    ++object.references;
    m.set_return(S_OK);
    return;
  }
  if (slot == factory_AddRef) {
    m.set_return(++object.references);
    return;
  }
  if (slot == factory_Release) {
    const u32 remaining = --object.references;
    if (!remaining) {
      m_factories.erase(it);
      (void)m_memory.unmap_region(self, object_region_size);
    }
    m.set_return(remaining);
    return;
  }
  if (slot == factory_ReloadSystemMetrics) {
    const auto dpi = m_dpi();
    object.dpi_x = dpi.first;
    object.dpi_y = dpi.second;
    m.set_return(S_OK);
    return;
  }
  if (slot == factory_GetDesktopDpi) {
    u32 x, y;
    std::memcpy(&x, &object.dpi_x, sizeof(x));
    std::memcpy(&y, &object.dpi_y, sizeof(y));
    if (m_memory.write_u32(m.arg(1), x).is_err()) {
      (void)m.raise_access_violation(true, m.arg(1));
      return;
    }
    if (m_memory.write_u32(m.arg(2), y).is_err())
      (void)m.raise_access_violation(true, m.arg(2));
    return;
  }
  if(slot==factory_CreatePathGeometry){create_path_geometry(m,self);return;}
  if (slot == factory_CreateDCRenderTarget) {
    create_dc_target(m, self);
    return;
  }
  const usize output_argument = factory_methods[slot].bytes / sizeof(u32) - 1;
  if (!write_pointer(m, m.arg(output_argument), 0))
    return;
  if (!m_warned[slot]) {
    m_warned[slot] = true;
    std::fprintf(stderr,
                 "[vitreo] UNIMPLEMENTED d2d1.dll!ID2D1Factory::%s: resource "
                 "implementation unavailable\n",
                 factory_methods[slot].name);
  }
  m.set_return(E_NOTIMPL);
}
DllExports create_d2d1_thunks(D2DManager &manager) {
  DllExports exports;
  exports.entries.push_back(
      {"D2D1CreateFactory", create_factory_ordinal,
       [&manager](Marshaler &m) { manager.create_factory(m); },
       create_factory_bytes});
  for (usize slot = 0; slot < std::size(factory_methods); ++slot)
    exports.entries.push_back(
        {std::string("_D2D1_Factory_") + factory_methods[slot].name, 0,
         [&manager, slot](Marshaler &m) { manager.factory_method(m, slot); },
         factory_methods[slot].bytes, true});
  for (usize slot = 0; slot < std::size(target_methods); ++slot)
    exports.entries.push_back(
        {std::string("_D2D1_Target_") + target_methods[slot].name, 0,
         [&manager, slot](Marshaler &m) { manager.target_method(m, slot); },
         target_methods[slot].bytes, true});
  for (usize slot = 0; slot < std::size(brush_methods); ++slot)
    exports.entries.push_back(
        {std::string("_D2D1_Brush_") + brush_methods[slot].name, 0,
         [&manager, slot](Marshaler &m) { manager.brush_method(m, slot); },
         brush_methods[slot].bytes, true});
  for(usize slot=0;slot<std::size(bitmap_methods);++slot)
    exports.entries.push_back({std::string("_D2D1_Bitmap_")+bitmap_methods[slot].name,0,
      [&manager,slot](Marshaler& m){manager.bitmap_method(m,slot);},bitmap_methods[slot].bytes,true});
  for(bool sink:{false,true})for(usize slot=0;slot<(sink?std::size(sink_methods):std::size(path_methods));++slot)exports.entries.push_back({std::string(sink?"_D2D1_Sink_":"_D2D1_Path_")+(sink?sink_methods[slot].name:path_methods[slot].name),0,[&manager,slot,sink](Marshaler& m){manager.geometry_method(m,slot,sink);},sink?sink_methods[slot].bytes:path_methods[slot].bytes,true});
  return exports;
}
} // namespace vitreo::winapi
