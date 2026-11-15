// Player-facing Turkish texts for every network outcome the client can run into: handshake/connect
// results, login results, character create/delete/enter-world results and the reasons a live
// connection ended. Pure functions (vo_client_core), unit-tested to cover every enum value.
#pragma once

#include "shared/net/CloseReason.h"
#include "shared/net/ClientSession.h"
#include "shared/net/Opcodes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace vo::client::net {

// Title + body for a message box.
struct UserMessage
{
    std::string title;
    std::string text;
};

// Why GameClient could not establish a session.
enum class ConnectFailure : std::uint8_t
{
    Session,          // see ConnectResult
    MissingServerKey, // config has no (valid) server_public_key
};

[[nodiscard]] UserMessage connectFailureMessage(vo::net::ConnectResult result);
[[nodiscard]] UserMessage missingServerKeyMessage();

// LOGIN_RES failures (LoginResult::Ok yields an empty message).
[[nodiscard]] UserMessage loginFailureMessage(vo::net::LoginResult result);
// LOGIN_RES result byte as received; unknown values get a generic text.
[[nodiscard]] UserMessage loginFailureMessage(std::uint8_t rawResult);

[[nodiscard]] std::string charCreateResultMessage(vo::net::CharCreateResult result);
[[nodiscard]] std::string charDeleteResultMessage(vo::net::CharDeleteResult result);
[[nodiscard]] std::string enterWorldResultMessage(vo::net::EnterWorldResult result);
[[nodiscard]] std::string leaveWorldResultMessage(std::uint8_t result);

// Why an established connection ended (not reported for a local disconnect()).
enum class DisconnectCause : std::uint8_t
{
    Closed,         // the connection closed; see closeReason and serverError
    RequestTimeout, // a request got no answer in time
    PingTimeout,    // no PONG for too long
};

struct DisconnectInfo
{
    DisconnectCause cause = DisconnectCause::Closed;
    vo::net::CloseReason closeReason = vo::net::CloseReason::PeerClosed;
    // ERROR packet received right before the close, if any (raw u16: unknown codes are possible).
    std::optional<std::uint16_t> serverError;
};

[[nodiscard]] UserMessage disconnectMessage(const DisconnectInfo& info);

} // namespace vo::client::net
