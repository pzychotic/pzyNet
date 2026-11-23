// BitStream encodings of RakNet's own types and strings, as upstream RakNet 4.081 writes them
// (BitStream.h, RakString.cpp, RakWString.cpp). Built only on the streams' public primitives.

#include "pzynet/raknet/bit_stream.hpp"

namespace pzynet
{
namespace raknet
{

// --- 24-bit integers -------------------------------------------------------------------------------

void BitStreamWriter::write_uint24(std::uint32_t value)
{
    const std::uint8_t bytes[3] = {static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8),
                                   static_cast<std::uint8_t>(value >> 16)};
    write_aligned_bytes(ByteView(bytes, 3));
}

std::uint32_t BitStreamReader::read_uint24()
{
    std::uint8_t bytes[3] = {};
    if (!read_aligned_bytes(MutableByteView(bytes, 3)))
        return 0;
    return bytes[0] | (std::uint32_t{bytes[1]} << 8) | (std::uint32_t{bytes[2]} << 16);
}

// --- System addresses ------------------------------------------------------------------------------

namespace
{
constexpr std::uint8_t ipv4_version = 4;
}

void BitStreamWriter::write(const Endpoint& endpoint, std::error_code& ec)
{
    // RakNet writes an IPv6 SystemAddress as version 6 and a raw sockaddr_in6, and only when built with
    // IPv6 support; a build without it writes the version byte alone. Neither is worth reproducing.
    if (!endpoint.address().is_v4())
    {
        ec = make_error_code(Errc::address_family_mismatch);
        return;
    }
    ec.clear();
    const auto address = endpoint.address().v4_bytes();
    const std::uint8_t bytes[7] = {ipv4_version,
                                   static_cast<std::uint8_t>(~address[0]),
                                   static_cast<std::uint8_t>(~address[1]),
                                   static_cast<std::uint8_t>(~address[2]),
                                   static_cast<std::uint8_t>(~address[3]),
                                   static_cast<std::uint8_t>(endpoint.port() >> 8),
                                   static_cast<std::uint8_t>(endpoint.port())};
    write_bytes(ByteView(bytes, sizeof bytes));
}

void write(BitStreamWriter& out, const Endpoint& endpoint)
{
    std::error_code ec;
    out.write(endpoint, ec);
    detail::throw_if(ec, "BitStreamWriter::write(Endpoint)");
}

void read(BitStreamReader& in, Endpoint& endpoint)
{
    if (in.read<std::uint8_t>() != ipv4_version)
    {
        in.fail();
        return;
    }
    std::uint8_t bytes[6] = {};
    if (!in.read_bytes(MutableByteView(bytes, sizeof bytes)))
        return;
    const auto port = static_cast<std::uint16_t>((bytes[4] << 8) | bytes[5]);
    endpoint = Endpoint(IpAddress::v4(static_cast<std::uint8_t>(~bytes[0]), static_cast<std::uint8_t>(~bytes[1]),
                                      static_cast<std::uint8_t>(~bytes[2]), static_cast<std::uint8_t>(~bytes[3])),
                        port);
}

// --- Length-prefixed strings -----------------------------------------------------------------------

namespace
{
constexpr std::size_t max_string_length = 0xFFFF; // the uint16 length prefix
}

void BitStreamWriter::write_string(std::string_view text, std::error_code& ec)
{
    if (text.size() > max_string_length)
    {
        ec = make_error_code(Errc::invalid_argument);
        return;
    }
    ec.clear();
    write(static_cast<std::uint16_t>(text.size()));
    write_aligned_bytes(ByteView(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

void BitStreamWriter::write_string(std::string_view text)
{
    std::error_code ec;
    write_string(text, ec);
    detail::throw_if(ec, "BitStreamWriter::write_string");
}

std::string BitStreamReader::read_string()
{
    const std::size_t length = read<std::uint16_t>();
    align();
    if (!ok_ || bits_remaining() < length * 8)
    {
        fail();
        return {};
    }
    std::string text(length, '\0');
    read_bytes(MutableByteView(reinterpret_cast<std::uint8_t*>(&text[0]), length));
    return text;
}

// --- Wide strings ----------------------------------------------------------------------------------
// Ticket #5 asks for "the non-Huffman path for compressed strings". We read that as upstream 4.081's
// WriteCompressed(RakWString) / WriteCompressed(const wchar_t*), which serialize with
// RakWString::Serialize instead of the StringCompressor (RakWString.cpp:289-309): a uint16 count,
// then one uint16 per wchar_t. We take UTF-16 code units: exactly what upstream writes where wchar_t
// is UTF-16 (Windows), and for every Basic Multilingual Plane character where wchar_t is 32 bits;
// there upstream truncates characters beyond the BMP to 16 bits, which is not worth reproducing.
// (WriteAlignedBytesSafe, a compressed length then aligned bytes, is a raw-bytes helper rather than
// a string encoding and is not covered here.)

void BitStreamWriter::write_wide_string(std::u16string_view text, std::error_code& ec)
{
    if (text.size() > max_string_length)
    {
        ec = make_error_code(Errc::invalid_argument);
        return;
    }
    ec.clear();
    write(static_cast<std::uint16_t>(text.size()));
    for (const char16_t unit : text)
        write(static_cast<std::uint16_t>(unit));
}

void BitStreamWriter::write_wide_string(std::u16string_view text)
{
    std::error_code ec;
    write_wide_string(text, ec);
    detail::throw_if(ec, "BitStreamWriter::write_wide_string");
}

std::u16string BitStreamReader::read_wide_string()
{
    const std::size_t length = read<std::uint16_t>();
    if (!ok_ || bits_remaining() < length * 16)
    {
        fail();
        return {};
    }
    std::u16string text(length, u'\0');
    for (char16_t& unit : text)
        unit = static_cast<char16_t>(read<std::uint16_t>());
    return text;
}

// --- Huffman-compressed strings --------------------------------------------------------------------
// Upstream 4.081's WriteCompressed(RakString) / WriteCompressed(const char*) call
// RakString::SerializeCompressed with language 0 and no language id, which is
// StringCompressor::EncodeString(str, 0xFFFF, bs) (StringCompressor.cpp:366-397) on the tree
// HuffmanEncodingTree::GenerateFromFrequencyTable builds from englishCharacterFrequencies
// (DS_HuffmanEncodingTree.cpp). Decoding is StringCompressor::DecodeString (:399-427).

namespace
{

// Upstream's englishCharacterFrequencies (StringCompressor.cpp:62-319), value for value.
constexpr std::uint32_t english_character_frequencies[256] = {
    0,     0,  0,  0,  0,   0,   0,  0,   0,  0,   722, 0,  0,   2,  0,   0,                             // 0x00
    0,     0,  0,  0,  0,   0,   0,  0,   0,  0,   0,   0,  0,   0,  0,   0,                             // 0x10
    11084, 58, 63, 1,  0,   31,  0,  317, 64, 64,  44,  0,  695, 62, 980, 266,                           // 0x20
    69,    67, 56, 7,  73,  3,   14, 2,   69, 1,   167, 9,  1,   2,  25,  94,                            // 0x30
    0,     195, 139, 34, 96, 48, 103, 56, 125, 653, 21, 5, 23, 64, 85, 44,                               // 0x40
    34,    7,  92, 76, 147, 12,  14, 57,  15, 39,  15,  1,  1,   1,  2,   3,                             // 0x50
    0,     3611, 845, 1077, 1884, 5870, 841, 1057, 2501, 3212, 164, 531, 2019, 1330, 3056, 4037,         // 0x60
    848,   47, 2586, 2919, 4771, 1707, 535, 1106, 152, 1243, 100, 0, 2, 0, 10, 0,                        // 0x70
    // 0x80-0xFF: all 0
};

constexpr std::size_t max_huffman_string_length = 0xFFFE; // EncodeString's maxCharsToWrite - 1
constexpr std::uint16_t leaf_count = 256;

struct HuffmanCode
{
    std::uint8_t bits[32] = {}; // from the root, most significant bit first; at most 255 bits deep
    std::size_t length = 0;
};

// Nodes 0-255 are the leaves (the byte values), 256-510 the inner nodes in the order they were made.
class HuffmanTree
{
public:
    HuffmanTree()
    {
        std::uint32_t weight[2 * leaf_count - 1] = {};
        std::uint16_t parent[2 * leaf_count - 1] = {};

        // A list sorted by weight; a node goes in before the first node of the same or a greater weight
        // (upstream's InsertNodeIntoSortedList). That tie order decides every code.
        std::vector<std::uint16_t> sorted;
        const auto insert = [&](std::uint16_t node) {
            auto at = sorted.begin();
            while (at != sorted.end() && weight[*at] < weight[node])
                ++at;
            sorted.insert(at, node);
        };
        for (std::uint16_t c = 0; c < leaf_count; ++c)
        {
            weight[c] = english_character_frequencies[c] != 0 ? english_character_frequencies[c] : 1;
            insert(c);
        }

        // Merge the two lightest trees, the lighter one (popped first) on the left, until one is left.
        std::uint16_t next = leaf_count;
        for (;;)
        {
            const std::uint16_t lesser = sorted[0];
            const std::uint16_t greater = sorted[1];
            sorted.erase(sorted.begin(), sorted.begin() + 2);
            const std::uint16_t node = next++;
            children_[node - leaf_count][0] = lesser;
            children_[node - leaf_count][1] = greater;
            weight[node] = weight[lesser] + weight[greater];
            parent[lesser] = node;
            parent[greater] = node;
            if (sorted.empty())
            {
                root_ = node;
                break;
            }
            insert(node);
        }

        // A code is the path from the root to the leaf, 0 for left and 1 for right.
        for (std::uint16_t c = 0; c < leaf_count; ++c)
        {
            bool path[leaf_count] = {}; // leaf to root
            std::size_t length = 0;
            for (std::uint16_t node = c; node != root_; node = parent[node])
                path[length++] = children_[parent[node] - leaf_count][1] == node;
            HuffmanCode& code = codes_[c];
            code.length = length;
            for (std::size_t i = 0; i < length; ++i)
                if (path[length - 1 - i])
                    code.bits[i / 8] |= static_cast<std::uint8_t>(0x80u >> (i % 8));
        }
    }

    const HuffmanCode& code(std::uint8_t c) const noexcept { return codes_[c]; }
    std::uint16_t root() const noexcept { return root_; }
    static bool is_leaf(std::uint16_t node) noexcept { return node < leaf_count; }
    std::uint16_t child(std::uint16_t node, bool right) const noexcept
    {
        return children_[node - leaf_count][right ? 1 : 0];
    }

    // EncodeArray pads to a byte boundary with the first `bit_count` bits of the first code (by byte
    // value) longer than that, so that the padding never completes a code.
    const HuffmanCode& padding(std::size_t bit_count) const noexcept
    {
        for (const HuffmanCode& code : codes_)
            if (code.length > bit_count)
                return code;
        assert(false); // a 256-leaf tree always has codes longer than 7 bits
        return codes_[0];
    }

private:
    std::uint16_t children_[leaf_count - 1][2] = {};
    HuffmanCode codes_[leaf_count];
    std::uint16_t root_ = 0;
};

const HuffmanTree& english_tree()
{
    static const HuffmanTree tree;
    return tree;
}

} // namespace

void BitStreamWriter::write_huffman_string(std::string_view text, std::error_code& ec)
{
    if (text.size() > max_huffman_string_length || text.find('\0') != std::string_view::npos)
    {
        ec = make_error_code(Errc::invalid_argument);
        return;
    }
    ec.clear();
    const HuffmanTree& tree = english_tree();
    std::size_t code_bits = 0;
    for (const char c : text)
        code_bits += tree.code(static_cast<std::uint8_t>(c)).length;
    const std::size_t padding_bits = (8 - code_bits % 8) % 8;

    write_compressed(static_cast<std::uint32_t>(code_bits + padding_bits));
    for (const char c : text)
    {
        const HuffmanCode& code = tree.code(static_cast<std::uint8_t>(c));
        write_bits(ByteView(code.bits, sizeof code.bits), code.length, false);
    }
    if (padding_bits != 0)
        write_bits(ByteView(tree.padding(padding_bits).bits, sizeof HuffmanCode::bits), padding_bits, false);
}

void BitStreamWriter::write_huffman_string(std::string_view text)
{
    std::error_code ec;
    write_huffman_string(text, ec);
    detail::throw_if(ec, "BitStreamWriter::write_huffman_string");
}

std::string BitStreamReader::read_huffman_string()
{
    const std::uint32_t bit_length = read_compressed<std::uint32_t>();
    if (!ok_ || bits_remaining() < bit_length)
    {
        fail();
        return {};
    }
    // Every announced bit is walked, as DecodeArray does, but only the bytes before the first zero byte
    // and at most 65534 of them are kept (DecodeString's 0xFFFF buffer, NUL-terminated).
    const HuffmanTree& tree = english_tree();
    std::string text;
    bool ended = false;
    std::uint16_t node = tree.root();
    for (std::uint32_t i = 0; i < bit_length; ++i)
    {
        node = tree.child(node, read_bit());
        if (!HuffmanTree::is_leaf(node))
            continue;
        if (node == 0 || text.size() == max_huffman_string_length)
            ended = true;
        if (!ended)
            text.push_back(static_cast<char>(node));
        node = tree.root();
    }
    return text;
}

} // namespace raknet
} // namespace pzynet
