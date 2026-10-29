/// @file ffmpeg_media_player.cpp
/// @brief Optional out-of-process media decoder feeding CoreAudio voices.

#include "audio/contracts/media_player.hpp"

#include "audio/contracts/audio_renderer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
#include <crt_externs.h>
#endif

namespace vitreo::audio {
namespace {

constexpr i64 REFERENCE_TIME_PER_SECOND = 10'000'000;
constexpr u32 OUTPUT_SAMPLE_RATE = 44'100;
constexpr u32 OUTPUT_FRAME_BYTES = 4;

[[nodiscard]] bool media_diag_enabled() {
    static const bool enabled = std::getenv("VITREO_MEDIA_DIAG") != nullptr;
    return enabled;
}

[[nodiscard]] char** host_environment() {
#if defined(__APPLE__)
    return *_NSGetEnviron();
#else
    extern char** environ;
    return environ;
#endif
}

[[nodiscard]] bool executable_exists(const char* path) {
    return path != nullptr && path[0] != '\0' && ::access(path, X_OK) == 0;
}

/// DirectShow-era WMA/WMV is decoded out of process. ffmpeg is a host
/// install: VITREO_FFMPEG, the configure-time path if it still exists,
/// then PATH. A binary copied to another Mac still works once ffmpeg is
/// installed there; we do not ship or sidecar the decoder.
[[nodiscard]] const std::string& ffmpeg_executable() {
    static const std::string path = []() -> std::string {
        if (const char* env = std::getenv("VITREO_FFMPEG");
            executable_exists(env)) {
            return env;
        }
#if defined(VITREO_FFMPEG_EXECUTABLE)
        if (executable_exists(VITREO_FFMPEG_EXECUTABLE)) {
            return VITREO_FFMPEG_EXECUTABLE;
        }
#endif
        if (const char* path_env = std::getenv("PATH")) {
            std::string remaining(path_env);
            usize start = 0;
            while (start < remaining.size()) {
                const usize colon = remaining.find(':', start);
                const std::string dir = remaining.substr(
                    start, colon == std::string::npos ? std::string::npos
                                                      : colon - start);
                if (!dir.empty()) {
                    const std::string candidate = dir + "/ffmpeg";
                    if (executable_exists(candidate.c_str())) return candidate;
                }
                if (colon == std::string::npos) break;
                start = colon + 1;
            }
        }
        // Homebrew prefixes: macOS GUI launches often have a PATH that
        // omits these even after `brew install ffmpeg`.
        for (const char* hint : {"/usr/local/bin/ffmpeg",
                                 "/opt/homebrew/bin/ffmpeg"}) {
            if (executable_exists(hint)) return hint;
        }
        return {};
    }();
    return path;
}

void log_missing_ffmpeg_once() {
    static const bool logged = [] {
        std::fprintf(stderr,
                     "[media] ffmpeg is not installed (not on PATH, and "
                     "VITREO_FFMPEG is unset). WMA/WMV DirectShow graphs "
                     "will return VFW_E_CANNOT_RENDER.\n");
        return true;
    }();
    (void)logged;
}

[[nodiscard]] i64 ffmpeg_duration_100ns(std::string_view text) {
    const auto pos = text.find("Duration:");
    if (pos == std::string_view::npos) return 0;
    const char* p = text.data() + pos + 9;
    while (*p == ' ' || *p == '\t') ++p;
    int hours = 0;
    int minutes = 0;
    double seconds = 0.0;
    if (std::sscanf(p, "%d:%d:%lf", &hours, &minutes, &seconds) != 3) {
        return 0;
    }
    if (hours < 0 || minutes < 0 || minutes >= 60 || seconds < 0.0) {
        return 0;
    }
    const double total =
        static_cast<double>(hours * 3600 + minutes * 60) + seconds;
    if (total <= 0.0 || total > 24.0 * 3600.0) return 0;
    return static_cast<i64>(
        total * static_cast<double>(REFERENCE_TIME_PER_SECOND) + 0.5);
}

[[nodiscard]] bool probe_video_stream(const std::string& path, u32& width,
                                      u32& height, double& fps,
                                      i64& duration_100ns, bool& has_audio) {
    duration_100ns = 0;
    has_audio = false;
    const std::string& ffmpeg = ffmpeg_executable();
    if (ffmpeg.empty()) {
        log_missing_ffmpeg_once();
        return false;
    }
    int err_pipe[2] = {-1, -1};
    if (::pipe(err_pipe) != 0) return false;
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        return false;
    }
    (void)posix_spawn_file_actions_addopen(
        &actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    (void)posix_spawn_file_actions_addopen(
        &actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    (void)posix_spawn_file_actions_adddup2(
        &actions, err_pipe[1], STDERR_FILENO);
    (void)posix_spawn_file_actions_addclose(&actions, err_pipe[0]);
    (void)posix_spawn_file_actions_addclose(&actions, err_pipe[1]);

    std::vector<char> mutable_path(path.begin(), path.end());
    mutable_path.push_back('\0');
    std::vector<char> executable(ffmpeg.begin(), ffmpeg.end());
    executable.push_back('\0');
    char* const arguments[] = {
        executable.data(),
        const_cast<char*>("-nostdin"),
        const_cast<char*>("-hide_banner"),
        const_cast<char*>("-i"),
        mutable_path.data(),
        nullptr,
    };
    pid_t child = -1;
    const int spawned = ::posix_spawn(
        &child, executable.data(), &actions, nullptr, arguments,
        host_environment());
    posix_spawn_file_actions_destroy(&actions);
    ::close(err_pipe[1]);
    if (spawned != 0) {
        ::close(err_pipe[0]);
        return false;
    }

    std::string text;
    std::array<char, 4096> buf{};
    for (;;) {
        const ssize_t n = ::read(err_pipe[0], buf.data(), buf.size());
        if (n > 0) {
            text.append(buf.data(), static_cast<usize>(n));
            continue;
        }
        if (n == 0) break;
        if (errno == EINTR) continue;
        break;
    }
    ::close(err_pipe[0]);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}

    duration_100ns = ffmpeg_duration_100ns(text);
    has_audio = text.find("Audio:") != std::string::npos;
    const char* video = std::strstr(text.c_str(), "Video:");
    if (video == nullptr) return false;
    const char* line_end = std::strchr(video, '\n');
    const std::string line(
        video, line_end != nullptr
                   ? static_cast<usize>(line_end - video)
                   : std::strlen(video));
    width = 0;
    height = 0;
    fps = 30.0;
    for (usize i = 0; i + 2 < line.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(line[i]))) continue;
        char* end = nullptr;
        const unsigned long w =
            std::strtoul(line.data() + i, &end, 10);
        if (end == nullptr || *end != 'x' ||
            !std::isdigit(static_cast<unsigned char>(end[1]))) {
            continue;
        }
        char* end_h = nullptr;
        const unsigned long h = std::strtoul(end + 1, &end_h, 10);
        if (w < 2 || h < 2 || w > 8192 || h > 8192) continue;
        width = static_cast<u32>(w);
        height = static_cast<u32>(h);
        break;
    }
    if (width == 0 || height == 0) return false;
    const char* fps_tag = std::strstr(line.data(), " fps");
    if (fps_tag != nullptr) {
        const char* p = fps_tag;
        while (p > line.data() &&
               (std::isdigit(static_cast<unsigned char>(p[-1])) ||
                p[-1] == '.')) {
            --p;
        }
        const double parsed = std::strtod(p, nullptr);
        if (parsed >= 1.0 && parsed <= 120.0) fps = parsed;
    }
    return true;
}

class FfmpegMediaPlayer final : public IMediaPlayer {
public:
    ~FfmpegMediaPlayer() override {
        stop_video_decoder();
        if (m_worker.joinable()) {
            m_worker.request_stop();
            const pid_t child = m_child.load(std::memory_order_acquire);
            if (child > 0) (void)::kill(child, SIGTERM);
            m_worker.join();
        }
    }

    [[nodiscard]] bool open(std::string_view host_path) override {
        if (host_path.empty() || m_worker.joinable()) return false;
        if (ffmpeg_executable().empty()) {
            log_missing_ffmpeg_once();
            return false;
        }

        const std::string path(host_path);
        m_host_path = path;
        u32 video_w = 0;
        u32 video_h = 0;
        double video_fps = 30.0;
        i64 probed_duration = 0;
        bool has_audio = false;
        m_has_video = probe_video_stream(path, video_w, video_h, video_fps,
                                         probed_duration, has_audio);
        m_has_audio = has_audio;
        if (m_has_video) {
            m_video_width = video_w;
            m_video_height = video_h;
            m_video_fps = video_fps;
        }
        if (probed_duration > 0) {
            m_duration_100ns = probed_duration;
        }

        // A video-only source has no audio filter to fail. A present audio
        // stream whose decoder or device fails must not be called successful.
        if (!has_audio) {
            if (!m_has_video || !start_video_decoder(path) ||
                !wait_for_preroll_frame(std::chrono::seconds(2))) {
                m_phase.store(Phase::Failed, std::memory_order_release);
                return false;
            }
            m_phase.store(Phase::Ready, std::memory_order_release);
            return true;
        }

        int output_pipe[2] = {-1, -1};
        if (::pipe(output_pipe) != 0) return false;

        posix_spawn_file_actions_t actions;
        if (posix_spawn_file_actions_init(&actions) != 0) {
            ::close(output_pipe[0]);
            ::close(output_pipe[1]);
            return false;
        }
        (void)posix_spawn_file_actions_addopen(
            &actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        (void)posix_spawn_file_actions_adddup2(
            &actions, output_pipe[1], STDOUT_FILENO);
        (void)posix_spawn_file_actions_addopen(
            &actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        (void)posix_spawn_file_actions_addclose(&actions, output_pipe[0]);
        (void)posix_spawn_file_actions_addclose(&actions, output_pipe[1]);

        const std::string& ffmpeg = ffmpeg_executable();
        std::vector<char> mutable_path(path.begin(), path.end());
        mutable_path.push_back('\0');
        std::vector<char> executable(ffmpeg.begin(), ffmpeg.end());
        executable.push_back('\0');
        char* const arguments[] = {
            executable.data(),
            const_cast<char*>("-nostdin"),
            const_cast<char*>("-v"),
            const_cast<char*>("error"),
            const_cast<char*>("-i"),
            mutable_path.data(),
            const_cast<char*>("-vn"),
            const_cast<char*>("-f"),
            const_cast<char*>("s16le"),
            const_cast<char*>("-acodec"),
            const_cast<char*>("pcm_s16le"),
            const_cast<char*>("-ac"),
            const_cast<char*>("2"),
            const_cast<char*>("-ar"),
            const_cast<char*>("44100"),
            const_cast<char*>("pipe:1"),
            nullptr,
        };

        pid_t child = -1;
        const int result = ::posix_spawn(
            &child, executable.data(), &actions, nullptr, arguments,
            host_environment());
        posix_spawn_file_actions_destroy(&actions);
        ::close(output_pipe[1]);
        if (result != 0) {
            ::close(output_pipe[0]);
            return false;
        }

        m_child.store(child, std::memory_order_release);
        m_phase.store(Phase::Loading, std::memory_order_release);
        if (media_diag_enabled()) {
            std::fprintf(stderr,
                         "[media-diag] decoder-start pid=%d path='%s' "
                         "video=%ux%u fps=%.3f\n",
                         child, path.c_str(), m_video_width, m_video_height,
                         m_video_fps);
        }
        m_worker = std::jthread([this, read_fd = output_pipe[0], child](
                                    std::stop_token stop) {
            decode_worker(read_fd, child, stop);
        });
        if (m_has_video && !start_video_decoder(path)) {
            m_phase.store(Phase::Failed, std::memory_order_release);
            return false;
        }
        // DirectShow Pause/Run prerolls until the renderer holds a picture.
        if (m_has_video) {
            (void)wait_for_preroll_frame(std::chrono::seconds(2));
        }
        return true;
    }

    [[nodiscard]] MediaStartResult run() override {
        std::lock_guard<std::mutex> command(m_command_mutex);
        if (m_has_video) {
            (void)wait_for_preroll_frame(std::chrono::seconds(2));
        }
        std::unique_lock<std::mutex> lock(m_mutex);
        const Phase phase = m_phase.load(std::memory_order_acquire);
        if (m_want_running) {
            // Learn Run: an already running graph is not restarted.
            if (phase == Phase::Ready) return MediaStartResult::Started;
            if (phase == Phase::Failed) return MediaStartResult::Failed;
            return MediaStartResult::Pending;
        }
        ++m_clock_generation;
        m_want_running = true;
        m_completion_reported = false;
        auto finish_start = [&](MediaStartResult result) {
            // Sink startup can block inside the host device. The state lock
            // is released for it, but stream time starts only when startup
            // returns (Run / EC_COMPLETE contracts).
            if (result != MediaStartResult::Failed && !m_run_origin_set) {
                m_run_origin = std::chrono::steady_clock::now();
                m_run_origin_set = true;
            }
            if (result == MediaStartResult::Failed) {
                ++m_clock_generation;
                m_want_running = false;
            }
            refresh_completion_locked();
            m_video_cv.notify_all();
            return result;
        };
        if (phase == Phase::Failed) {
            return finish_start(MediaStartResult::Failed);
        }
        if (phase != Phase::Ready) return finish_start(MediaStartResult::Pending);
        if (m_at_end) return finish_start(MediaStartResult::Started);
        if (m_voice == nullptr) return finish_start(
            m_has_video ? MediaStartResult::Started : MediaStartResult::Failed);
        auto* voice = m_voice.get();
        lock.unlock();
        const bool started = voice->play(false);
        lock.lock();
        if (!started) return finish_start(MediaStartResult::Failed);
        m_playback_started = true;
        return finish_start(MediaStartResult::Started);
    }

    void pause() override {
        std::lock_guard<std::mutex> command(m_command_mutex);
        const i64 position = position_100ns();
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_want_running) return;
        ++m_clock_generation;
        m_pending_position_100ns = position;
        m_want_running = false;
        m_run_origin_set = false;
        auto* voice = m_voice.get();
        lock.unlock();
        if (voice != nullptr && voice->is_playing()) voice->pause();
        if (!m_has_video && voice != nullptr) {
            const i64 position = bytes_to_position(voice->play_cursor());
            lock.lock();
            m_pending_position_100ns = position;
        }
    }

    void stop() override {
        std::lock_guard<std::mutex> command(m_command_mutex);
        const i64 position = position_100ns();
        std::unique_lock<std::mutex> lock(m_mutex);
        // Learn Stop/get_CurrentPosition: stopping retains the resume point.
        ++m_clock_generation;
        m_pending_position_100ns = position;
        m_want_running = false;
        m_playback_started = false;
        m_at_end = false;
        m_completion_pending = 0;
        m_completion_reported = false;
        m_run_origin_set = false;
        auto* voice = m_voice.get();
        const u32 byte_position = position_to_bytes(m_pending_position_100ns);
        lock.unlock();
        if (voice != nullptr) {
            if (voice->is_playing()) voice->pause();
            const u32 paused_cursor = voice->play_cursor();
            voice->stop();
            voice->set_position(m_has_video ? byte_position : paused_cursor);
            if (!m_has_video) {
                lock.lock();
                m_pending_position_100ns = bytes_to_position(paused_cursor);
            }
        }
        m_video_cv.notify_all();
    }

    [[nodiscard]] bool ready() const override {
        return m_phase.load(std::memory_order_acquire) == Phase::Ready;
    }

    [[nodiscard]] bool failed() const override {
        return m_phase.load(std::memory_order_acquire) == Phase::Failed;
    }

    [[nodiscard]] bool take_completed() override {
        std::unique_lock<std::mutex> lock(m_mutex);
        i64 position = clock_position_locked();
        const u64 generation = m_clock_generation;
        if (!m_has_video && m_want_running && m_voice != nullptr) {
            auto* voice = m_voice.get();
            lock.unlock();
            position = bytes_to_position(voice->play_cursor());
            lock.lock();
        }
        // A seek or state command during the external cursor query invalidates
        // its result. Retain already queued events, but never invent a new EOS
        // for the new stream position from an old cursor sample.
        if (generation == m_clock_generation) refresh_completion_locked(position);
        if (m_completion_pending == 0) return false;
        --m_completion_pending;
        return true;
    }

    void set_volume(i32 hundredth_db) override {
        std::lock_guard<std::mutex> command(m_command_mutex);
        std::unique_lock<std::mutex> lock(m_mutex);
        m_volume = std::clamp(hundredth_db, -10000, 0);
        auto* voice = m_voice.get();
        const i32 value = m_volume;
        lock.unlock();
        if (voice != nullptr) voice->set_volume(value);
    }

    void set_balance(i32 hundredth_db) override {
        std::lock_guard<std::mutex> command(m_command_mutex);
        std::unique_lock<std::mutex> lock(m_mutex);
        m_balance = std::clamp(hundredth_db, -10000, 10000);
        auto* voice = m_voice.get();
        const i32 value = m_balance;
        lock.unlock();
        if (voice != nullptr) voice->set_pan(value);
    }

    [[nodiscard]] bool seek_100ns(i64 position) override {
        std::lock_guard<std::mutex> command(m_command_mutex);
        std::unique_lock<std::mutex> lock(m_mutex);
        const i64 clamped = std::clamp<i64>(position, 0, m_duration_100ns);
        const bool running = m_want_running;
        ++m_clock_generation;
        m_want_running = false;
        m_pending_position_100ns = clamped;
        m_at_end = clamped >= m_duration_100ns && m_duration_100ns != 0;
        m_completion_pending = 0;
        m_completion_reported = false;
        m_run_origin_set = false;
        auto* voice = m_voice.get();
        const u32 byte_position = position_to_bytes(clamped);
        const bool at_end = m_at_end;
        lock.unlock();
        if (voice != nullptr) {
            if (voice->is_playing()) voice->pause();
            voice->set_position(byte_position);
        }
        if (m_has_video) {
            stop_video_decoder();
            {
                std::lock_guard<std::mutex> state(m_mutex);
                m_video_ring.clear();
            }
            if (!at_end && (!start_video_decoder(m_host_path, clamped) ||
                !wait_for_preroll_frame(std::chrono::seconds(2)))) {
                m_phase.store(Phase::Failed, std::memory_order_release);
                return false;
            }
        }
        const bool started = !running || at_end || voice == nullptr || voice->play(false);
        lock.lock();
        if (!started) {
            m_phase.store(Phase::Failed, std::memory_order_release);
            return false;
        }
        m_want_running = running;
        if (running) {
            m_run_origin = std::chrono::steady_clock::now();
            m_run_origin_set = true;
            refresh_completion_locked();
        }
        m_video_cv.notify_all();
        return true;
    }

    [[nodiscard]] i64 position_100ns() const override {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_has_video || !m_want_running || m_voice == nullptr)
            return clock_position_locked();
        // The device lives until this player's destructor; publication is
        // protected by m_mutex and command callbacks never replace it.
        auto* voice = m_voice.get();
        const i64 duration = m_duration_100ns;
        lock.unlock();
        const i64 position = bytes_to_position(voice->play_cursor());
        return duration > 0 ? std::clamp<i64>(position, 0, duration) : position;
    }

    [[nodiscard]] i64 duration_100ns() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_duration_100ns;
    }

    [[nodiscard]] bool has_video() const override { return m_has_video; }
    [[nodiscard]] bool has_audio() const override { return m_has_audio; }

    void native_video_size(u32& width, u32& height) const override {
        width = m_video_width;
        height = m_video_height;
    }

    [[nodiscard]] bool copy_video_frame(std::vector<u8>& rgba, u32& width,
                                        u32& height,
                                        u64& frame_index) const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_has_video || m_video_ring.empty()) return false;
        u64 want = 0;
        if (m_video_fps > 0.0) {
            const i64 pos = clock_position_locked();
            want = static_cast<u64>(
                static_cast<double>(std::max<i64>(pos, 0)) * m_video_fps /
                static_cast<double>(REFERENCE_TIME_PER_SECOND));
        }
        while (m_video_ring.size() > 1 && m_video_ring[1].index <= want) {
            m_video_ring.pop_front();
        }
        rgba = m_video_ring.front().rgba;
        width = m_video_width;
        height = m_video_height;
        frame_index = m_video_ring.front().index;
        m_video_cv.notify_all();
        return true;
    }

private:
    enum class Phase : u8 { Empty, Loading, Ready, Failed };

    void decode_worker(int read_fd, pid_t child, std::stop_token stop) {
        std::vector<u8> decoded;
        std::array<u8, 64 * 1024> buffer{};
        bool read_ok = true;
        for (;;) {
            const ssize_t count = ::read(read_fd, buffer.data(), buffer.size());
            if (count > 0) {
                decoded.insert(decoded.end(), buffer.begin(),
                               buffer.begin() + count);
                continue;
            }
            if (count == 0) break;
            if (errno == EINTR) {
                if (stop.stop_requested()) break;
                continue;
            }
            read_ok = false;
            break;
        }
        ::close(read_fd);

        int status = 0;
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        m_child.store(-1, std::memory_order_release);
        if (stop.stop_requested()) return;

        const usize aligned_size = decoded.size() &
                                   ~static_cast<usize>(OUTPUT_FRAME_BYTES - 1u);
        decoded.resize(aligned_size);
        if (!read_ok || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
            decoded.empty() || decoded.size() > UINT32_MAX) {
            if (media_diag_enabled()) {
                std::fprintf(stderr,
                             "[media-diag] decoder-failed pid=%d status=%d "
                             "bytes=%zu\n",
                             child, status, decoded.size());
            }
            m_phase.store(Phase::Failed, std::memory_order_release);
            return;
        }

        auto renderer = create_audio_renderer();
        const PcmFormat format{OUTPUT_SAMPLE_RATE, 2, 16};
        auto voice = renderer != nullptr ? renderer->create_voice(
            format, decoded.data(), static_cast<u32>(decoded.size())) : nullptr;
        if (voice == nullptr) {
            m_phase.store(Phase::Failed, std::memory_order_release);
            return;
        }
        std::lock_guard<std::mutex> command(m_command_mutex);
        std::unique_lock<std::mutex> lock(m_mutex);
        m_pcm = std::move(decoded); // the device's PCM pointers remain stable
        const i64 pcm_duration = bytes_to_position(static_cast<u32>(m_pcm.size()));
        if (m_duration_100ns == 0 && pcm_duration > 0) m_duration_100ns = pcm_duration;
        m_renderer = std::move(renderer);
        m_voice = std::move(voice);
        auto* active_voice = m_voice.get();
        const i32 volume = m_volume;
        const i32 balance = m_balance;
        const u32 position = position_to_bytes(m_pending_position_100ns);
        const bool running = m_want_running && !m_at_end;
        lock.unlock();
        active_voice->set_volume(volume);
        active_voice->set_pan(balance);
        active_voice->set_position(position);
        const bool started_playback = !running || active_voice->play(false);
        lock.lock();
        m_playback_started = running && started_playback;
        m_phase.store(started_playback ? Phase::Ready : Phase::Failed,
                      std::memory_order_release);
        m_video_cv.notify_all();
    }

    [[nodiscard]] bool wait_for_preroll_frame(
        std::chrono::milliseconds timeout) {
        if (!m_has_video) return true;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::unique_lock<std::mutex> lock(m_mutex);
        while (m_video_ring.empty()) {
            if (m_video_cv.wait_until(lock, deadline) ==
                std::cv_status::timeout) {
                break;
            }
        }
        const bool ready = !m_video_ring.empty();
        if (!ready) {
            std::fprintf(stderr,
                         "[media] video preroll empty %ux%u after %lld ms\n",
                         m_video_width, m_video_height,
                         static_cast<long long>(timeout.count()));
        }
        return ready;
    }

    void stop_video_decoder() {
        if (!m_video_worker.joinable()) return;
        m_video_worker.request_stop();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_video_cv.notify_all();
        }
        const pid_t child = m_video_child.load(std::memory_order_acquire);
        if (child > 0) (void)::kill(child, SIGTERM);
        m_video_worker.join();
    }

    [[nodiscard]] bool start_video_decoder(const std::string& path, i64 position = 0) {
        if (m_video_width == 0 || m_video_height == 0) return false;
        if (ffmpeg_executable().empty()) {
            log_missing_ffmpeg_once();
            return false;
        }
        int output_pipe[2] = {-1, -1};
        if (::pipe(output_pipe) != 0) return false;
        posix_spawn_file_actions_t actions;
        if (posix_spawn_file_actions_init(&actions) != 0) {
            ::close(output_pipe[0]);
            ::close(output_pipe[1]);
            return false;
        }
        (void)posix_spawn_file_actions_addopen(
            &actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        (void)posix_spawn_file_actions_adddup2(
            &actions, output_pipe[1], STDOUT_FILENO);
        (void)posix_spawn_file_actions_addclose(&actions, output_pipe[0]);
        (void)posix_spawn_file_actions_addclose(&actions, output_pipe[1]);

        const std::string& ffmpeg = ffmpeg_executable();
        std::vector<char> mutable_path(path.begin(), path.end());
        mutable_path.push_back('\0');
        std::vector<char> executable(ffmpeg.begin(), ffmpeg.end());
        executable.push_back('\0');
        char size_arg[32];
        std::snprintf(size_arg, sizeof(size_arg), "%ux%u", m_video_width,
                      m_video_height);
        char seek_arg[48];
        std::snprintf(seek_arg, sizeof(seek_arg), "%.7f",
                      static_cast<double>(position) / REFERENCE_TIME_PER_SECOND);
        char* const arguments[] = {
            executable.data(),
            const_cast<char*>("-nostdin"),
            const_cast<char*>("-hide_banner"),
            const_cast<char*>("-v"),
            const_cast<char*>("error"),
            // FFmpeg's default accurate input seek decodes/discards preroll.
            const_cast<char*>("-ss"), seek_arg,
            const_cast<char*>("-i"),
            mutable_path.data(),
            const_cast<char*>("-an"),
            const_cast<char*>("-map"),
            const_cast<char*>("0:v:0"),
            const_cast<char*>("-s"),
            size_arg,
            const_cast<char*>("-f"),
            const_cast<char*>("rawvideo"),
            const_cast<char*>("-pix_fmt"),
            const_cast<char*>("rgba"),
            const_cast<char*>("pipe:1"),
            nullptr,
        };
        pid_t child = -1;
        const int result = ::posix_spawn(
            &child, executable.data(), &actions, nullptr, arguments,
            host_environment());
        posix_spawn_file_actions_destroy(&actions);
        ::close(output_pipe[1]);
        if (result != 0) {
            ::close(output_pipe[0]);
            return false;
        }
        m_video_child.store(child, std::memory_order_release);
        const u64 first_index = static_cast<u64>(
            static_cast<double>(position) * m_video_fps / REFERENCE_TIME_PER_SECOND);
        m_video_worker = std::jthread([this, read_fd = output_pipe[0], child, first_index](
                                          std::stop_token stop) {
            video_decode_worker(read_fd, child, stop, first_index);
        });
        return true;
    }

    void video_decode_worker(int read_fd, pid_t child, std::stop_token stop, u64 first_index) {
        const usize frame_bytes = static_cast<usize>(m_video_width) *
                                  static_cast<usize>(m_video_height) * 4u;
        if (frame_bytes == 0) {
            ::close(read_fd);
            return;
        }
        std::vector<u8> frame(frame_bytes);
        usize filled = 0;
        u64 index = first_index;
        while (!stop.stop_requested()) {
            const ssize_t n = ::read(read_fd, frame.data() + filled,
                                     frame_bytes - filled);
            if (n > 0) {
                filled += static_cast<usize>(n);
                if (filled < frame_bytes) continue;
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_video_cv.wait(lock, [&] {
                        return stop.stop_requested() ||
                               m_video_ring.size() < 8;
                    });
                    if (stop.stop_requested()) break;
                    m_video_ring.push_back(
                        VideoFrame{index, std::move(frame)});
                    ++index;
                    frame = std::vector<u8>(frame_bytes);
                    filled = 0;
                    m_video_cv.notify_all();
                }
                continue;
            }
            if (n == 0) break;
            if (errno == EINTR) continue;
            break;
        }
        ::close(read_fd);
        int status = 0;
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        m_video_child.store(-1, std::memory_order_release);
    }

    [[nodiscard]] i64 clock_position_locked() const {
        // A connected video renderer is the graph clock. An audio voice that
        // has not started (or has already drained a short PCM buffer) must
        // not pin the picture on frame 0 or report EOS.
        if (m_has_video) {
            i64 pos = m_pending_position_100ns;
            if (m_want_running && m_run_origin_set) {
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - m_run_origin);
                pos += elapsed.count() * 10;
            }
            if (m_duration_100ns > 0 && pos > m_duration_100ns) {
                pos = m_duration_100ns;
            }
            return pos;
        }
        return m_pending_position_100ns;
    }

    void refresh_completion_locked(i64 position = -1) {
        if (!m_want_running || m_completion_reported) {
            return;
        }
        // Learn EC_COMPLETE: a renderer finished. !is_playing() is not EOS —
        // CoreAudio is often not playing in the first milliseconds after Run,
        // and a WMV audio voice must not complete a video graph.
        if (m_duration_100ns <= 0) {
            return;
        }
        if (m_has_video && !m_run_origin_set) {
            return;
        }
        if (!m_has_video && m_voice == nullptr) {
            return;
        }
        if (position < 0) position = clock_position_locked();
        if (position >= m_duration_100ns) {
            m_pending_position_100ns = m_duration_100ns;
            m_at_end = true;
            ++m_completion_pending;
            m_completion_reported = true;
        }
    }

    [[nodiscard]] u32 position_to_bytes(i64 position) const {
        if (m_pcm.empty()) return 0;
        const i64 frames = position * OUTPUT_SAMPLE_RATE /
                           REFERENCE_TIME_PER_SECOND;
        const u64 bytes = static_cast<u64>(std::max<i64>(frames, 0)) *
                          OUTPUT_FRAME_BYTES;
        const u64 last = m_pcm.size() >= OUTPUT_FRAME_BYTES
                             ? m_pcm.size() - OUTPUT_FRAME_BYTES
                             : 0u;
        return static_cast<u32>(std::min(bytes, last));
    }

    [[nodiscard]] static i64 bytes_to_position(u32 bytes) {
        return static_cast<i64>(bytes / OUTPUT_FRAME_BYTES) *
               REFERENCE_TIME_PER_SECOND / OUTPUT_SAMPLE_RATE;
    }

    struct VideoFrame {
        u64 index{0};
        std::vector<u8> rgba;
    };

    std::jthread m_worker;
    std::jthread m_video_worker;
    std::atomic<pid_t> m_child{-1};
    std::atomic<pid_t> m_video_child{-1};
    std::atomic<Phase> m_phase{Phase::Empty};
    std::mutex m_command_mutex;
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_video_cv;
    std::vector<u8> m_pcm;
    std::unique_ptr<IAudioRenderer> m_renderer;
    std::unique_ptr<IAudioVoice> m_voice;
    i64 m_duration_100ns{0};
    i64 m_pending_position_100ns{0};
    u64 m_clock_generation{0};
    bool m_want_running{false};
    bool m_playback_started{false};
    bool m_at_end{false};
    u64 m_completion_pending{0};
    bool m_completion_reported{false};
    bool m_has_video{false};
    bool m_has_audio{false};
    std::string m_host_path;
    bool m_run_origin_set{false};
    std::chrono::steady_clock::time_point m_run_origin{};
    u32 m_video_width{0};
    u32 m_video_height{0};
    double m_video_fps{30.0};
    mutable std::deque<VideoFrame> m_video_ring;
    i32 m_volume{0};
    i32 m_balance{0};
};

} // namespace

std::unique_ptr<IMediaPlayer> create_media_player() {
    if (ffmpeg_executable().empty()) {
        log_missing_ffmpeg_once();
        return nullptr;
    }
    return std::make_unique<FfmpegMediaPlayer>();
}

} // namespace vitreo::audio
