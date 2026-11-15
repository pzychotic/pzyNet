#pragma once

#include <string>
#include <system_error>

namespace pzynet
{

/// Library-specific error conditions. Operating-system failures are reported with their native code in
/// std::system_category(); "would block" and "in progress" are normalised to std::errc so that
/// `ec == std::errc::operation_would_block` works identically on Windows and Linux.
enum class Errc
{
    invalid_argument = 1,
    not_open,
    already_open,
    unknown_connection,
    not_connected,
    message_too_large,
    connection_limit_reached,
    reserved_message_id,
    address_family_mismatch,
    host_not_found,
    running,
    already_connected,
};

namespace detail
{
class ErrorCategory final : public std::error_category
{
public:
    const char* name() const noexcept override { return "pzynet"; }
    std::string message(int value) const override
    {
        switch (static_cast<Errc>(value))
        {
        case Errc::invalid_argument: return "invalid argument";
        case Errc::not_open: return "socket is not open";
        case Errc::already_open: return "socket is already open";
        case Errc::unknown_connection: return "unknown connection id";
        case Errc::not_connected: return "connection is not established";
        case Errc::message_too_large: return "message too large";
        case Errc::connection_limit_reached: return "connection limit reached";
        case Errc::reserved_message_id: return "payload starts with a message id reserved by the protocol";
        case Errc::address_family_mismatch: return "address family not supported by this socket or protocol";
        case Errc::host_not_found: return "host not found";
        case Errc::running: return "operation not permitted while the background thread is running";
        case Errc::already_connected: return "a connection to this endpoint already exists";
        }
        return "unknown pzynet error";
    }
};
} // namespace detail

inline const std::error_category& error_category() noexcept
{
    static const detail::ErrorCategory instance;
    return instance;
}

inline std::error_code make_error_code(Errc e) noexcept
{
    return std::error_code(static_cast<int>(e), error_category());
}

namespace detail
{
/// Backs the throwing overloads: every fallible call exists as `f(args)` and `f(args, std::error_code&)`.
inline void throw_if(const std::error_code& ec, const char* what)
{
    if (ec)
        throw std::system_error(ec, what);
}
} // namespace detail

} // namespace pzynet

template <>
struct std::is_error_code_enum<pzynet::Errc> : std::true_type
{
};
