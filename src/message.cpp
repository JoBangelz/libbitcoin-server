#include <obelisk/message.hpp>

#include <random>
#include <bitcoin/format.hpp>
#include <bitcoin/utility/assert.hpp>
#include <bitcoin/utility/sha256.hpp>
#include <obelisk/zmq_message.hpp>

using namespace bc;

bool incoming_message::recv(zmq::socket_t& socket)
{
    zmq_message message;
    message.recv(socket);
    const data_stack& parts = message.parts();
    if (parts.size() == 1)
    {
        id_ = std::numeric_limits<uint32_t>::max();
        command_ = std::string(parts[0].begin(), parts[0].end());
        return true;
    }
    else if (parts.size() != 5 && parts.size() != 6)
        return false;
    auto it = parts.begin();
    // Read destination if exists.
    if (parts.size() == 6)
    {
        dest_ = *it;
        ++it;
    }
    // Discard empty frame.
    ++it;
    // [ COMMAND ]
    const data_chunk& raw_command = *it;
    command_ = std::string(raw_command.begin(), raw_command.end());
    ++it;
    // [ ID ]
    const data_chunk& raw_id = *it;
    if (raw_id.size() != 4)
        return false;
    id_ = cast_chunk<uint32_t>(raw_id);
    ++it;
    // [ DATA ]
    data_ = *it;
    ++it;
    // [ CHECKSUM ]
    const data_chunk& raw_checksum = *it;
    uint32_t checksum = cast_chunk<uint32_t>(raw_checksum);
    if (checksum != generate_sha256_checksum(data_))
        return false;
    ++it;
    BITCOIN_ASSERT(it == parts.end());
    return true;
}

bool incoming_message::is_signal() const
{
    return id_ == std::numeric_limits<uint32_t>::max();
}

const bc::data_chunk incoming_message::dest() const
{
    return dest_;
}
const std::string& incoming_message::command() const
{
    return command_;
}
const uint32_t incoming_message::id() const
{
    return id_;
}
const data_chunk& incoming_message::data() const
{
    return data_;
}

outgoing_message::outgoing_message()
{
}

outgoing_message::outgoing_message(
    const std::string& command, const data_chunk& data)
  : command_(command), id_(rand()), data_(data)
{
}

outgoing_message::outgoing_message(
    const incoming_message& request, const data_chunk& data)
  : dest_(request.dest()), command_(request.command()),
    id_(request.id()), data_(data)
{
}

void outgoing_message::send(zmq::socket_t& socket) const
{
    zmq_message message;
    if (!dest_.empty())
        message.append(dest_);
    message.append(data_chunk{0x00});
    // [ COMMAND ]
    // [ ID ]
    // [ DATA ]
    // [ CHECKSUM ]
    message.append(data_chunk(command_.begin(), command_.end()));
    data_chunk raw_id = uncast_type(id_);
    BITCOIN_ASSERT(raw_id.size() == 4);
    message.append(raw_id);
    message.append(data_);
    data_chunk raw_checksum = uncast_type(generate_sha256_checksum(data_));
    BITCOIN_ASSERT(raw_checksum.size() == 4);
    message.append(raw_checksum);
    message.send(socket);
}

const uint32_t outgoing_message::id() const
{
    return id_;
}

