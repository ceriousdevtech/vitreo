#include "checkpoint_format.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <limits>

namespace vitreo::checkpoint {
namespace {

constexpr std::array<u8, 8> kMagic{'V', 'T', 'R', 'M', 'C', 'K', 'P', 'T'};
constexpr u64 kFnvOffset = 1469598103934665603ULL;
constexpr u64 kFnvPrime = 1099511628211ULL;
constexpr u64 kMaxRecords = 16ULL * 1024ULL * 1024ULL;
constexpr u64 kMaxBlob = 64ULL * 1024ULL * 1024ULL;

u64 checksum(std::span<const u8> bytes) {
    u64 value = kFnvOffset;
    for (u8 byte : bytes) {
        value ^= byte;
        value *= kFnvPrime;
    }
    return value;
}

class Writer {
public:
    template<typename T>
    void integer(T value) {
        using U = std::make_unsigned_t<T>;
        U bits = static_cast<U>(value);
        for (usize i = 0; i < sizeof(T); ++i) {
            bytes.push_back(static_cast<u8>(bits >> (i * 8)));
        }
    }

    void blob(std::span<const u8> value) {
        integer<u64>(value.size());
        bytes.insert(bytes.end(), value.begin(), value.end());
    }

    void string(std::string_view value) {
        blob(std::span<const u8>(
            reinterpret_cast<const u8*>(value.data()), value.size()));
    }

    std::vector<u8> bytes;
};

class Reader {
public:
    explicit Reader(std::span<const u8> source) : bytes(source) {}

    template<typename T>
    bool integer(T& value) {
        if (remaining() < sizeof(T)) return false;
        using U = std::make_unsigned_t<T>;
        U bits = 0;
        for (usize i = 0; i < sizeof(T); ++i) {
            bits |= static_cast<U>(bytes[position++]) << (i * 8);
        }
        value = static_cast<T>(bits);
        return true;
    }

    bool blob(std::vector<u8>& value, u64 maximum = kMaxBlob) {
        u64 size = 0;
        if (!integer(size) || size > maximum || size > remaining()) return false;
        value.assign(bytes.begin() + static_cast<isize>(position),
                     bytes.begin() + static_cast<isize>(position + size));
        position += static_cast<usize>(size);
        return true;
    }

    bool string(std::string& value) {
        std::vector<u8> storage;
        if (!blob(storage, 1024 * 1024)) return false;
        value.assign(reinterpret_cast<const char*>(storage.data()), storage.size());
        return true;
    }

    [[nodiscard]] usize remaining() const { return bytes.size() - position; }

private:
    std::span<const u8> bytes;
    usize position{0};
};

template<typename Container, typename DecodeOne>
bool read_records(Reader& reader, Container& output, DecodeOne decode_one) {
    u64 count = 0;
    if (!reader.integer(count) || count > kMaxRecords) return false;
    output.clear();
    output.reserve(static_cast<usize>(count));
    for (u64 i = 0; i < count; ++i) {
        typename Container::value_type item;
        if (!decode_one(item)) return false;
        output.push_back(std::move(item));
    }
    return true;
}

} // namespace

bool encode(const Checkpoint& checkpoint, std::vector<u8>& bytes,
            std::string& error) {
    if (checkpoint.format_version != kFormatVersion) {
        error = "unsupported checkpoint version for writing";
        return false;
    }

    Writer payload;
    payload.integer<u8>(static_cast<u8>(checkpoint.architecture));
    payload.integer<u64>(checkpoint.determinism.virtual_time_ns);
    payload.integer<u64>(checkpoint.determinism.rng_seed);
    payload.integer<u64>(checkpoint.determinism.retired_instructions);

    payload.integer<u64>(checkpoint.pages.size());
    for (const auto& page : checkpoint.pages) {
        if ((page.address % kGuestPageSize) != 0 ||
            (page.has_contents && page.contents.size() != kGuestPageSize) ||
            (!page.has_contents && !page.contents.empty())) {
            error = "invalid memory page record";
            return false;
        }
        payload.integer<u64>(page.address);
        payload.integer<u8>(static_cast<u8>(page.protection));
        payload.integer<u8>(page.has_contents ? 1 : 0);
        payload.blob(page.contents);
    }

    payload.integer<u64>(checkpoint.threads.size());
    for (const auto& thread : checkpoint.threads) {
        payload.integer<u32>(thread.guest_thread_id);
        payload.integer<u32>(thread.cpu_state_abi_size);
        payload.blob(thread.cpu_state);
    }

    payload.integer<u64>(checkpoint.modules.size());
    for (const auto& module : checkpoint.modules) {
        payload.integer<u64>(module.base);
        payload.integer<u64>(module.image_size);
        payload.string(module.guest_path);
        payload.string(module.identity);
    }

    payload.integer<u8>(checkpoint.handles_complete ? 1 : 0);
    payload.integer<u64>(checkpoint.handle_providers.size());
    for (const auto& provider : checkpoint.handle_providers) {
        payload.string(provider.provider);
        payload.integer<u32>(provider.provider_version);
        payload.blob(provider.payload);
    }

    Writer output;
    output.bytes.insert(output.bytes.end(), kMagic.begin(), kMagic.end());
    output.integer<u32>(kFormatVersion);
    output.integer<u32>(kMinimumReaderVersion);
    output.integer<u64>(payload.bytes.size());
    output.integer<u64>(checksum(payload.bytes));
    output.bytes.insert(output.bytes.end(), payload.bytes.begin(), payload.bytes.end());
    bytes = std::move(output.bytes);
    error.clear();
    return true;
}

bool decode(std::span<const u8> bytes, Checkpoint& checkpoint,
            std::string& error) {
    constexpr usize kHeaderSize = 8 + 4 + 4 + 8 + 8;
    if (bytes.size() < kHeaderSize ||
        !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        error = "not a Vitreo checkpoint";
        return false;
    }

    Reader header(bytes.subspan(8, kHeaderSize - 8));
    u32 version = 0;
    u32 minimum_reader = 0;
    u64 payload_size = 0;
    u64 expected_checksum = 0;
    if (!header.integer(version) || !header.integer(minimum_reader) ||
        !header.integer(payload_size) || !header.integer(expected_checksum) ||
        version != kFormatVersion || minimum_reader > kFormatVersion ||
        payload_size != bytes.size() - kHeaderSize) {
        error = "unsupported or malformed checkpoint header";
        return false;
    }

    const auto payload_bytes = bytes.subspan(kHeaderSize);
    if (checksum(payload_bytes) != expected_checksum) {
        error = "checkpoint payload checksum mismatch";
        return false;
    }

    Reader reader(payload_bytes);
    Checkpoint decoded;
    u8 architecture = 0;
    if (!reader.integer(architecture) ||
        (architecture != static_cast<u8>(GuestArchitecture::X86) &&
         architecture != static_cast<u8>(GuestArchitecture::X64)) ||
        !reader.integer(decoded.determinism.virtual_time_ns) ||
        !reader.integer(decoded.determinism.rng_seed) ||
        !reader.integer(decoded.determinism.retired_instructions)) {
        error = "malformed checkpoint metadata";
        return false;
    }
    decoded.architecture = static_cast<GuestArchitecture>(architecture);

    const bool pages_ok = read_records(reader, decoded.pages, [&](MemoryPage& page) {
        u8 protection = 0;
        u8 has_contents = 0;
        if (!reader.integer(page.address) || !reader.integer(protection) ||
            !reader.integer(has_contents) || protection > 7 ||
            has_contents > 1 || !reader.blob(page.contents, kGuestPageSize)) {
            return false;
        }
        page.protection = static_cast<MemoryProtection>(protection);
        page.has_contents = has_contents != 0;
        return (page.address % kGuestPageSize) == 0 &&
               ((page.has_contents && page.contents.size() == kGuestPageSize) ||
                (!page.has_contents && page.contents.empty()));
    });
    const bool threads_ok = pages_ok &&
        read_records(reader, decoded.threads, [&](ThreadState& thread) {
            return reader.integer(thread.guest_thread_id) &&
                   reader.integer(thread.cpu_state_abi_size) &&
                   reader.blob(thread.cpu_state);
        });
    const bool modules_ok = threads_ok &&
        read_records(reader, decoded.modules, [&](ModuleMetadata& module) {
            return reader.integer(module.base) &&
                   reader.integer(module.image_size) &&
                   reader.string(module.guest_path) &&
                   reader.string(module.identity);
        });

    u8 handles_complete = 0;
    const bool handles_ok = modules_ok && reader.integer(handles_complete) &&
        handles_complete <= 1 &&
        read_records(reader, decoded.handle_providers,
                     [&](HandleProviderState& provider) {
            return reader.string(provider.provider) &&
                   reader.integer(provider.provider_version) &&
                   reader.blob(provider.payload);
        });
    if (!handles_ok || reader.remaining() != 0) {
        error = "malformed checkpoint payload";
        return false;
    }
    decoded.handles_complete = handles_complete != 0;
    decoded.format_version = version;
    checkpoint = std::move(decoded);
    error.clear();
    return true;
}

bool save_file(const std::string& path, const Checkpoint& checkpoint,
               std::string& error) {
    std::vector<u8> bytes;
    if (!encode(checkpoint, bytes, error)) return false;
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream ||
        !stream.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()))) {
        error = "failed to write checkpoint: " + path;
        return false;
    }
    return true;
}

bool load_file(const std::string& path, Checkpoint& checkpoint,
               std::string& error) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        error = "failed to open checkpoint: " + path;
        return false;
    }
    const auto end = stream.tellg();
    if (end < 0 ||
        static_cast<u64>(end) > std::numeric_limits<usize>::max()) {
        error = "checkpoint is too large";
        return false;
    }
    std::vector<u8> bytes(static_cast<usize>(end));
    stream.seekg(0);
    if (!bytes.empty() &&
        !stream.read(reinterpret_cast<char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()))) {
        error = "failed to read checkpoint: " + path;
        return false;
    }
    return decode(bytes, checkpoint, error);
}

} // namespace vitreo::checkpoint
