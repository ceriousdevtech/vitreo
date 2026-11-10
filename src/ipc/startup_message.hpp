// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

#include "ipc/named_sync.hpp"
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unistd.h>

namespace vitreo::ipc {

inline constexpr const char* startup_message_environment = "VITREO_STARTUP_MESSAGE_EVENT";

// A creator owns this capsule-scoped event until its startup wait finishes.
// The child can only attach: a late notification after timeout cannot recreate
// an orphan. This is message retrieval, not the existence of a guest window.
class StartupMessageEvent {
public:
    StartupMessageEvent() {
        static std::atomic<u64> sequence{0};
        name_ = "Vitreo.StartupMessage." + std::to_string(::getpid()) + "." +
                std::to_string(sequence.fetch_add(1)) + "." + std::to_string(::arc4random());
        event_ = SharedSyncObject::create_or_open(name_, SyncKind::Event, 1, 0, true);
        if (event_ && !event_->was_created()) event_.reset();
    }
    [[nodiscard]] bool valid() const { return event_ != nullptr; }
    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] std::string environment_entry() const {
        return std::string(startup_message_environment) + "=" + name_;
    }
    u32 wait(u32 milliseconds, const std::atomic<bool>* child_exited = nullptr) {
        SharedSyncObject* object = event_.get();
        return wait_set(&object, 1, false, milliseconds, 0, child_exited);
    }
    static bool notify(const std::string& name) {
        auto event = SharedSyncObject::create_or_open(name, SyncKind::Event, 1, 0, false);
        if (!event) return false;
        (void)event->set_event();
        return true;
    }
private:
    std::string name_;
    std::shared_ptr<SharedSyncObject> event_;
};

inline void notify_startup_message() {
    static std::once_flag first_message;
    std::call_once(first_message, [] {
        if (const char* name = std::getenv(startup_message_environment); name && *name) {
            (void)StartupMessageEvent::notify(name);
        }
    });
}

} // namespace vitreo::ipc
