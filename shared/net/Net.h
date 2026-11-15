// shared/net - wire protocol (docs/PROTOCOL.md). Umbrella header for the asio-free parts:
//   Opcodes.h       opcodes, error codes, result enums, limits
//   PacketWriter.h  / PacketReader.h   little-endian field encoding
//   Messages.h      one struct per packet
//   FrameCodec.h    stream framing
//   Crypto.h        handshake keys, signatures, frame cipher, password hashing
// asio-based: Connection.h (server and client), ClientSession.h (client handshake; asio-free header).
#pragma once

#include "shared/net/Crypto.h"
#include "shared/net/FrameCodec.h"
#include "shared/net/Messages.h"
#include "shared/net/Opcodes.h"
#include "shared/net/PacketReader.h"
#include "shared/net/PacketWriter.h"
