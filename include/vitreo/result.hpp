#pragma once

/// @file result.hpp
/// @brief Result<T, E> error handling type for exception-free error propagation
///
/// Vitreo uses -fno-exceptions, so all error handling is done via Result types.
/// This is similar to Rust's Result<T, E> or C++23's std::expected<T, E>.

#include <new>
#include <type_traits>
#include <utility>

namespace vitreo {

// =============================================================================
// Error Enumeration
// =============================================================================

/// Common error codes used throughout Vitreo
enum class Error {
    None = 0,
    
    // General errors
    InvalidArgument,
    OutOfMemory,
    NotFound,
    AlreadyExists,
    AccessDenied,
    NotSupported,
    
    // I/O errors
    IoError,
    ReadError,
    WriteError,
    SeekError,
    
    // Parse/decode errors
    ParseError,
    InvalidFormat,
    InvalidInstruction,
    InvalidOpcode,
    
    // Memory errors
    InvalidAddress,
    PageFault,
    ProtectionFault,
    
    // Execution errors
    DivideByZero,
    Overflow,
    StackOverflow,
    InvalidHandle,
    
    // System errors
    NotImplemented,
    InternalError,
};

/// Convert error to string for debugging
constexpr const char* error_to_string(Error e) noexcept {
    switch (e) {
        case Error::None:               return "None";
        case Error::InvalidArgument:    return "InvalidArgument";
        case Error::OutOfMemory:        return "OutOfMemory";
        case Error::NotFound:           return "NotFound";
        case Error::AlreadyExists:      return "AlreadyExists";
        case Error::AccessDenied:       return "AccessDenied";
        case Error::NotSupported:       return "NotSupported";
        case Error::IoError:            return "IoError";
        case Error::ReadError:          return "ReadError";
        case Error::WriteError:         return "WriteError";
        case Error::SeekError:          return "SeekError";
        case Error::ParseError:         return "ParseError";
        case Error::InvalidFormat:      return "InvalidFormat";
        case Error::InvalidInstruction: return "InvalidInstruction";
        case Error::InvalidOpcode:      return "InvalidOpcode";
        case Error::InvalidAddress:     return "InvalidAddress";
        case Error::PageFault:          return "PageFault";
        case Error::ProtectionFault:    return "ProtectionFault";
        case Error::DivideByZero:       return "DivideByZero";
        case Error::Overflow:           return "Overflow";
        case Error::StackOverflow:      return "StackOverflow";
        case Error::InvalidHandle:      return "InvalidHandle";
        case Error::NotImplemented:     return "NotImplemented";
        case Error::InternalError:      return "InternalError";
    }
    return "Unknown";
}

// =============================================================================
// Result<T, E> Implementation
// =============================================================================

/// Tag type for constructing Result with a value
struct OkTag {};

/// Tag type for constructing Result with an error
struct ErrTag {};

/// Inline constexpr tag instances
inline constexpr OkTag Ok{};
inline constexpr ErrTag Err{};

/// Result type for error handling without exceptions
///
/// @tparam T The success value type
/// @tparam E The error type (defaults to Error enum)
///
/// Example usage:
/// @code
///     Result<int, Error> divide(int a, int b) {
///         if (b == 0) {
///             return Result<int, Error>::err(Error::DivideByZero);
///         }
///         return Result<int, Error>::ok(a / b);
///     }
///
///     auto result = divide(10, 2);
///     if (result.is_ok()) {
///         int value = result.value();
///     }
/// @endcode
template<typename T, typename E = Error>
class Result {
public:
    // -------------------------------------------------------------------------
    // Static Factory Methods
    // -------------------------------------------------------------------------
    
    /// Create a successful result with a value
    [[nodiscard]] static constexpr Result ok(T value) noexcept(std::is_nothrow_move_constructible_v<T>) {
        return Result(Ok, std::move(value));
    }
    
    /// Create a failed result with an error
    [[nodiscard]] static constexpr Result err(E error) noexcept(std::is_nothrow_move_constructible_v<E>) {
        return Result(Err, std::move(error));
    }
    
    // -------------------------------------------------------------------------
    // Constructors
    // -------------------------------------------------------------------------
    
    /// Construct with value (success)
    constexpr Result(OkTag, T value) noexcept(std::is_nothrow_move_constructible_v<T>)
        : is_ok_(true) {
        new (&storage_.value) T(std::move(value));
    }
    
    /// Construct with error (failure)
    constexpr Result(ErrTag, E error) noexcept(std::is_nothrow_move_constructible_v<E>)
        : is_ok_(false) {
        new (&storage_.error) E(std::move(error));
    }
    
    /// Copy constructor
    constexpr Result(const Result& other) noexcept(
        std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_copy_constructible_v<E>)
        : is_ok_(other.is_ok_) {
        if (is_ok_) {
            new (&storage_.value) T(other.storage_.value);
        } else {
            new (&storage_.error) E(other.storage_.error);
        }
    }
    
    /// Move constructor
    constexpr Result(Result&& other) noexcept(
        std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_constructible_v<E>)
        : is_ok_(other.is_ok_) {
        if (is_ok_) {
            new (&storage_.value) T(std::move(other.storage_.value));
        } else {
            new (&storage_.error) E(std::move(other.storage_.error));
        }
    }
    
    /// Destructor
    ~Result() {
        destroy();
    }
    
    // -------------------------------------------------------------------------
    // Assignment
    // -------------------------------------------------------------------------
    
    /// Copy assignment
    constexpr Result& operator=(const Result& other) noexcept(
        std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_copy_constructible_v<E>) {
        if (this != &other) {
            destroy();
            is_ok_ = other.is_ok_;
            if (is_ok_) {
                new (&storage_.value) T(other.storage_.value);
            } else {
                new (&storage_.error) E(other.storage_.error);
            }
        }
        return *this;
    }
    
    /// Move assignment
    constexpr Result& operator=(Result&& other) noexcept(
        std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_constructible_v<E>) {
        if (this != &other) {
            destroy();
            is_ok_ = other.is_ok_;
            if (is_ok_) {
                new (&storage_.value) T(std::move(other.storage_.value));
            } else {
                new (&storage_.error) E(std::move(other.storage_.error));
            }
        }
        return *this;
    }
    
    // -------------------------------------------------------------------------
    // State Queries
    // -------------------------------------------------------------------------
    
    /// Check if result contains a value (success)
    [[nodiscard]] constexpr bool is_ok() const noexcept { return is_ok_; }
    
    /// Check if result contains an error (failure)
    [[nodiscard]] constexpr bool is_err() const noexcept { return !is_ok_; }
    
    /// Boolean conversion (true if ok)
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return is_ok_; }
    
    // -------------------------------------------------------------------------
    // Value Accessors
    // -------------------------------------------------------------------------
    
    /// Get reference to value (undefined behavior if is_err())
    [[nodiscard]] constexpr T& value() & noexcept {
        return storage_.value;
    }
    
    /// Get const reference to value
    [[nodiscard]] constexpr const T& value() const& noexcept {
        return storage_.value;
    }
    
    /// Get rvalue reference to value
    [[nodiscard]] constexpr T&& value() && noexcept {
        return std::move(storage_.value);
    }
    
    /// Get value or return default if error
    [[nodiscard]] constexpr T value_or(T default_value) const& noexcept(std::is_nothrow_copy_constructible_v<T>) {
        return is_ok_ ? storage_.value : std::move(default_value);
    }
    
    /// Get value or return default if error (move version)
    [[nodiscard]] constexpr T value_or(T default_value) && noexcept(std::is_nothrow_move_constructible_v<T>) {
        return is_ok_ ? std::move(storage_.value) : std::move(default_value);
    }
    
    // -------------------------------------------------------------------------
    // Error Accessors
    // -------------------------------------------------------------------------
    
    /// Get reference to error (undefined behavior if is_ok())
    [[nodiscard]] constexpr E& error() & noexcept {
        return storage_.error;
    }
    
    /// Get const reference to error
    [[nodiscard]] constexpr const E& error() const& noexcept {
        return storage_.error;
    }
    
    /// Get rvalue reference to error
    [[nodiscard]] constexpr E&& error() && noexcept {
        return std::move(storage_.error);
    }
    
    // -------------------------------------------------------------------------
    // Monadic Operations
    // -------------------------------------------------------------------------
    
    /// Transform the value if ok, propagate error if err
    template<typename F>
    [[nodiscard]] constexpr auto map(F&& f) const& -> Result<decltype(f(std::declval<const T&>())), E> {
        using U = decltype(f(std::declval<const T&>()));
        if (is_ok_) {
            return Result<U, E>::ok(f(storage_.value));
        }
        return Result<U, E>::err(storage_.error);
    }
    
    /// Transform the value if ok (move version)
    template<typename F>
    [[nodiscard]] constexpr auto map(F&& f) && -> Result<decltype(f(std::declval<T&&>())), E> {
        using U = decltype(f(std::declval<T&&>()));
        if (is_ok_) {
            return Result<U, E>::ok(f(std::move(storage_.value)));
        }
        return Result<U, E>::err(std::move(storage_.error));
    }
    
    /// Chain operations that return Result
    template<typename F>
    [[nodiscard]] constexpr auto and_then(F&& f) const& -> decltype(f(std::declval<const T&>())) {
        using ResultType = decltype(f(std::declval<const T&>()));
        if (is_ok_) {
            return f(storage_.value);
        }
        return ResultType::err(storage_.error);
    }
    
    /// Chain operations that return Result (move version)
    template<typename F>
    [[nodiscard]] constexpr auto and_then(F&& f) && -> decltype(f(std::declval<T&&>())) {
        using ResultType = decltype(f(std::declval<T&&>()));
        if (is_ok_) {
            return f(std::move(storage_.value));
        }
        return ResultType::err(std::move(storage_.error));
    }
    
    /// Transform the error if err, propagate value if ok
    template<typename F>
    [[nodiscard]] constexpr auto map_err(F&& f) const& -> Result<T, decltype(f(std::declval<const E&>()))> {
        using U = decltype(f(std::declval<const E&>()));
        if (is_ok_) {
            return Result<T, U>::ok(storage_.value);
        }
        return Result<T, U>::err(f(storage_.error));
    }
    
private:
    /// Destroy contained value or error
    constexpr void destroy() noexcept {
        if (is_ok_) {
            storage_.value.~T();
        } else {
            storage_.error.~E();
        }
    }
    
    /// Storage union for value or error
    union Storage {
        T value;
        E error;
        
        // Default constructor does nothing (will be placement new'd)
        constexpr Storage() noexcept {}
        
        // Destructor does nothing (handled by Result)
        ~Storage() {}
    };
    
    Storage storage_;
    bool is_ok_;
};

// =============================================================================
// Result<void, E> Specialization
// =============================================================================

/// Specialization for Result with no value (just success/failure)
template<typename E>
class Result<void, E> {
public:
    /// Create a successful result
    [[nodiscard]] static constexpr Result ok() noexcept {
        return Result(true);
    }
    
    /// Create a failed result with an error
    [[nodiscard]] static constexpr Result err(E error) noexcept(std::is_nothrow_move_constructible_v<E>) {
        return Result(std::move(error));
    }
    
    /// Check if result is success
    [[nodiscard]] constexpr bool is_ok() const noexcept { return is_ok_; }
    
    /// Check if result is error
    [[nodiscard]] constexpr bool is_err() const noexcept { return !is_ok_; }
    
    /// Boolean conversion
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return is_ok_; }
    
    /// Get const reference to error
    [[nodiscard]] constexpr const E& error() const& noexcept { return error_; }
    
    /// Get reference to error
    [[nodiscard]] constexpr E& error() & noexcept { return error_; }
    
    /// Get rvalue reference to error
    [[nodiscard]] constexpr E&& error() && noexcept { return std::move(error_); }
    
private:
    explicit constexpr Result(bool ok) noexcept : is_ok_(ok), error_{} {}
    
    explicit constexpr Result(E error) noexcept(std::is_nothrow_move_constructible_v<E>)
        : is_ok_(false), error_(std::move(error)) {}
    
    bool is_ok_;
    E error_;
};

// =============================================================================
// Helper Type Aliases
// =============================================================================

/// Result with default Error type
template<typename T>
using VResult = Result<T, Error>;

/// Void result with default Error type
using VoidResult = Result<void, Error>;

// =============================================================================
// Macros for Error Propagation
// =============================================================================

/// Try a Result expression, return early if error
/// Usage: VITREO_TRY(result_expr)
#define VITREO_TRY(expr)                    \
    do {                                     \
        auto&& _result = (expr);             \
        if (_result.is_err()) {              \
            return decltype(_result)::err(   \
                std::move(_result).error()); \
        }                                    \
    } while (0)

/// Try a Result expression and assign value if ok, return early if error
/// Usage: auto value = VITREO_TRY_UNWRAP(result_expr)
#define VITREO_TRY_UNWRAP(expr)             \
    ({                                       \
        auto&& _result = (expr);             \
        if (_result.is_err()) {              \
            return decltype(_result)::err(   \
                std::move(_result).error()); \
        }                                    \
        std::move(_result).value();          \
    })

} // namespace vitreo
