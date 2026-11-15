# VedatOnline Network Protocol — v1

Status: **frozen contract for milestone "İlk Adım"**. Client, server and bot client implement exactly this.
Changes go through the project lead and bump `PROTOCOL_VERSION`.

## 1. Transport and encoding

- TCP, default port **15001**, `TCP_NODELAY` on both sides.
- All integers little-endian. `f32` = IEEE-754 single.
- `str8` = `u8 byte_length` + UTF-8 bytes (no terminator). `str16` = `u16 byte_length` + UTF-8 bytes.
- `bool` = `u8` (0/1; any other value is a protocol error).
- A reader that runs out of bytes, or a packet with trailing bytes left over, is a protocol error → connection closed.

## 2. Framing

### 2.1 Plaintext frames (handshake only)

```
u16 length        // bytes that follow, 2..512
u16 opcode
...payload
```

Only `HELLO` and `HELLO_ACK` are ever sent in plaintext.

### 2.2 Encrypted frames (everything after the handshake)

```
u16 ct_length     // bytes that follow, 18..8208  (plaintext 2..8192 + 16-byte tag)
u8  ciphertext[ct_length]
```

- Cipher: `crypto_aead_xchacha20poly1305_ietf` (libsodium).
- Key: per-direction session key (§3).
- Nonce (24 bytes): 16 zero bytes followed by a `u64` LE frame counter. Each direction has its own counter starting at 0, incremented by one per frame sent. The counter is **not** transmitted; both sides track it. This gives replay/reorder protection for free.
- Additional data: the 2 `ct_length` bytes exactly as on the wire.
- Plaintext: `u16 opcode` + payload.
- Any length out of range or authentication failure → close the connection immediately, no error packet.

## 3. Handshake

Constants: `MAGIC = 0x4C4E4F56` ("VONL" on the wire), `PROTOCOL_VERSION = 1`.

```
C → S  HELLO (0x0001), plaintext
    u32 magic
    u16 protocol_version
    u32 client_build
    u8  client_kx_pk[32]        // ephemeral, crypto_kx_keypair

S → C  HELLO_ACK (0x0002), plaintext
    u8  result                  // 0 = ok, 1 = version mismatch, 2 = server full, 3 = maintenance
    u16 server_protocol_version
    u8  server_kx_pk[32]        // ephemeral, new per connection
    u8  signature[64]           // Ed25519 (crypto_sign_detached) with the server's long-term key
```

- Signed message: ASCII `"VO-HS1"` ‖ `client_kx_pk` ‖ `server_kx_pk` ‖ `u16 protocol_version`.
- If `result != 0` the key and signature fields are zero-filled and the server closes after sending.
- The client verifies the signature against the server public key it ships with (`config/client.toml: server_public_key`, hex). Failure → abort, show "sunucu doğrulanamadı".
- Session keys: server calls `crypto_kx_server_session_keys(rx, tx, server_pk, server_sk, client_pk)`, client calls `crypto_kx_client_session_keys(rx, tx, client_pk, client_sk, server_pk)`. `tx` encrypts what you send, `rx` decrypts what you receive.
- Ephemeral secret keys are wiped (`sodium_memzero`) right after key derivation.
- Server long-term Ed25519 key pair lives in `config/keys/` (git-ignored); `vo_server --gen-keys` creates it and prints the public key hex.
- Timeouts: the server closes a connection that has not completed the handshake within 5 s, or not logged in within 30 s after it.

## 4. Session states

| State | Entered when | Accepted client opcodes |
|---|---|---|
| `Handshake` | TCP accept | `HELLO` |
| `Auth` | handshake ok | `LOGIN_REQ`, `PING` |
| `CharSelect` | login ok / left world | `CHAR_LIST_REQ`, `CHAR_CREATE_REQ`, `CHAR_DELETE_REQ`, `ENTER_WORLD_REQ`, `LOGOUT`, `PING` |
| `InWorld` | enter world ok | `MOVE_REQ`, `LEAVE_WORLD_REQ`, `LOGOUT`, `PING` |

An opcode not valid in the current state → `ERROR(BadState)` and close.

## 5. Opcodes

`C→S` = client to server, `S→C` = server to client.

| Opcode | Name | Dir |
|---|---|---|
| 0x0001 | HELLO | C→S |
| 0x0002 | HELLO_ACK | S→C |
| 0x0010 | PING | C→S |
| 0x0011 | PONG | S→C |
| 0x0012 | ERROR | S→C |
| 0x0013 | LOGOUT | C→S |
| 0x0100 | LOGIN_REQ | C→S |
| 0x0101 | LOGIN_RES | S→C |
| 0x0110 | CHAR_LIST_REQ | C→S |
| 0x0111 | CHAR_LIST_RES | S→C |
| 0x0112 | CHAR_CREATE_REQ | C→S |
| 0x0113 | CHAR_CREATE_RES | S→C |
| 0x0114 | CHAR_DELETE_REQ | C→S |
| 0x0115 | CHAR_DELETE_RES | S→C |
| 0x0120 | ENTER_WORLD_REQ | C→S |
| 0x0121 | ENTER_WORLD_RES | S→C |
| 0x0122 | LEAVE_WORLD_REQ | C→S |
| 0x0123 | LEAVE_WORLD_RES | S→C |
| 0x0200 | MOVE_REQ | C→S |
| 0x0201 | ENTITY_SPAWN | S→C |
| 0x0202 | ENTITY_DESPAWN | S→C |
| 0x0203 | ENTITY_MOVE | S→C |
| 0x0204 | POSITION_CORRECT | S→C |

### 5.1 Common

```
PING            u32 client_time_ms
PONG            u32 client_time_ms (echo), u32 server_time_ms
ERROR           u16 error_code, str8 debug_message     // server usually closes afterwards
LOGOUT          (empty)                                 // server saves state and closes
```

The client sends `PING` every 5 s; the server drops a connection silent for 30 s.

### 5.2 Login

```
LOGIN_REQ       str8 username, str8 password
LOGIN_RES       u8 result, u32 account_id
```

`result`: 0 Ok · 1 InvalidCredentials · 2 Banned · 3 AlreadyLoggedIn (unused: the old session is kicked instead) · 4 TooManyAttempts · 5 ServerError.

- Username: 4–20 chars, `[a-zA-Z0-9_]`, case-insensitive. Password: 6–64 bytes.
- Unknown user and wrong password both return `InvalidCredentials`.
- Rate limit: 5 failed attempts per IP per minute → `TooManyAttempts`.
- If `auth.dev_auto_register = true` in the server config, an unknown username with a valid-format password creates the account and logs in.
- Logging in to an account that is already online kicks the old session (`ERROR(KickedDuplicateLogin)` to it).

### 5.3 Characters

```
CharSummary:
    u32  char_id
    str8 name
    u8   race
    u8   class
    u8   face
    u8   hair
    u16  level
    u16  zone_id

CHAR_LIST_REQ    (empty)
CHAR_LIST_RES    u8 count (0..3), CharSummary[count]

CHAR_CREATE_REQ  str8 name, u8 race, u8 class, u8 face, u8 hair
CHAR_CREATE_RES  u8 result, CharSummary character      // character present only if result == 0

CHAR_DELETE_REQ  u32 char_id
CHAR_DELETE_RES  u8 result, u32 char_id

ENTER_WORLD_REQ  u32 char_id
ENTER_WORLD_RES  u8 result
                 // only if result == 0:
                 u32 entity_id
                 u16 zone_id
                 f32 x, f32 y, f32 z, f32 yaw
                 f32 walk_speed, f32 run_speed       // m/s
                 u32 server_time_ms

LEAVE_WORLD_REQ  (empty)
LEAVE_WORLD_RES  u8 result                           // back to CharSelect
```

Create `result`: 0 Ok · 1 NameInvalid · 2 NameTaken · 3 NoFreeSlot · 4 InvalidRaceOrClass · 5 InvalidAppearance · 6 ServerError.
Delete `result`: 0 Ok · 1 NotFound · 2 ServerError. Deletion is soft (`deleted_at`), the name stays reserved.
Enter `result`: 0 Ok · 1 NotFound · 2 ServerError.

Character name: 3–16 characters; letters (ASCII plus `çğıöşüÇĞİÖŞÜ`) and digits; must start with a letter; unique case-insensitively; for uniqueness all four i-like letters (`I`, `ı`, `İ`, `i`) fold to the same key, so `Irmak`, `ırmak`, `İrmak` and `irmak` collide. Max 3 characters per account.

Enums (shared header `shared/game`):

| race | value | allowed classes |
|---|---|---|
| Aydor | 1 | Warrior, Rogue, Mage, Priest |
| Kargan | 2 | Warrior, Rogue, Mage, Priest |

| class | value |
|---|---|
| Warrior (Savaşçı) | 1 |
| Rogue (Haydut) | 2 |
| Mage (Büyücü) | 3 |
| Priest (Şifacı) | 4 |

`face` 0–3, `hair` 0–3. New characters spawn at the zone's race spawn point (zone 1), level 1.

### 5.4 World

Coordinate system: left-handed, **+X right, +Y up, +Z forward**, units in metres. `yaw` in radians, 0 = facing +Z, increasing clockwise seen from above. Zone 1 spans `0..512` on X and Z.

```
MOVE_REQ          u32 seq                 // client-incremented
                  u8  flags               // bit0 moving, bit1 running
                  f32 x, f32 y, f32 z
                  f32 yaw

ENTITY_SPAWN      u32 entity_id
                  u8  entity_type         // 1 = player
                  str8 name
                  u8  race, u8 class, u8 face, u8 hair
                  u16 level
                  f32 x, f32 y, f32 z, f32 yaw
                  u8  flags               // as MOVE_REQ

ENTITY_DESPAWN    u32 entity_id

ENTITY_MOVE       u32 entity_id
                  u8  flags
                  f32 x, f32 y, f32 z
                  f32 yaw
                  u32 server_time_ms

POSITION_CORRECT  u32 seq                 // last MOVE_REQ seq the server processed
                  f32 x, f32 y, f32 z, f32 yaw
```

Movement model (client-predicted, server-validated):

- The client moves its own character immediately and sends `MOVE_REQ` at **10 Hz while moving**, plus one immediately on start, stop and run/walk toggle (the stop packet has `moving = 0`).
- The server keeps the last accepted position and its time. For each `MOVE_REQ` it accepts the new position if all hold:
  - horizontal distance from last accepted ≤ `speed × elapsed × 1.25 + 0.5 m` (speed = run or walk per flags),
  - inside zone bounds and on walkable terrain (slope limit, not inside blocked cells),
  - `|y − terrain_height(x, z)| ≤ 1.0 m` (the server then snaps `y` to the terrain).
- On rejection the server keeps the old position and sends `POSITION_CORRECT`; the client snaps to it and drops its pending predictions.
- More than 30 `MOVE_REQ` per second from one client → `ERROR(Flood)` and close.
- The world ticks at 20 Hz. Accepted moves are relayed as `ENTITY_MOVE` to players in the area of interest at most 10 times per second per entity. Remote entities are rendered ~100 ms in the past, interpolating between the last two updates.
- Area of interest: 32 m grid cells; a player sees entities in the 5×5 block of cells centred on its own cell (view radius of at least 64 m). Entering range → `ENTITY_SPAWN`, leaving → `ENTITY_DESPAWN`. On `ENTER_WORLD_RES` the server follows up with `ENTITY_SPAWN` for everyone already in range.
- Position is persisted on leave/logout/disconnect and every 60 s.

## 6. Error codes (`ERROR.error_code`)

| Code | Name |
|---|---|
| 1 | ProtocolViolation |
| 2 | BadState |
| 3 | Flood |
| 4 | Timeout |
| 5 | KickedDuplicateLogin |
| 6 | ServerShutdown |
| 7 | InternalError |

## 7. Limits summary

| Limit | Value |
|---|---|
| Max plaintext per encrypted frame | 8192 bytes |
| Handshake timeout | 5 s |
| Login timeout after handshake | 30 s |
| Idle timeout | 30 s |
| Ping interval | 5 s |
| Failed logins | 5 / IP / minute |
| MOVE_REQ rate | 30 / s hard cap |
| Characters per account | 3 |
| Per-connection outbound queue | 256 KB, exceeded → close |
