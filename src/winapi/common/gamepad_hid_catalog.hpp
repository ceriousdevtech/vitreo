// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include "winapi/common/gamepad_hid_report.hpp"
#include "hardware/contracts/gamepad_background_input.hpp"
#include <deque>
#include <atomic>
#include <cctype>
#include <string>
#include <mutex>
#include <optional>

namespace vitreo::winapi {
// One HID file object, shared by DuplicateHandle and pinned by pending I/O.
// Each independent CreateFile receives its own input ring. Device generation
// prevents an open file from silently attaching to a replacement controller.
class HidCompletionSignal {
public:
    virtual ~HidCompletionSignal()=default;
    virtual void reset()=0;
    virtual void signal()=0;
    [[nodiscard]] virtual u64 native_id() const=0;
};
struct HidOpenFile {
    std::mutex mutex;
    u32 slot{};
    u64 generation{};
    u32 access{};
    u32 share{};
    bool overlapped{};
    bool disconnected{};
    bool cleaned_up{};
    std::shared_ptr<hardware::GamepadBackgroundInputLease> input_lease;
    std::shared_ptr<HidCompletionSignal> completion_signal;
    // Typed by the completion owner; shared by aliases, never a manager pointer.
    std::shared_ptr<void> completion_route;
    std::atomic<u8> notification_modes{0};
    u32 capacity{hid_sdk::kDefaultInputBuffers};
    std::deque<gamepad_hid::Report> reports;
};
using HidOpenFileHolder = std::shared_ptr<HidOpenFile>;

class GamepadHidCatalog {
public:
    struct Device {
        u32 slot{};
        u64 generation{};
        u64 connection_id{};
        gamepad_hid::Report report{};
        [[nodiscard]] u64 raw_handle() const { return 0x40000000u | (static_cast<u32>(generation & 0x0fffffffull) << 2) | slot; }
        [[nodiscard]] std::string instance_id() const {
            // IG_ is Microsoft's documented dual-API duplicate-filter signal.
            // It classifies the shared XInput backing, not a vendor/USB identity
            // and not a promise that the suffix is an XInput user index.
            return "HID\\VITREO_GAMEPAD&IG_00\\"+std::to_string(slot)+"&"+std::to_string(generation);
        }
        [[nodiscard]] std::string path() const {
            auto id=instance_id();std::replace(id.begin(),id.end(),'\\','#');
            return "\\\\?\\"+id+"#{4d1e55b2-f16f-11cf-88cb-001111000030}";
        }
    };
    explicit GamepadHidCatalog(std::shared_ptr<hardware::GamepadHost> host) : m_host(std::move(host)) {}
    [[nodiscard]] bool acquire_input(const HidOpenFileHolder& file) {
        {std::lock_guard lock(file->mutex);if(file->input_lease) return true;}
        auto lease=acquire_native_input();if(!lease) return false;
        std::lock_guard lock(file->mutex);
        if(!file->input_lease) file->input_lease=std::move(lease);
        return true;
    }
    // Returns actual connected native snapshots; query failure never invents
    // an absent device or neutral input. Callers retain the preceding census.
    [[nodiscard]] std::vector<Device> devices() {
        std::lock_guard lock(m_mutex);
        for (u32 slot=0;slot<m_slots.size();++slot) {
            hardware::GamepadState state;
            m_query_ok[slot]=m_host && m_host->query_state(slot,state);
            if (!m_query_ok[slot]) continue;
            auto& previous=m_slots[slot];
            if (!state.connected) {
                if (previous) disconnect_locked(slot,previous->generation);
                previous.reset();
                continue;
            }
            if(previous && state.connection_id!=previous->connection_id) {
                disconnect_locked(slot,previous->generation);previous.reset();
            }
            const auto report=gamepad_hid::encode(state);
            const bool changed=!previous || previous->report!=report;
            if (!previous) previous=Device{slot,++m_generations[slot],state.connection_id,report};
            else previous->report=report;
            if (changed) {
                for (auto it=m_files.begin();it!=m_files.end();) {
                    if (auto file=it->lock()) {
                        std::lock_guard file_lock(file->mutex);
                        if (file->input_lease && file->slot==slot && file->generation==previous->generation && !file->disconnected && !file->cleaned_up) {
                            if (file->reports.size()>=file->capacity) file->reports.pop_front();
                            file->reports.push_back(report);
                        }
                        ++it;
                    } else it=m_files.erase(it);
                }
            }
        }
        std::vector<Device> result;
        for (const auto& slot:m_slots) if(slot) result.push_back(*slot);
        return result;
    }
    [[nodiscard]] HidOpenFileHolder open(std::string_view path,u32 access,bool overlapped,u32 share=3,bool* conflict=nullptr,bool* host_failed=nullptr) {
        auto lease=(access & (hid_sdk::kGENERIC_READ|hid_sdk::kGENERIC_ALL|hid_sdk::kFILE_READ_DATA)) ? acquire_native_input() : nullptr;
        const auto census=devices();
        for (const auto& device:census) {
            auto candidate=device.path();
            if (candidate.size()!=path.size() || !std::equal(candidate.begin(),candidate.end(),path.begin(),
                [](unsigned char a,unsigned char b) {return std::tolower(a)==std::tolower(b);})) continue;
            std::lock_guard lock(m_mutex);
            if(!m_slots[device.slot] || m_slots[device.slot]->generation!=device.generation) return {};
            if(!m_query_ok[device.slot]) {if(host_failed) *host_failed=true;return {};}
            std::erase_if(m_files,[](const auto& weak){return weak.expired();});
            for(auto& weak:m_files) if(auto existing=weak.lock()) {
                std::lock_guard file_lock(existing->mutex);
                if(existing->cleaned_up) continue;
                if(existing->slot!=device.slot || existing->generation!=device.generation) continue;
                const auto required=[](u32 rights) {
                    return ((rights & (hid_sdk::kGENERIC_READ|hid_sdk::kGENERIC_ALL|hid_sdk::kFILE_READ_DATA)) ? 1u:0u) |
                        ((rights & (hid_sdk::kGENERIC_WRITE|hid_sdk::kGENERIC_ALL|hid_sdk::kFILE_WRITE_DATA)) ? 2u:0u) |
                        ((rights & hid_sdk::kDELETE) ? 4u:0u);
                };
                if((required(access)&~existing->share) || (required(existing->access)&~share)) {
                    if(conflict) *conflict=true;
                    return {};
                }
            }
            auto file=std::make_shared<HidOpenFile>();
            file->slot=device.slot; file->generation=device.generation;
            file->access=access;file->share=share;file->overlapped=overlapped;
            file->input_lease=std::move(lease);
            if(file->input_lease) file->reports.push_back(m_slots[device.slot]->report);
            m_files.push_back(file);
            return file;
        }
        return {};
    }
    [[nodiscard]] std::optional<gamepad_hid::Report> snapshot(const HidOpenFileHolder& file,bool* host_failed=nullptr,bool* unsupported=nullptr) {
        // Metadata-only handles acquire monitoring only for this query.
        std::shared_ptr<hardware::GamepadBackgroundInputLease> lease;
        {std::lock_guard lock(file->mutex);lease=file->input_lease;}
        if(!lease) lease=acquire_native_input();
        if(!lease) {if(unsupported) *unsupported=true;return {};}
        (void)devices();
        std::lock_guard lock(m_mutex);
        if(!m_query_ok[file->slot]) {if(host_failed) *host_failed=true;return {};}
        const auto& device=m_slots[file->slot];
        if(device && device->generation==file->generation) return device->report;
        return {};
    }
private:
    [[nodiscard]] std::shared_ptr<hardware::GamepadBackgroundInputLease> acquire_native_input() {
        return m_host ? m_host->acquire_background_input() : nullptr;
    }
    void disconnect_locked(u32 slot,u64 generation) {
        for(auto& weak:m_files) if(auto file=weak.lock()) {
            std::lock_guard file_lock(file->mutex);
            if(file->slot==slot && file->generation==generation) {
                file->disconnected=true;file->reports.clear();
            }
        }
    }
    std::shared_ptr<hardware::GamepadHost> m_host;
    std::mutex m_mutex;
    std::array<std::optional<Device>,hid_sdk::kXUSER_MAX_COUNT> m_slots{};
    std::array<u64,hid_sdk::kXUSER_MAX_COUNT> m_generations{};
    std::array<bool,hid_sdk::kXUSER_MAX_COUNT> m_query_ok{};
    std::vector<std::weak_ptr<HidOpenFile>> m_files;
};
} // namespace vitreo::winapi
