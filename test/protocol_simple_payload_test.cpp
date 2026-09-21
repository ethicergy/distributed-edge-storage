#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "protocol.h"

using namespace protocol;

int main() {
    const std::uint8_t key[] = {
        't', 'e', 'm', 'p'
    };

    const std::uint8_t value[] = {
        '3', '1', '.', '5'
    };

    std::uint8_t buffer[MAX_PAYLOAD_SIZE]{};
    std::size_t bytes_written = 0;

    // GET_REQ
    GetRequestPayload get_request{{key, 4}};

    Result result = serialize_get_request(
        get_request,
        buffer,
        sizeof(buffer),
        bytes_written
    );

    assert(result == Result::OK);
    assert(bytes_written == 5);
    assert(buffer[0] == 4);
    assert(std::memcmp(buffer + 1, key, 4) == 0);

    GetRequestPayload decoded_get{};
    result = deserialize_get_request(
        buffer,
        bytes_written,
        decoded_get
    );

    assert(result == Result::OK);
    assert(decoded_get.key.len == 4);
    assert(std::memcmp(decoded_get.key.data, key, 4) == 0);

    // SET_ACK
    SetAckPayload set_ack{
        StatusCode::OK,
        {key, 4}
    };

    result = serialize_set_ack(
        set_ack,
        buffer,
        sizeof(buffer),
        bytes_written
    );

    assert(result == Result::OK);
    assert(bytes_written == 6);
    assert(buffer[0] == 0x00);
    assert(buffer[1] == 4);
    assert(std::memcmp(buffer + 2, key, 4) == 0);

    SetAckPayload decoded_set_ack{};
    result = deserialize_set_ack(
        buffer,
        bytes_written,
        decoded_set_ack
    );

    assert(result == Result::OK);
    assert(decoded_set_ack.status == StatusCode::OK);
    assert(decoded_set_ack.key.len == 4);

    // GET_RESP success
    GetResponsePayload get_response{
        StatusCode::OK,
        {key, 4},
        {value, 4}
    };

    result = serialize_get_response(
        get_response,
        buffer,
        sizeof(buffer),
        bytes_written
    );

    assert(result == Result::OK);
    assert(bytes_written == 11);

    GetResponsePayload decoded_get_response{};
    result = deserialize_get_response(
        buffer,
        bytes_written,
        decoded_get_response
    );

    assert(result == Result::OK);
    assert(decoded_get_response.status == StatusCode::OK);
    assert(decoded_get_response.key.len == 4);
    assert(decoded_get_response.value.len == 4);

    assert(std::memcmp(
        decoded_get_response.value.data,
        value,
        4
    ) == 0);

    // GET_RESP NOT_FOUND
    GetResponsePayload not_found{
        StatusCode::NOT_FOUND,
        {key, 4},
        {}
    };

    result = serialize_get_response(
        not_found,
        buffer,
        sizeof(buffer),
        bytes_written
    );

    assert(result == Result::OK);
    assert(bytes_written == 7);

    GetResponsePayload decoded_not_found{};
    result = deserialize_get_response(
        buffer,
        bytes_written,
        decoded_not_found
    );

    assert(result == Result::OK);
    assert(decoded_not_found.status == StatusCode::NOT_FOUND);
    assert(decoded_not_found.key.len == 4);
    assert(decoded_not_found.value.len == 0);

    // DEL_REQ
    DeleteRequestPayload delete_request{{key, 4}};

    result = serialize_delete_request(
        delete_request,
        buffer,
        sizeof(buffer),
        bytes_written
    );

    assert(result == Result::OK);
    assert(bytes_written == 5);

    DeleteRequestPayload decoded_delete_request{};
    result = deserialize_delete_request(
        buffer,
        bytes_written,
        decoded_delete_request
    );

    assert(result == Result::OK);
    assert(decoded_delete_request.key.len == 4);

    // DEL_ACK
    DeleteAckPayload delete_ack{
        StatusCode::OK,
        {key, 4}
    };

    result = serialize_delete_ack(
        delete_ack,
        buffer,
        sizeof(buffer),
        bytes_written
    );

    assert(result == Result::OK);
    assert(bytes_written == 6);

    DeleteAckPayload decoded_delete_ack{};
    result = deserialize_delete_ack(
        buffer,
        bytes_written,
        decoded_delete_ack
    );

    assert(result == Result::OK);
    assert(decoded_delete_ack.status == StatusCode::OK);

    // REPL_ACK
    ReplicationAckPayload replication_ack{
        StatusCode::OK,
        {key, 4}
    };

    result = serialize_replication_ack(
        replication_ack,
        buffer,
        sizeof(buffer),
        bytes_written
    );

    assert(result == Result::OK);
    assert(bytes_written == 6);

    ReplicationAckPayload decoded_replication_ack{};
    result = deserialize_replication_ack(
        buffer,
        bytes_written,
        decoded_replication_ack
    );

    assert(result == Result::OK);
    assert(decoded_replication_ack.status == StatusCode::OK);

    std::cout << "Simple payload serialization tests: PASS\n";
    std::cout << "Simple payload round-trip tests: PASS\n";

    return 0;
}