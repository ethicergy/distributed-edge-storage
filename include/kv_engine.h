#pragma once

#include <cstddef>
#include <cstdint>

#include "membership.h"
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
    NOT_READY
};

struct OperationResponse {
    Result result = Result::INVALID_REQUEST;

    std::uint8_t key[storage::KEY_MAX]{};
    std::uint8_t key_length = 0;

    std::uint8_t value[storage::VALUE_MAX]{};
    std::uint16_t value_length = 0;
};

class Engine {
public:
    Engine(
        node::Id local_node_id,
        storage::StorageManager& storage,
        const placement::Resolver& placement
    );

    bool begin();

    bool ready() const;

    Result execute(
        const protocol::DecodedMessage& message,
        const membership::Manager& membership,
        std::uint64_t now_ms,
        OperationResponse& response
    ) const;

    node::Id local_node_id() const;

private:
    Result execute_set(
        const protocol::DecodedMessage& message,
        OperationResponse& response
    ) const;

    Result execute_get(
        const protocol::DecodedMessage& message,
        OperationResponse& response
    ) const;

    Result execute_delete(
        const protocol::DecodedMessage& message,
        OperationResponse& response
    ) const;

    Result map_storage_result(
        storage::Result result
    ) const;

    void copy_key(
        protocol::KeyView key,
        OperationResponse& response
    ) const;

    node::Id local_node_id_;
    storage::StorageManager& storage_;
    const placement::Resolver& placement_;

    bool initialized_ = false;
};

} // namespace kv