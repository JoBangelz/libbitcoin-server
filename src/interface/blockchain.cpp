/**
 * Copyright (c) 2011-2017 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/server/interface/blockchain.hpp>

#include <cstdint>
#include <cstddef>
#include <functional>
#include <bitcoin/blockchain.hpp>
#include <bitcoin/server/define.hpp>
#include <bitcoin/server/messages/message.hpp>
#include <bitcoin/server/server_node.hpp>
#include <bitcoin/server/utility/fetch_helpers.hpp>

namespace libbitcoin {
namespace server {

using namespace std::placeholders;
using namespace bc::blockchain;
using namespace bc::chain;
using namespace bc::wallet;

static const auto canonical_version = bc::message::version::level::canonical;

void blockchain::fetch_history2(server_node& node, const message& request,
    send_handler handler)
{
    static constexpr size_t limit = 0;
    size_t from_height;
    payment_address address;

    if (!unwrap_fetch_history_args(address, from_height, request))
    {
        handler(message(request, error::bad_stream));
        return;
    }

    LOG_DEBUG(LOG_SERVER)
        << "blockchain.fetch_history2(" << address.encoded()
        << ", from_height=" << from_height << ")";

    node.chain().fetch_history(address, limit, from_height,
        std::bind(send_history_result,
            _1, _2, request, handler));
}

void blockchain::fetch_transaction(server_node& node, const message& request,
    send_handler handler)
{
    hash_digest hash;

    if (!unwrap_fetch_transaction_args(hash, request))
    {
        handler(message(request, error::bad_stream));
        return;
    }

    LOG_DEBUG(LOG_SERVER)
        << "blockchain.fetch_transaction(" << encode_hash(hash) << ")";

    // The response is restricted to confirmed transactions.
    node.chain().fetch_transaction(hash, true,
        std::bind(transaction_fetched,
            _1, _2, _3, _4, request, handler));
}

void blockchain::fetch_last_height(server_node& node, const message& request,
    send_handler handler)
{
    const auto& data = request.data();

    if (!data.empty())
    {
        handler(message(request, error::bad_stream));
        return;
    }

    node.chain().fetch_last_height(
        std::bind(&blockchain::last_height_fetched,
            _1, _2, request, handler));
}

void blockchain::last_height_fetched(const code& ec, size_t last_height,
    const message& request, send_handler handler)
{
    BITCOIN_ASSERT(last_height <= max_uint32);
    auto last_height32 = static_cast<uint32_t>(last_height);

    // [ code:4 ]
    // [ heigh:4 ]
    const auto result = build_chunk(
    {
        message::to_bytes(ec),
        to_little_endian(last_height32)
    });

    handler(message(request, result));
}

void blockchain::fetch_block_header(server_node& node, const message& request,
    send_handler handler)
{
    const auto& data = request.data();

    if (data.size() == hash_size)
        blockchain::fetch_block_header_by_hash(node, request, handler);
    else if (data.size() == sizeof(uint32_t))
        blockchain::fetch_block_header_by_height(node, request, handler);
    else
        handler(message(request, error::bad_stream));
}

void blockchain::fetch_block_header_by_hash(server_node& node,
    const message& request, send_handler handler)
{
    const auto& data = request.data();
    BITCOIN_ASSERT(data.size() == hash_size);

    auto deserial = make_safe_deserializer(data.begin(), data.end());
    const auto block_hash = deserial.read_hash();

    node.chain().fetch_block_header(block_hash,
        std::bind(&blockchain::block_header_fetched,
            _1, _2, request, handler));
}

void blockchain::fetch_block_header_by_height(server_node& node,
    const message& request, send_handler handler)
{
    const auto& data = request.data();
    BITCOIN_ASSERT(data.size() == sizeof(uint32_t));

    auto deserial = make_safe_deserializer(data.begin(), data.end());
    const uint64_t height = deserial.read_4_bytes_little_endian();

    node.chain().fetch_block_header(height,
        std::bind(&blockchain::block_header_fetched,
            _1, _2, request, handler));
}

void blockchain::block_header_fetched(const code& ec, header_const_ptr header,
    const message& request, send_handler handler)
{
    // [ code:4 ]
    // [ block... ]
    const auto result = build_chunk(
    {
        message::to_bytes(ec),
        header->to_data(canonical_version)
    });

    handler(message(request, result));
}

void blockchain::fetch_block_transaction_hashes(server_node& node,
    const message& request, send_handler handler)
{
    const auto& data = request.data();

    if (data.size() == hash_size)
        fetch_block_transaction_hashes_by_hash(node, request, handler);
    else if (data.size() == sizeof(uint32_t))
        fetch_block_transaction_hashes_by_height(node, request, handler);
    else
        handler(message(request, error::bad_stream));
}

void blockchain::fetch_block_transaction_hashes_by_hash(server_node& node,
    const message& request, send_handler handler)
{
    const auto& data = request.data();
    BITCOIN_ASSERT(data.size() == hash_size);

    auto deserial = make_safe_deserializer(data.begin(), data.end());
    const auto block_hash = deserial.read_hash();
    node.chain().fetch_merkle_block(block_hash,
        std::bind(&blockchain::merkle_block_fetched,
            _1, _2, _3, request, handler));
}

void blockchain::fetch_block_transaction_hashes_by_height(server_node& node,
    const message& request, send_handler handler)
{
    const auto& data = request.data();
    BITCOIN_ASSERT(data.size() == sizeof(uint32_t));

    auto deserial = make_safe_deserializer(data.begin(), data.end());
    const size_t block_height = deserial.read_4_bytes_little_endian();
    node.chain().fetch_merkle_block(block_height,
        std::bind(&blockchain::merkle_block_fetched,
            _1, _2, _3, request, handler));
}

void blockchain::merkle_block_fetched(const code& ec, merkle_block_ptr block,
    size_t, const message& request, send_handler handler)
{
    // [ code:4 ]
    // [[ hash:32 ]...]
    data_chunk result(code_size + hash_size * block->hashes().size());
    auto serial = make_unsafe_serializer(result.begin());
    serial.write_error_code(ec);

    for (const auto& hash: block->hashes())
        serial.write_hash(hash);

    handler(message(request, result));
}

void blockchain::fetch_transaction_index(server_node& node,
    const message& request, send_handler handler)
{
    const auto& data = request.data();

    if (data.size() != hash_size)
    {
        handler(message(request, error::bad_stream));
        return;
    }

    auto deserial = make_safe_deserializer(data.begin(), data.end());
    const auto hash = deserial.read_hash();

    // The response is restricted to confirmed transactions (backward compat).
    node.chain().fetch_transaction_position(hash, false,
        std::bind(&blockchain::transaction_index_fetched,
            _1, _2, _3, request, handler));
}

void blockchain::transaction_index_fetched(const code& ec,
    size_t tx_position, size_t block_height, const message& request,
    send_handler handler)
{
    BITCOIN_ASSERT(tx_position <= max_uint32);
    BITCOIN_ASSERT(block_height <= max_uint32);

    auto tx_position32 = static_cast<uint32_t>(tx_position);
    auto block_height32 = static_cast<uint32_t>(block_height);

    // [ code:4 ]
    // [ block_height:4 ]
    // [ tx_position:4 ]
    const auto result = build_chunk(
    {
        message::to_bytes(ec),
        to_little_endian(block_height32),
        to_little_endian(tx_position32)
    });

    handler(message(request, result));
}

void blockchain::fetch_spend(server_node& node, const message& request,
    send_handler handler)
{
    const auto& data = request.data();

    if (data.size() != point_size)
    {
        handler(message(request, error::bad_stream));
        return;
    }

    using namespace boost::iostreams;
    static const auto fail_bit = stream<byte_source<data_chunk>>::failbit;
    stream<byte_source<data_chunk>> istream(data);
    istream.exceptions(fail_bit);
    chain::output_point outpoint;
    outpoint.from_data(istream);

    node.chain().fetch_spend(outpoint,
        std::bind(&blockchain::spend_fetched,
            _1, _2, request, handler));
}

void blockchain::spend_fetched(const code& ec, const chain::input_point& inpoint,
    const message& request, send_handler handler)
{
    // [ code:4 ]
    // [ hash:32 ]
    // [ index:4 ]
    const auto result = build_chunk(
    {
        message::to_bytes(ec),
        inpoint.to_data()
    });

    handler(message(request, result));
}

void blockchain::fetch_block_height(server_node& node,
    const message& request, send_handler handler)
{
    const auto& data = request.data();

    if (data.size() != hash_size)
    {
        handler(message(request, error::bad_stream));
        return;
    }

    auto deserial = make_safe_deserializer(data.begin(), data.end());
    const auto block_hash = deserial.read_hash();
    node.chain().fetch_block_height(block_hash,
        std::bind(&blockchain::block_height_fetched,
            _1, _2, request, handler));
}

void blockchain::block_height_fetched(const code& ec, size_t block_height,
    const message& request, send_handler handler)
{
    BITCOIN_ASSERT(block_height <= max_uint32);
    auto block_height32 = static_cast<uint32_t>(block_height);

    // [ code:4 ]
    // [ height:4 ]
    const auto result = build_chunk(
    {
        message::to_bytes(ec),
        to_little_endian(block_height32)
    });

    handler(message(request, result));
}

void blockchain::fetch_stealth2(server_node& node, const message& request,
    send_handler handler)
{
    const auto& data = request.data();

    if (data.empty())
    {
        handler(message(request, error::bad_stream));
        return;
    }

    auto deserial = make_safe_deserializer(data.begin(), data.end());

    // number_bits
    const auto bit_size = deserial.read_byte();

    if (data.size() != sizeof(uint8_t) + binary::blocks_size(bit_size) +
        sizeof(uint32_t))
    {
        handler(message(request, error::bad_stream));
        return;
    }

    // actual bitfield data
    const auto blocks = deserial.read_bytes(binary::blocks_size(bit_size));
    const binary prefix(bit_size, blocks);

    // from_height
    const size_t from_height = deserial.read_4_bytes_little_endian();

    node.chain().fetch_stealth(prefix, from_height,
        std::bind(&blockchain::stealth_fetched,
            _1, _2, request, handler));
}

void blockchain::stealth_fetched(const code& ec,
    const stealth_compact::list& stealth_results, const message& request,
    send_handler handler)
{
    static constexpr size_t row_size = hash_size + short_hash_size + hash_size;

    // [ code:4 ]
    // [[ ephemeral_key_hash:32 ][ address_hash:20 ][ tx_hash:32 ]...]
    data_chunk result(code_size + row_size * stealth_results.size());
    auto serial = make_unsafe_serializer(result.begin());
    serial.write_error_code(ec);

    for (const auto& row: stealth_results)
    {
        serial.write_hash(row.ephemeral_public_key_hash);
        serial.write_short_hash(row.public_key_hash);
        serial.write_hash(row.transaction_hash);
    }

    handler(message(request, result));
}

void blockchain::fetch_stealth_transaction(server_node& node,
    const message& request, send_handler handler)
{
    const auto& data = request.data();

    if (data.empty())
    {
        handler(message(request, error::bad_stream));
        return;
    }

    auto deserial = make_safe_deserializer(data.begin(), data.end());

    // number_bits
    const auto bit_size = deserial.read_byte();

    if (data.size() != sizeof(uint8_t) + binary::blocks_size(bit_size) +
        sizeof(uint32_t))
    {
        handler(message(request, error::bad_stream));
        return;
    }

    // actual bitfield data
    const auto blocks = deserial.read_bytes(binary::blocks_size(bit_size));
    const binary prefix(bit_size, blocks);

    // from_height
    const size_t from_height = deserial.read_4_bytes_little_endian();

    node.chain().fetch_stealth(prefix, from_height,
        std::bind(&blockchain::stealth_fetched2,
            _1, _2, request, handler));
}

void blockchain::stealth_fetched2(const code& ec,
    const stealth_compact::list& stealth_results, const message& request,
    send_handler handler)
{
    // [ code:4 ]
    // [[ tx_hash:32 ]...]
    data_chunk result(code_size + hash_size * stealth_results.size());
    auto serial = make_unsafe_serializer(result.begin());
    serial.write_error_code(ec);

    for (const auto& row: stealth_results)
        serial.write_hash(row.transaction_hash);

    handler(message(request, result));
}

// Save to blockchain and announce to all connected peers.
void blockchain::broadcast(server_node& node, const message& request,
    send_handler handler)
{
    const auto block = std::make_shared<bc::message::block>();

    if (!block->from_data(canonical_version, request.data()))
    {
        handler(message(request, error::bad_stream));
        return;
    }

    // Organize into our chain.
    block->validation.simulate = false;

    // This call is async but blocks on other organizations until started.
    // Subscribed channels will pick up and announce via inventory to peers.
    node.chain().organize(block,
        std::bind(handle_broadcast, _1, request, handler));
}

void blockchain::handle_broadcast(const code& ec, const message& request,
    send_handler handler)
{
    // Returns validation error or error::success.
    handler(message(request, ec));
}

void blockchain::validate(server_node& node, const message& request,
    send_handler handler)
{
    const auto block = std::make_shared<bc::message::block>();

    if (!block->from_data(canonical_version, request.data()))
    {
        handler(message(request, error::bad_stream));
        return;
    }

    // Simulate organization into our chain.
    block->validation.simulate = true;

    // This call is async but blocks on other organizations until started.
    node.chain().organize(block,
        std::bind(handle_validated, _1, request, handler));
}

void blockchain::handle_validated(const code& ec, const message& request,
    send_handler handler)
{
    // Returns validation error or error::success.
    handler(message(request, ec));
}

} // namespace server
} // namespace libbitcoin
