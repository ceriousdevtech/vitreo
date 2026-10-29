// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_sync.mm
/// @brief GPU/CPU frame synchronization via MTLSharedEvent + dispatch semaphore

#import <Metal/Metal.h>
#import <dispatch/dispatch.h>

#include <vitreo/common.hpp>
#include <cstdio>

namespace vitreo::graphics {

// =============================================================================
// SyncManager — triple-buffered frame sync
// =============================================================================

class SyncManager {
public:
    explicit SyncManager(void* device);  // id<MTLDevice>
    ~SyncManager();

    /// Wait on semaphore (blocks if 3 frames already in-flight)
    void begin_frame();
    /// Signal completed event and release semaphore
    void end_frame(void* cmd_buf);  // id<MTLCommandBuffer>

    u64 current_frame() const { return m_current_frame; }
    u64 completed_frame() const { return m_completed_frame; }

    /// Returns id<MTLSharedEvent> as void*
    void* shared_event() const { return m_shared_event; }

    SyncManager(const SyncManager&) = delete;
    SyncManager& operator=(const SyncManager&) = delete;

private:
    void* m_device;           // id<MTLDevice>
    void* m_shared_event;     // id<MTLSharedEvent>
    dispatch_semaphore_t m_semaphore;
    u64 m_current_frame = 0;
    u64 m_completed_frame = 0;
};

// =============================================================================
// Implementation
// =============================================================================

static constexpr int MAX_FRAMES_IN_FLIGHT = 3;

SyncManager::SyncManager(void* device)
    : m_device(device)
    , m_shared_event(nil)
    , m_semaphore(nullptr) {
    id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device;

    id<MTLSharedEvent> event = [mtl_device newSharedEvent];
    m_shared_event = (__bridge_retained void*)event;
    (void)m_device; // retained for future fence creation

    m_semaphore = dispatch_semaphore_create(MAX_FRAMES_IN_FLIGHT);

    VITREO_TRACE("[sync] SyncManager created: device=%p, max_frames_in_flight=%d\n",
                 device, MAX_FRAMES_IN_FLIGHT);
}

SyncManager::~SyncManager() {
    // Wait for all in-flight frames to finish
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        dispatch_semaphore_wait(m_semaphore, DISPATCH_TIME_FOREVER);
    }
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        dispatch_semaphore_signal(m_semaphore);
    }

    if (m_shared_event) {
        CFRelease(m_shared_event);
        m_shared_event = nil;
    }

    VITREO_TRACE("[sync] SyncManager destroyed\n");
}

void SyncManager::begin_frame() {
    dispatch_semaphore_wait(m_semaphore, DISPATCH_TIME_FOREVER);
    ++m_current_frame;

    VITREO_TRACE("[sync] begin_frame: frame=%llu\n", m_current_frame);
}

void SyncManager::end_frame(void* cmd_buf) {
    id<MTLCommandBuffer> buffer = (__bridge id<MTLCommandBuffer>)cmd_buf;
    id<MTLSharedEvent> event = (__bridge id<MTLSharedEvent>)m_shared_event;

    u64 frame = m_current_frame;
    dispatch_semaphore_t sem = m_semaphore;

    [buffer addCompletedHandler:^(id<MTLCommandBuffer> /* cb */) {
        m_completed_frame = frame;
        dispatch_semaphore_signal(sem);
    }];

    [buffer encodeSignalEvent:event value:frame];

    VITREO_TRACE("[sync] end_frame: signalling frame=%llu\n", frame);
}

} // namespace vitreo::graphics
