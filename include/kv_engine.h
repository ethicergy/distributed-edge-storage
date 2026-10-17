#pragma once

#include <cstddef>
#include <cstdint>

#include "node_id.h"
#include "placement.h"
#include "protocol.h"
#include "storage.h"

namespace kv {

enum class Result : std::uint8_t {
    OK = 0x00,
    NOT_FOUND,
    INVALID_REQUEST,
    KEY_TOO_LARGE,
    VALUE_TOO_LARGE,
    STORAGE_ERROR,
    NOT_RESPONSIBLE,
    STALE_VERSION,
    NOT_READY,
    REPLICATION_FAILED
};

const char* to_string(Result result);

struct OperationResponse {
    Result result = Result::INVALID_REQUEST;

    std::uint8_t key[storage::KEY_MAX]{};
    std::uint8_t key_length = 0;

    std::uint8_t value[storage::VALUE_MAX]{};
    std::uint16_t value_length = 0;

    // Owners of the key in the view used to execute the request.
    placement::Assignment assignment{node::INVALID, node::INVALID};
};

/*
 * Applies SET/GET/DELETE and versioned replication data to the local
 * Storage Manager. Knows placement (for the primary check) but not
 * the radio, timing or request reliability.
 *
 * Version rule (docs/DESIGN.md, "Versions"): the PRIMARY assigns the
 * version of a client mutation from its own HLC after merging the
 * request's HLC, with version_origin = primary node id. A single
 * clock never repeats a value, so versions are unique even when two
 * nodes briefly both act as primary for a key.
 */
class Engine {
public:
    Engine(
        node::Id local_node_id,
        storage::StorageManager& storage,
        const placement::Resolver& placement
    );

    bool begin();

    bool ready() const;

    /*
     * SET_REQ / GET_REQ / DEL_REQ on this node.
     * NOT_RESPONSIBLE unless this node is the primary in `alive`.
     * mutation_version is used for SET and DEL only.
     */
    Result execute(
        const protocol::DecodedMessage& message,
        placement::AliveMask alive,
        storage::Version mutation_version,
        OperationResponse& response
    ) const;

    /*
     * REPL_REQ (must carry FLAG_REPLICATED) or RESYNC_DATA:
     * version = (header.hlc, header.origin_node_id), tombstone when
     * FLAG_TOMBSTONE is set. Applied with last-writer-wins and no
     * ownership check: a newer version is always safe to keep, and
     * rejecting data during a membership change only loses it.
     */
    Result apply_versioned(
        const protocol::DecodedMessage& message,
        OperationResponse& response
    ) const;

    node::Id local_node_id() const;

private:
    Result execute_set(
        const protocol::DecodedMessage& message,
        storage::Version version,
        OperationResponse& response
    ) const;

    Result execute_get(
        const protocol::DecodedMessage& message,
        OperationResponse& response
    ) const;

    Result execute_delete(
        const protocol::DecodedMessage& message,
        storage::Version version,
        OperationResponse& response
    ) const;

    static Result map_storage_result(
        storage::Result result
    );

    static void copy_key(
        protocol::KeyView key,
        OperationResponse& response
    );

    node::Id local_node_id_;
    storage::StorageManager& storage_;
    const placement::Resolver& placement_;

    bool initialized_ = false;
};

} // namespace kv
