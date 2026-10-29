#pragma once

#include <vitreo/common.hpp>

#include <functional>
#include <memory>
#include <span>

namespace vitreo::windowing {

struct WaveOutputFormat {
    u16 channels{0};
    u32 samples_per_second{0};
    u32 average_bytes_per_second{0};
    u16 block_align{0};
    u16 bits_per_sample{0};
};

/// Platform-neutral PCM output stream. Tokens are opaque to the backend and
/// are returned after their bytes have left the logical output queue.
class WaveOutput {
public:
    using CompletionCallback = std::function<void(u64 token, u32 bytes)>;

    virtual ~WaveOutput() = default;

    [[nodiscard]] virtual u32 device_count() const noexcept = 0;
    [[nodiscard]] virtual bool supports(const WaveOutputFormat& format) const noexcept = 0;
    [[nodiscard]] virtual bool open(const WaveOutputFormat& format,
                                    CompletionCallback completion) = 0;
    [[nodiscard]] virtual bool is_open() const noexcept = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool submit(u64 token, std::span<const Byte> pcm) = 0;
    virtual void reset() = 0;
    virtual void set_paused(bool paused) = 0;
    [[nodiscard]] virtual bool set_volume(float gain) = 0;
};

/// Create the platform wave output implementation. Production composition
/// calls this only from RuntimeContext; tests inject a fake WaveOutput.
[[nodiscard]] std::unique_ptr<WaveOutput> create_wave_output();

} // namespace vitreo::windowing
