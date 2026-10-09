#include "kv_engine.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace kv {

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
    const membership::Manager& membership,
    std::uint64_t now_ms,
    OperationResponse& response
) const
{
    response = OperationResponse{};

    if (!ready() || !membership.ready()) {
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

    if (key.len > storage::KEY_MAX) {
        response.result = Result::KEY_TOO_LARGE;
        return response.result;
    }

    placement::Assignment assignment{};

    if (
        !placement_.locate(
            key.data,
            key.len,
            membership,
            now_ms,
            assignment
        )
    ) {
        response.result = Result::NOT_RESPONSIBLE;
        copy_key(key, response);
        return response.result;
    }

    if (assignment.primary != local_node_id_) {
        response.result = Result::NOT_RESPONSIBLE;
        copy_key(key, response);
        return response.result;
    }

    switch (message.header.msg_type) {
        case protocol::MessageType::SET_REQ:
            return execute_set(message, response);

        case protocol::MessageType::GET_REQ:
            return execute_get(message, response);

        case protocol::MessageType::DEL_REQ:
            return execute_delete(message, response);

        default:
            response.result = Result::INVALID_REQUEST;
            return response.result;
    }
}

Result Engine::execute_set(
    const protocol::DecodedMessage& message,
    OperationResponse& response
) const
{
    const protocol::KeyView key =
        message.payload.set_request.key;

    const protocol::ValueView value =
        message.payload.set_request.value;

    if (key.len > storage::KEY_MAX) {
        response.result = Result::KEY_TOO_LARGE;
        return response.result;
    }

    if (value.len > storage::VALUE_MAX) {
        response.result = Result::VALUE_TOO_LARGE;
        return response.result;
    }

    copy_key(key, response);

    const storage::Version version{
        message.header.hlc,
        message.header.origin_node_id
    };

    const storage::Result result =
        storage_.set(
            storage::KeyView{
                key.data,
                key.len
            },
            storage::ValueView{
                value.data,
                value.len
            },
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
    const protocol::KeyView key =
        message.payload.get_request.key;

    if (key.len > storage::KEY_MAX) {
        response.result = Result::KEY_TOO_LARGE;
        return response.result;
    }

    copy_key(key, response);

    std::size_t value_length = 0;

    const storage::Result result =
        storage_.get(
            storage::KeyView{
                key.data,
                key.len
            },
            response.value,
            sizeof(response.value),
            value_length
        );

    response.result = map_storage_result(result);

    if (response.result == Result::OK) {
        response.value_length =
            static_cast<std::uint16_t>(
                value_length
            );
    } else {
        response.value_length = 0;
    }

    return response.result;
}

Result Engine::execute_delete(
    const protocol::DecodedMessage& message,
    OperationResponse& response
) const
{
    const protocol::KeyView key =
        message.payload.delete_request.key;

    if (key.len > storage::KEY_MAX) {
        response.result = Result::KEY_TOO_LARGE;
        return response.result;
    }

    copy_key(key, response);

    const storage::Version version{
        message.header.hlc,
        message.header.origin_node_id
    };

    const storage::Result result =
        storage_.remove(
            storage::KeyView{
                key.data,
                key.len
            },
            version
        );

    response.result = map_storage_result(result);

    return response.result;
}

Result Engine::map_storage_result(
    storage::Result result
) const
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

        case storage::Result::NOT_INITIALIZED:
            return Result::NOT_READY;

        default:
            return Result::STORAGE_ERROR;
    }
}

void Engine::copy_key(
    protocol::KeyView key,
    OperationResponse& response
) const
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