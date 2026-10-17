#include "kv_engine.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace kv {

const char* to_string(Result result)
{
    switch (result) {
        case Result::OK: return "OK";
        case Result::NOT_FOUND: return "NOT_FOUND";
        case Result::INVALID_REQUEST: return "INVALID_REQUEST";
        case Result::KEY_TOO_LARGE: return "KEY_TOO_LARGE";
        case Result::VALUE_TOO_LARGE: return "VALUE_TOO_LARGE";
        case Result::STORAGE_ERROR: return "STORAGE_ERROR";
        case Result::NOT_RESPONSIBLE: return "NOT_RESPONSIBLE";
        case Result::STALE_VERSION: return "STALE_VERSION";
        case Result::NOT_READY: return "NOT_READY";
        case Result::REPLICATION_FAILED: return "REPLICATION_FAILED";
    }

    return "UNKNOWN";
}

Engine::Engine(
    node::Id local_node_id,
    storage::StorageManager& storage,
    const placement::Resolver& placement
)
    : local_node_id_(local_node_id),
      storage_(storage),
      placement_(placement)
{
}

bool Engine::begin()
{
    if (
        local_node_id_ < node::MIN ||
        local_node_id_ > node::MAX
    ) {
        return false;
    }

    if (!storage_.ready()) {
        return false;
    }

    if (!placement_.ready()) {
        return false;
    }

    initialized_ = true;
    return true;
}

bool Engine::ready() const
{
    return initialized_ &&
           storage_.ready() &&
           placement_.ready();
}

node::Id Engine::local_node_id() const
{
    return local_node_id_;
}

Result Engine::execute(
    const protocol::DecodedMessage& message,
    placement::AliveMask alive,
    storage::Version mutation_version,
    OperationResponse& response
) const
{
    response = OperationResponse{};

    if (!ready()) {
        response.result = Result::NOT_READY;
        return response.result;
    }

    protocol::KeyView key{};

    switch (message.header.msg_type) {
        case protocol::MessageType::SET_REQ:
            key = message.payload.set_request.key;
            break;

        case protocol::MessageType::GET_REQ:
            key = message.payload.get_request.key;
            break;

        case protocol::MessageType::DEL_REQ:
            key = message.payload.delete_request.key;
            break;

        default:
            response.result = Result::INVALID_REQUEST;
            return response.result;
    }

    if (key.len == 0 || key.data == nullptr) {
        response.result = Result::INVALID_REQUEST;
        return response.result;
    }

    if (key.len > storage::KEY_MAX) {
        response.result = Result::KEY_TOO_LARGE;
        return response.result;
    }

    copy_key(key, response);

    if (
        !placement_.locate(
            key.data,
            key.len,
            alive,
            response.assignment
        ) ||
        response.assignment.primary != local_node_id_
    ) {
        response.result = Result::NOT_RESPONSIBLE;
        return response.result;
    }

    switch (message.header.msg_type) {
        case protocol::MessageType::SET_REQ:
            return execute_set(message, mutation_version, response);

        case protocol::MessageType::GET_REQ:
            return execute_get(message, response);

        case protocol::MessageType::DEL_REQ:
            return execute_delete(message, mutation_version, response);

        default:
            response.result = Result::INVALID_REQUEST;
            return response.result;
    }
}

Result Engine::apply_versioned(
    const protocol::DecodedMessage& message,
    OperationResponse& response
) const
{
    response = OperationResponse{};

    if (!ready()) {
        response.result = Result::NOT_READY;
        return response.result;
    }

    protocol::KeyView key{};
    protocol::ValueView value{};

    switch (message.header.msg_type) {
        case protocol::MessageType::REPL_REQ:
            if ((message.header.flags & protocol::FLAG_REPLICATED) == 0) {
                response.result = Result::INVALID_REQUEST;
                return response.result;
            }

            key = message.payload.replication_request.key;
            value = message.payload.replication_request.value;
            break;

        case protocol::MessageType::RESYNC_DATA:
            key = message.payload.resync_data.key;
            value = message.payload.resync_data.value;
            break;

        default:
            response.result = Result::INVALID_REQUEST;
            return response.result;
    }

    if (key.len == 0 || key.data == nullptr) {
        response.result = Result::INVALID_REQUEST;
        return response.result;
    }

    if (key.len > storage::KEY_MAX) {
        response.result = Result::KEY_TOO_LARGE;
        return response.result;
    }

    copy_key(key, response);

    if (value.len > storage::VALUE_MAX) {
        response.result = Result::VALUE_TOO_LARGE;
        return response.result;
    }

    if (value.len > 0 && value.data == nullptr) {
        response.result = Result::INVALID_REQUEST;
        return response.result;
    }

    const bool tombstone =
        (message.header.flags & protocol::FLAG_TOMBSTONE) != 0;

    if (tombstone && value.len != 0) {
        response.result = Result::INVALID_REQUEST;
        return response.result;
    }

    const storage::Version version{
        message.header.hlc,
        message.header.origin_node_id
    };

    const storage::Result storage_result =
        storage_.apply(
            storage::KeyView{key.data, key.len},
            storage::ValueView{value.data, value.len},
            version,
            tombstone
                ? storage::State::TOMBSTONE
                : storage::State::LIVE
        );

    response.result = map_storage_result(storage_result);
    return response.result;
}

Result Engine::execute_set(
    const protocol::DecodedMessage& message,
    storage::Version version,
    OperationResponse& response
) const
{
    const protocol::ValueView value =
        message.payload.set_request.value;

    if (value.len > storage::VALUE_MAX) {
        response.result = Result::VALUE_TOO_LARGE;
        return response.result;
    }

    const storage::Result result =
        storage_.set(
            storage::KeyView{response.key, response.key_length},
            storage::ValueView{value.data, value.len},
            version
        );

    response.result = map_storage_result(result);
    return response.result;
}

Result Engine::execute_get(
    const protocol::DecodedMessage& message,
    OperationResponse& response
) const
{
    (void)message;

    std::size_t value_length = 0;

    const storage::Result result =
        storage_.get(
            storage::KeyView{response.key, response.key_length},
            response.value,
            sizeof(response.value),
            value_length
        );

    response.result = map_storage_result(result);

    response.value_length =
        response.result == Result::OK
            ? static_cast<std::uint16_t>(value_length)
            : 0;

    return response.result;
}

Result Engine::execute_delete(
    const protocol::DecodedMessage& message,
    storage::Version version,
    OperationResponse& response
) const
{
    (void)message;

    const storage::Result result =
        storage_.remove(
            storage::KeyView{response.key, response.key_length},
            version
        );

    response.result = map_storage_result(result);
    return response.result;
}

Result Engine::map_storage_result(
    storage::Result result
)
{
    switch (result) {
        case storage::Result::OK:
            return Result::OK;

        case storage::Result::NOT_FOUND:
            return Result::NOT_FOUND;

        case storage::Result::STALE_VERSION:
            return Result::STALE_VERSION;

        case storage::Result::KEY_TOO_LARGE:
            return Result::KEY_TOO_LARGE;

        case storage::Result::VALUE_TOO_LARGE:
            return Result::VALUE_TOO_LARGE;

        case storage::Result::INVALID_ARGUMENT:
            return Result::INVALID_REQUEST;

        case storage::Result::NOT_INITIALIZED:
            return Result::NOT_READY;

        // INDEX_FULL, NO_SPACE, STORAGE_ERROR, CORRUPTED, ...
        default:
            return Result::STORAGE_ERROR;
    }
}

void Engine::copy_key(
    protocol::KeyView key,
    OperationResponse& response
)
{
    response.key_length =
        static_cast<std::uint8_t>(key.len);

    if (key.len == 0 || key.data == nullptr) {
        return;
    }

    std::memcpy(
        response.key,
        key.data,
        key.len
    );
}

} // namespace kv
