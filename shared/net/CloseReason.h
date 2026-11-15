// Why a Connection was closed (reported once through its closed callback).
#pragma once

#include <cstdint>
#include <string_view>

namespace vo::net {

enum class CloseReason : std::uint8_t
{
    LocalClose,       // close()/abort() called by the owner
    PeerClosed,       // orderly shutdown by the remote side (EOF)
    NetworkError,     // socket error (reset, unreachable, ...)
    BadFrame,         // frame length out of range or frame too short
    DecryptFailed,    // AEAD authentication failure (tampered, replayed or wrong key)
    OutboundOverflow, // more than kMaxOutboundQueueBytes queued for sending
};

[[nodiscard]] constexpr std::string_view closeReasonName(CloseReason r) noexcept
{
    switch (r)
    {
    case CloseReason::LocalClose: return "local close";
    case CloseReason::PeerClosed: return "peer closed";
    case CloseReason::NetworkError: return "network error";
    case CloseReason::BadFrame: return "bad frame";
    case CloseReason::DecryptFailed: return "decrypt failed";
    case CloseReason::OutboundOverflow: return "outbound queue overflow";
    }
    return "unknown";
}

} // namespace vo::net
