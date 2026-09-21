#pragma once

#include <cstddef>
#include <cstdint>

namespace protocol {

	// Protocol wide constants
	constexpr std::uint8_t PROTOCOL_VERSION = 0x01;

constexpr std::size_t HEADER_SIZE = 22;
constexpr std::size_t MAX_PAYLOAD_SIZE = 228;
constexpr std::size_t MAX_PACKET_SIZE = 250;

constexpr std::size_t KEY_MAX = 32;
constexpr std::size_t VALUE_MAX = 192;



	// node ids

using NodeId = std::uint8_t;

constexpr NodeId NODE_ID_INVALID = 0x00;

constexpr NodeId NODE_A = 0x01;
constexpr NodeId NODE_B = 0x02;
constexpr NodeId NODE_C = 0x03;
constexpr NodeId NODE_D = 0x04;

constexpr NodeId NODE_ID_MIN = NODE_A;
constexpr NodeId NODE_ID_MAX = NODE_D;

constexpr NodeId NODE_ID_BROADCAST = 0xFF;

//request identity

using RequestId = std::uint32_t;

// logical identity of a request: (origin_node_id, request_id)
//sender is not part of request identity cuz it may be relayed or replicated by another node

// HLC: 64 bit wire field: upper 48 bits = physical milliseconds, lower 16 bits = logical counter

using Hlc = std::uint64_t;

//message types:

enum class MessageType : std::uint8_t {
    SET_REQ       = 0x01,
    SET_ACK       = 0x02,
    GET_REQ       = 0x03,
    GET_RESP      = 0x04,
    DEL_REQ       = 0x05,
    DEL_ACK       = 0x06,
    REPL_REQ      = 0x07,
    REPL_ACK      = 0x08,
    HEARTBEAT     = 0x09,
    VERSION_TABLE = 0x0A,
    RESYNC_DATA   = 0x0B
};

//flags:

constexpr std::uint8_t FLAG_REPLICATED = 1u << 0;
constexpr std::uint8_t FLAG_RETRY      = 1u << 1;
constexpr std::uint8_t FLAG_TOMBSTONE  = 1u << 2;

constexpr std::uint8_t FLAG_RESERVED_MASK = 0xF8;

constexpr std::uint8_t VERSION_ENTRY_FLAG_TOMBSTONE = 1u << 0;
//status codes:

enum class StatusCode : std::uint8_t {
    OK                  = 0x00,
    NOT_FOUND           = 0x01,
    INVALID_REQUEST     = 0x02,
    KEY_TOO_LARGE       = 0x03,
    VALUE_TOO_LARGE     = 0x04,
    STORAGE_ERROR       = 0x05,
    NOT_RESPONSIBLE     = 0x06,
    REPLICATION_FAILED  = 0x07,
    STALE_VERSION       = 0x08,
    UNSUPPORTED          = 0x09
};




struct ByteView {
    const std::uint8_t* data;
    std::uint8_t len;
};

using KeyView = ByteView;
using ValueView = ByteView;


enum class Result : std::uint8_t {
    OK = 0x00,
    BUFFER_TOO_SMALL,
    INVALID_ARGUMENT,
    INVALID_PACKET,
    INVALID_MESSAGE,
    INVALID_PAYLOAD
};

//Logical packet header

//This struct is not a direct copy to send to esp-now frame, we will encode each field (in big endian order)
//Exact frame with HEADER_SIZE bytes properly done later

struct PacketHeader {
    std::uint8_t protocol_version;
    MessageType msg_type;
    std::uint8_t flags;
    std::uint8_t header_len;

    RequestId request_id;

    NodeId origin_node_id;
    NodeId sender_node_id;
    NodeId destination_node_id;

    std::uint8_t ttl;

    Hlc hlc;

    std::uint16_t payload_len;
};

struct PacketView {
    PacketHeader header;
    ByteView payload;
};
Result validate_message_header(
    const PacketHeader& header
);

Result serialize_packet(
    const PacketHeader& header,
    ByteView payload,
    std::uint8_t* out,
    std::size_t out_capacity,
    std::size_t& bytes_written
);

Result deserialize_packet(
    const std::uint8_t* data,
    std::size_t packet_len,
    PacketView& packet
);
Result serialize_header(
    const PacketHeader& header,
    std::uint8_t* out,
    std::size_t out_capacity
);

Result deserialize_header(
    const std::uint8_t* data,
    std::size_t packet_len,
    PacketHeader& header
);

struct SetRequestPayload {
    KeyView key;
    ValueView value;
};

struct ReplicationRequestPayload {
    KeyView key;
    ValueView value;
};

Result serialize_set_request(
    const SetRequestPayload& payload,
    std::uint8_t* out,
    std::size_t out_capacity,
    std::size_t& bytes_written
);

Result deserialize_set_request(
    const std::uint8_t* data,
    std::size_t payload_len,
    SetRequestPayload& payload
);

Result serialize_replication_request(
    const ReplicationRequestPayload& payload,
    std::uint8_t* out,
    std::size_t out_capacity,
    std::size_t& bytes_written
);

Result deserialize_replication_request(
    const std::uint8_t* data,
    std::size_t payload_len,
    ReplicationRequestPayload& payload
);

struct SetAckPayload {
    StatusCode status;
    KeyView key;
};

struct GetRequestPayload {
    KeyView key;
};

struct GetResponsePayload {
    StatusCode status;
    KeyView key;
    ValueView value;
};

struct DeleteRequestPayload {
    KeyView key;
};

struct DeleteAckPayload {
    StatusCode status;
    KeyView key;
};





struct ReplicationAckPayload {
    StatusCode status;
    KeyView key;
};


struct HeartbeatPayload {
    // Empty by specification.
};



struct VersionTableEntry {
    std::uint8_t entry_flags;
    KeyView key;
    Hlc version_hlc;
    NodeId version_origin_id;
};

struct VersionTablePayload {
    std::uint32_t sync_id;
    std::uint16_t chunk_index;
    std::uint16_t chunk_count;
    std::uint8_t entry_count;

    const VersionTableEntry* entries;
};


constexpr std::size_t VERSION_TABLE_MAX_ENTRIES = 5;

struct ResyncDataPayload {
    KeyView key;
    ValueView value;
};

struct DecodedMessage {
    PacketHeader header;

    union Payload {
        SetRequestPayload set_request;
        SetAckPayload set_ack;
        GetRequestPayload get_request;
        GetResponsePayload get_response;
        DeleteRequestPayload delete_request;
        DeleteAckPayload delete_ack;
        ReplicationRequestPayload replication_request;
        ReplicationAckPayload replication_ack;
        HeartbeatPayload heartbeat;
        VersionTablePayload version_table;
        ResyncDataPayload resync_data;
    } payload;

    VersionTableEntry version_table_entries[VERSION_TABLE_MAX_ENTRIES];
};

Result decode_message(
    const PacketView& packet,
    DecodedMessage& message
);

Result serialize_get_request(
    const GetRequestPayload& payload,
    std::uint8_t* out,
    std::size_t out_capacity,
    std::size_t& bytes_written
);

Result deserialize_get_request(
    const std::uint8_t* data,
    std::size_t payload_len,
    GetRequestPayload& payload
);

Result serialize_set_ack(
    const SetAckPayload& payload,
    std::uint8_t* out,
    std::size_t out_capacity,
    std::size_t& bytes_written
);

Result deserialize_set_ack(
    const std::uint8_t* data,
    std::size_t payload_len,
    SetAckPayload& payload
);

Result serialize_get_response(
    const GetResponsePayload& payload,
    std::uint8_t* out,
    std::size_t out_capacity,
    std::size_t& bytes_written
);

Result deserialize_get_response(
    const std::uint8_t* data,
    std::size_t payload_len,
    GetResponsePayload& payload
);

Result serialize_delete_request(
    const DeleteRequestPayload& payload,
    std::uint8_t* out,
    std::size_t out_capacity,
    std::size_t& bytes_written
);

Result deserialize_delete_request(
    const std::uint8_t* data,
    std::size_t payload_len,
    DeleteRequestPayload& payload
);

Result serialize_delete_ack(
    const DeleteAckPayload& payload,
    std::uint8_t* out,
    std::size_t out_capacity,
    std::size_t& bytes_written
);

Result deserialize_delete_ack(
    const std::uint8_t* data,
    std::size_t payload_len,
    DeleteAckPayload& payload
);

Result serialize_replication_ack(
    const ReplicationAckPayload& payload,
    std::uint8_t* out,
    std::size_t out_capacity,
    std::size_t& bytes_written
);

Result deserialize_replication_ack(
    const std::uint8_t* data,
    std::size_t payload_len,
    ReplicationAckPayload& payload
);


Result serialize_version_table(
    const VersionTablePayload& payload,
    std::uint8_t* out,
    std::size_t out_capacity,
    std::size_t& bytes_written
);

Result deserialize_version_table(
    const std::uint8_t* data,
    std::size_t payload_len,
    VersionTablePayload& payload,
    VersionTableEntry* entries_out,
    std::size_t entries_capacity
);

Result serialize_resync_data(
    const ResyncDataPayload& payload,
    std::uint8_t* out,
    std::size_t out_capacity,
    std::size_t& bytes_written
);

Result deserialize_resync_data(
    const std::uint8_t* data,
    std::size_t payload_len,
    ResyncDataPayload& payload
);















bool is_valid_node_id(NodeId node_id);

bool is_valid_destination(NodeId node_id);

bool is_known_message_type(MessageType type);

bool has_valid_reserved_flags(std::uint8_t flags);



static_assert(sizeof(MessageType) == 1,
              "MessageType must occupy exactly 1 byte");

static_assert(sizeof(StatusCode) == 1,
              "StatusCode must occupy exactly 1 byte");

static_assert(HEADER_SIZE == 22,
              "v1 header size must remain 22 bytes");

static_assert(MAX_PACKET_SIZE ==
              HEADER_SIZE + MAX_PAYLOAD_SIZE,
              "Packet size constants are inconsistent");

static_assert(KEY_MAX <= 255,
              "Key length must fit in one byte");

static_assert(VALUE_MAX <= 255,
              "Value length must fit in one byte");

static_assert(
    9 + VERSION_TABLE_MAX_ENTRIES * 43 <= MAX_PAYLOAD_SIZE,
    "VERSION_TABLE entry count exceeds payload capacity"
);

static_assert(
    9 + (VERSION_TABLE_MAX_ENTRIES + 1) * 43 > MAX_PAYLOAD_SIZE,
    "VERSION_TABLE_MAX_ENTRIES is not the maximum possible count"
);


} //namespace protocol


