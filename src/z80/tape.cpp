#include "z80/tape.h"

#include <fstream>
#include <iterator>
#include <utility>

namespace z80 {

namespace {

::byte lo(word value) { return static_cast<::byte>(value & 0xFF); }
::byte hi(word value) { return static_cast<::byte>((value >> 8) & 0xFF); }

word read_word(const ::byte* p) {
    return static_cast<word>(p[0] | (static_cast<word>(p[1]) << 8));
}

void set_error(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

std::string at_offset(size_t offset) {
    return " at offset " + std::to_string(offset);
}

} // namespace

// ---------------------------------------------------------------- TapBlock --

::byte TapBlock::compute_checksum(::byte flag, const ::byte* data, size_t length) {
    ::byte sum = flag;
    for (size_t i = 0; i < length; i++) {
        sum ^= data[i];
    }
    return sum;
}

::byte TapBlock::computed_checksum() const {
    return compute_checksum(flag, payload.data(), payload.size());
}

TapBlock TapBlock::make(::byte flag, const ::byte* data, size_t length) {
    TapBlock block;
    block.flag = flag;
    if (length > 0) {
        block.payload.assign(data, data + length);
    }
    block.checksum = compute_checksum(flag, data, length);
    return block;
}

TapBlock TapBlock::make(::byte flag, const std::vector<::byte>& data) {
    return make(flag, data.data(), data.size());
}

// --------------------------------------------------------------- TapHeader --

bool TapHeader::decode(const TapBlock& block, TapHeader& out) {
    if (block.flag != TAP_FLAG_HEADER) return false;
    if (block.payload.size() != TAP_HEADER_PAYLOAD_SIZE) return false;

    const ::byte* p = block.payload.data();
    if (p[0] > static_cast<::byte>(TapFileType::Code)) return false;

    out.type = static_cast<TapFileType>(p[0]);

    // The name field is a fixed 10 bytes, space-padded on the right. Trailing
    // spaces are padding as far as the tape is concerned, so trim them.
    size_t name_length = TAP_NAME_SIZE;
    while (name_length > 0 && p[1 + name_length - 1] == ' ') {
        name_length--;
    }
    out.name.assign(reinterpret_cast<const char*>(p + 1), name_length);

    out.data_length = read_word(p + 11);
    out.param1      = read_word(p + 13);
    out.param2      = read_word(p + 15);
    return true;
}

TapBlock TapHeader::encode() const {
    std::vector<::byte> payload(TAP_HEADER_PAYLOAD_SIZE, 0);

    payload[0] = static_cast<::byte>(type);
    for (size_t i = 0; i < TAP_NAME_SIZE; i++) {
        payload[1 + i] = (i < name.size()) ? static_cast<::byte>(name[i])
                                           : static_cast<::byte>(' ');
    }
    payload[11] = lo(data_length); payload[12] = hi(data_length);
    payload[13] = lo(param1);      payload[14] = hi(param1);
    payload[15] = lo(param2);      payload[16] = hi(param2);

    return TapBlock::make(TAP_FLAG_HEADER, payload);
}

// ----------------------------------------------------------------- TapFile --

bool TapFile::parse(const ::byte* data, size_t length,
                    std::vector<TapBlock>& out, std::string* error) {
    out.clear();
    set_error(error, "");

    size_t pos = 0;
    while (pos < length) {
        const size_t frame_start = pos;

        if (length - pos < TAP_BLOCK_OVERHEAD) {
            set_error(error, "truncated block length" + at_offset(frame_start)
                             + ": expected 2 bytes, found "
                             + std::to_string(length - pos));
            return false;
        }
        const size_t block_length = read_word(data + pos);
        pos += 2;

        if (block_length < TAP_BLOCK_OVERHEAD) {
            set_error(error, "block length " + std::to_string(block_length)
                             + at_offset(frame_start)
                             + " is too small to hold a flag and a checksum");
            return false;
        }
        if (length - pos < block_length) {
            set_error(error, "truncated block body" + at_offset(frame_start)
                             + ": length says " + std::to_string(block_length)
                             + " but only " + std::to_string(length - pos)
                             + " bytes remain");
            return false;
        }

        TapBlock block;
        block.flag = data[pos];
        block.payload.assign(data + pos + 1, data + pos + block_length - 1);
        block.checksum = data[pos + block_length - 1];
        out.push_back(std::move(block));

        pos += block_length;
    }
    return true;
}

std::vector<::byte> TapFile::serialise() const {
    size_t total = 0;
    for (const TapBlock& block : blocks_) {
        total += TAP_BLOCK_OVERHEAD + block.tap_length();
    }

    std::vector<::byte> out;
    out.reserve(total);
    for (const TapBlock& block : blocks_) {
        const size_t block_length = block.tap_length();
        out.push_back(static_cast<::byte>(block_length & 0xFF));
        out.push_back(static_cast<::byte>((block_length >> 8) & 0xFF));
        out.push_back(block.flag);
        out.insert(out.end(), block.payload.begin(), block.payload.end());
        out.push_back(block.checksum);
    }
    return out;
}

bool TapFile::validate(std::string* error) const {
    set_error(error, "");
    for (size_t i = 0; i < blocks_.size(); i++) {
        if (blocks_[i].payload.size() > TAP_MAX_PAYLOAD) {
            set_error(error, "block " + std::to_string(i) + " payload is "
                             + std::to_string(blocks_[i].payload.size())
                             + " bytes, which will not fit the 16-bit length "
                               "prefix (max "
                             + std::to_string(TAP_MAX_PAYLOAD) + ")");
            return false;
        }
    }
    return true;
}

bool TapFile::load(const std::string& path) {
    blocks_.clear();
    last_error_.clear();

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        last_error_ = "cannot open '" + path + "' for reading";
        return false;
    }

    std::vector<::byte> raw((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
    if (file.bad()) {
        last_error_ = "read error on '" + path + "'";
        return false;
    }

    return parse(raw.data(), raw.size(), blocks_, &last_error_);
}

bool TapFile::save(const std::string& path) const {
    last_error_.clear();
    if (!validate(&last_error_)) return false;

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        last_error_ = "cannot open '" + path + "' for writing";
        return false;
    }

    const std::vector<::byte> raw = serialise();
    if (!raw.empty()) {
        file.write(reinterpret_cast<const char*>(raw.data()),
                   static_cast<std::streamsize>(raw.size()));
    }
    file.flush();
    if (!file) {
        last_error_ = "write error on '" + path + "'";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- TapeDeck --

bool TapeDeck::insert(std::string path)
{
    path_ = std::move(path);
    return insert();
}

bool TapeDeck::insert()
{
    position_ = 0;
    inserted_ = false;
    last_error_.clear();

    if (!tape_.load(path_)) {
        last_error_ = tape_.last_error();
        tape_.clear();
        return false;
    }
    inserted_ = true;
    return true;
}

void TapeDeck::new_tape()
{
    tape_.clear();
    position_ = 0;
    last_error_.clear();
    // A blank tape is still a tape in the deck, so loads report EndOfTape
    // rather than NoTape. The file on disk is untouched -- see the header.
    inserted_ = true;
}

void TapeDeck::eject()
{
    tape_.clear();
    position_ = 0;
    inserted_ = false;
    last_error_.clear();
}

TapeDeck::LoadOutcome TapeDeck::load_block(::byte expected_flag, Memory& memory,
                                           word dest, word length, bool verify)
{
    LoadOutcome outcome;

    if (!inserted_) { outcome.result = LoadResult::NoTape;    return outcome; }
    if (at_end())   { outcome.result = LoadResult::EndOfTape; return outcome; }

    const TapBlock& block = tape_.blocks()[position_];

    // The tape moves on whatever happens -- see the note in the header on how
    // this makes the ROM's retry loop skip non-matching blocks.
    position_++;

    if (block.flag != expected_flag) {
        outcome.result = LoadResult::FlagMismatch;
        return outcome;
    }
    if (block.payload.size() < length) {
        outcome.result = LoadResult::TooShort;
        return outcome;
    }

    for (size_t i = 0; i < length; i++) {
        // A word, so that a transfer running off the top of memory wraps to
        // zero as it does on real hardware, rather than off the array.
        const word address = dest + i;
        if (verify) {
            if (memory.read(address) != block.payload[i]) {
                outcome.transferred = static_cast<word>(i);
                outcome.result = LoadResult::VerifyMismatch;
                return outcome;
            }
        } else {
            // A load into ROM is discarded, as on real hardware.
            memory.write(address, block.payload[i]);
        }
    }
    outcome.transferred = length;

    if (block.payload.size() != length) {
        // The ROM reads `length` bytes and then takes the *next* byte as the
        // checksum, which will not match the XOR of a longer block -- so a
        // short read is a tape error on real hardware too.
        outcome.result = LoadResult::LengthMismatch;
        return outcome;
    }
    if (!block.checksum_valid()) {
        outcome.result = LoadResult::BadChecksum;
        return outcome;
    }

    outcome.result = LoadResult::Ok;
    return outcome;
}

TapeDeck::SaveResult TapeDeck::save_block(::byte flag, Memory& memory,
                                          word src, word length)
{
    if (path_.empty())            return SaveResult::NoPath;
    if (length > TAP_MAX_PAYLOAD) return SaveResult::TooLarge;

    std::vector<::byte> payload(length);
    for (size_t i = 0; i < length; i++) {
        // A word, so a read running off the top of memory wraps to zero.
        const word address = src + i;
        payload[i] = memory.read(address);
    }

    // Recording goes on the end, as on a cassette; the read head is left where
    // it is. See the header note for why, and for what accumulates.
    tape_.append(TapBlock::make(flag, payload));

    if (!tape_.save(path_)) {
        last_error_ = tape_.last_error();
        return SaveResult::WriteFailed;
    }

    // There is now a tape in the deck whether or not one was inserted before,
    // so it can be loaded straight back.
    inserted_ = true;
    last_error_.clear();
    return SaveResult::Ok;
}

} // namespace z80
