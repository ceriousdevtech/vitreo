#pragma once
/// @file winmm_mci.hpp
/// @brief MCI (mciSendCommand / mciSendString) with the cdaudio device.
///
/// The command set is Learn's (MCI commands and the "cdaudio" command-string
/// table); constants come from mciapi.h (mci_constants.inc) and the struct
/// layouts from the SDK compiled for both bitnesses (x64 MCI structs are
/// packed). What Learn leaves open is measured on Windows' cdaudio driver
/// over a mounted disc image (tools/oracle_probe/mci_cdaudio_probe.c,
/// tests/oracle_data/mci_cdaudio_probe_*.expected):
///   - lengths (disc and last track) are the lead-out distance minus 2
///     frames; milliseconds are frames * 1000 / 75 + 1; a TMSF length is
///     reported packed as MSF; positions start at 00:02:00 (LBA 0);
///   - a disc with no audio reports mode "not ready" until a seek;
///   - an out-of-range track fails with MCIERR_OUTOFRANGE and stores
///     0xFF000000 in dwReturn; an unknown device id or message is
///     MCIERR_UNSUPPORTED_FUNCTION; a second non-shareable open of the
///     drive is MCIERR_MUST_USE_SHAREABLE.
/// The disc is the capsule's optical mount (capsule::CdImage): a mixed-mode
/// image plays its audio tracks through a host WaveOutput stream.
///
/// Only cdaudio is implemented. Every other MCI device type fails to open
/// with MCIERR_CANNOT_LOAD_DRIVER and a one-time UNIMPLEMENTED line.

#include <vitreo/common.hpp>

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vitreo::capsule {
class CdImage;
class Vfs;
}
namespace vitreo::windowing {
class WaveOutput;
}

namespace vitreo::winapi {

class Marshaler;

class MciManager {
public:
    using OutputFactory = std::function<std::unique_ptr<windowing::WaveOutput>()>;
    using WindowNotifier =
        std::function<bool(u64 hwnd, u32 message, u64 wparam, u64 lparam)>;

    /// @p output_factory and @p notifier are the owner's hooks, read when used.
    MciManager(capsule::Vfs& vfs, const OutputFactory& output_factory,
               const WindowNotifier& notifier);
    ~MciManager();
    MciManager(const MciManager&) = delete;
    MciManager& operator=(const MciManager&) = delete;

    // Exports (A/W share one implementation).
    void impl_mciSendCommand(Marshaler& m, bool wide);
    void impl_mciSendString(Marshaler& m, bool wide);
    void impl_mciGetErrorString(Marshaler& m, bool wide);
    void impl_mciGetDeviceID(Marshaler& m, bool wide);
    void impl_mciGetDeviceIDFromElementID(Marshaler& m, bool wide);
    void impl_mciExecute(Marshaler& m);
    void impl_mciGetCreatorTask(Marshaler& m);
    void impl_mciGetYieldProc(Marshaler& m);
    void impl_mciSetYieldProc(Marshaler& m);

    /// A command in host form (what mciSendCommand's parameter block and a
    /// parsed command string both become).
    struct Request {
        u32 message = 0;
        u32 flags = 0;
        u64 callback = 0;
        // MCI_OPEN
        std::string device_type;
        u32 device_type_id = 0;
        std::string element;
        std::string alias;
        // MCI_STATUS / MCI_GETDEVCAPS / MCI_INFO / MCI_SYSINFO
        u32 item = 0;
        u32 track = 0;
        u32 number = 0;
        u32 sysinfo_type = 0;
        // MCI_SET
        u32 time_format = 0;
        u32 audio = 0;
        // MCI_PLAY / MCI_SEEK
        u32 from = 0;
        u32 to = 0;
    };
    /// What a command answers.
    struct Reply {
        enum class Kind {
            None, Integer, Bool, Mode, TimeFormat, Position, Length, TrackType,
            DeviceType, Text
        };
        Kind kind = Kind::None;
        u64 value = 0;
        std::string text;
        u32 time_format = 0;  ///< for Position / Length
        bool store_return = true;  ///< false: dwReturn is left untouched
    };

    /// Execute @p request on @p device_id (open: device_id is ignored and the
    /// new id is returned in reply.value). Returns the MCIERROR.
    [[nodiscard]] u32 dispatch(u32 device_id, Request& request, Reply& reply);
    /// mciSendString's whole contract on host strings.
    [[nodiscard]] u32 send_string(const std::string& command, std::string& result,
                                  u64 hwnd_callback);
    /// mciGetDeviceID on a host name.
    [[nodiscard]] u32 device_id_for_name(const std::string& name);
    /// mciGetErrorString text, or nullptr for a code with no message.
    [[nodiscard]] static const char* error_text(u32 code);

private:
    enum class Mode : u8 { NotReady, Stopped, Playing, Paused, Open };

    struct Player;
    struct Device {
        u32 id = 0;
        std::string name;  ///< what mciGetDeviceID / command strings use
        std::string element;
        char drive = 0;
        bool shareable = false;
        u32 time_format = 0;
        Mode mode = Mode::NotReady;
        bool door_open = false;
        bool positioned = false;  ///< a seek/play happened (data discs)
        u32 position = 0;         ///< LBA
        u32 play_to = 0;          ///< LBA, exclusive
        bool mute_left = false;
        bool mute_right = false;
        u64 notify_hwnd = 0;      ///< pending play notification
        bool notify_pending = false;
        u64 play_generation = 0;
        std::unique_ptr<Player> player;
    };

    [[nodiscard]] u32 open_device(Request& request, Reply& reply);
    [[nodiscard]] u32 device_command(Device& device, Request& request, Reply& reply,
                                     std::vector<std::pair<u64, u64>>& notices);
    [[nodiscard]] capsule::CdImage* disc_for(const Device& device);
    [[nodiscard]] u32 status(Device& device, const Request& request, Reply& reply);
    [[nodiscard]] u32 start_play(Device& device, u32 from, u32 to);
    void stop_player(Device& device);
    void release_retired();
    void finish_pending_notify(Device& device, u32 flag,
                               std::vector<std::pair<u64, u64>>& notices);
    void post_notices(const std::vector<std::pair<u64, u64>>& notices);
    [[nodiscard]] Device* find(u32 id);
    void close_device(u32 id, std::vector<std::pair<u64, u64>>& notices);
    [[nodiscard]] u32 parse_time(const Device& device, const std::string& text,
                                 u32& value) const;
    void player_progress(u32 device_id, u64 generation, u32 lba, bool finished);

    capsule::Vfs& m_vfs;
    const OutputFactory& m_output_factory;
    const WindowNotifier& m_notifier;
    std::recursive_mutex m_mutex;
    std::condition_variable_any m_play_done;
    std::map<u32, Device> m_devices;
    std::vector<std::unique_ptr<Player>> m_retired;
};

} // namespace vitreo::winapi
