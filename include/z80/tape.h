#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "globals.h"
#include "z80_mem.h"

namespace z80 {

/// The first byte of every tape block says what kind of block it is.
enum : ::byte {
    TAP_FLAG_HEADER = 0x00,
    TAP_FLAG_DATA   = 0xFF,
};

/// A header block's payload is always 17 bytes: the ROM's LD-LOOK-H at 0x0767
/// calls LD-BYTES with DE = 0x0011 to read one.
const size_t TAP_HEADER_PAYLOAD_SIZE = 17;

/// Filenames occupy a fixed 10 bytes on tape, space-padded.
const size_t TAP_NAME_SIZE = 10;

/// A .tap length prefix counts the flag and checksum along with the payload,
/// so the smallest legal block is 2 bytes (an empty payload).
const size_t TAP_BLOCK_OVERHEAD = 2;

/// The length prefix is 16 bits, which caps what a single block can carry.
const size_t TAP_MAX_PAYLOAD = 0xFFFF - TAP_BLOCK_OVERHEAD;

/// One block exactly as it sits on tape. The 2-byte length prefix is .tap file
/// framing rather than part of the block, so it is not stored here.
struct TapBlock {
    ::byte flag = TAP_FLAG_DATA;
    std::vector<::byte> payload;

    /// The checksum byte as read from the file. Kept rather than recomputed on
    /// demand so that a corrupt tape can be *detected* instead of silently
    /// repaired -- checksum_valid() is the check the ROM itself performs.
    ::byte checksum = 0;

    /// Builds a block from raw payload bytes, computing the checksum.
    static TapBlock make(::byte flag, const ::byte* data, size_t length);
    static TapBlock make(::byte flag, const std::vector<::byte>& data);

    /// XOR of the flag byte and every payload byte.
    static ::byte compute_checksum(::byte flag, const ::byte* data, size_t length);
    ::byte computed_checksum() const;
    bool checksum_valid() const { return checksum == computed_checksum(); }

    /// The value the .tap length prefix carries: flag + payload + checksum.
    size_t tap_length() const { return payload.size() + TAP_BLOCK_OVERHEAD; }

    bool is_header() const { return flag == TAP_FLAG_HEADER; }
    bool is_data() const { return flag == TAP_FLAG_DATA; }

    bool operator==(const TapBlock& other) const {
        return flag == other.flag && checksum == other.checksum
            && payload == other.payload;
    }
    bool operator!=(const TapBlock& other) const { return !(*this == other); }
};

/// The first byte of a header block's payload. The ROM handles only these four.
enum class TapFileType : ::byte {
    Program     = 0,
    NumberArray = 1,
    CharArray   = 2,
    Code        = 3,
};

/// The decoded 17-byte payload of a header block.
struct TapHeader {
    TapFileType type = TapFileType::Program;

    /// Up to 10 bytes as stored on tape, with trailing spaces trimmed. Held as
    /// raw bytes rather than text: values >= 0x80 are BASIC tokens and block
    /// graphics, and must survive a round trip untouched.
    ///
    /// Note the two lossy edges, both matching what the ROM's SAVE does:
    /// a name longer than 10 bytes is truncated, and trailing spaces in a name
    /// are indistinguishable from padding, so they do not come back.
    std::string name;

    /// Payload length of the data block that follows this header.
    word data_length = 0;

    /// Program: autostart line number, >= 0x8000 meaning "no autostart".
    /// Code:    start address.
    /// Arrays:  the variable name is in the high byte.
    word param1 = 0;

    /// Program: offset from the program start to the variables area.
    /// Code:    unused, conventionally 0x8000.
    word param2 = 0;

    /// Decodes a header block. Fails if the flag is not TAP_FLAG_HEADER, the
    /// payload is not exactly 17 bytes, or the type byte is not 0-3.
    static bool decode(const TapBlock& block, TapHeader& out);

    /// Encodes to a 19-byte header block, padding or truncating name to 10.
    TapBlock encode() const;

    bool operator==(const TapHeader& other) const {
        return type == other.type && name == other.name
            && data_length == other.data_length
            && param1 == other.param1 && param2 == other.param2;
    }
    bool operator!=(const TapHeader& other) const { return !(*this == other); }
};

/// A whole .tap image held in memory: an ordered list of blocks and nothing
/// else. The format carries no file header, no index and no end marker, so an
/// empty TapFile is a valid blank tape that serialises to zero bytes.
class TapFile {
public:
    /// Parses raw .tap bytes into `out`. Blocks decoded before an error are
    /// still placed in `out`, so a tape truncated by a bad recording can be
    /// used up to the point of damage; the return value says whether the whole
    /// image was well formed.
    static bool parse(const ::byte* data, size_t length,
                      std::vector<TapBlock>& out, std::string* error = nullptr);

    std::vector<::byte> serialise() const;

    /// Rejects blocks whose length would not fit the 16-bit prefix.
    bool validate(std::string* error = nullptr) const;

    /// Replaces the current contents. On failure the blocks read before the
    /// error are retained, mirroring parse().
    bool load(const std::string& path);
    bool save(const std::string& path) const;

    void clear() { blocks_.clear(); }
    void append(const TapBlock& block) { blocks_.push_back(block); }

    const std::vector<TapBlock>& blocks() const { return blocks_; }
    std::vector<TapBlock>& blocks() { return blocks_; }
    size_t size() const { return blocks_.size(); }
    bool empty() const { return blocks_.empty(); }

    /// Reason the last load()/save()/validate() failed; empty after success.
    const std::string& last_error() const { return last_error_; }

private:
    std::vector<TapBlock> blocks_;
    mutable std::string last_error_;
};

/// The tape as the machine sees it: a TapFile plus a play position.
///
/// load_block()/save_block() are the seam. Everything above this class deals
/// only in whole blocks, so a signal-level backend -- one that emulates the
/// EAR bit per T-state and lets the ROM's own edge detector do the work -- can
/// replace these two operations without the file layer changing at all.
class TapeDeck {
public:
    /// Why a load attempt ended. Only Ok sets the carry flag on return from
    /// LD-BYTES; every other value is reported to the ROM as a failure.
    enum class LoadResult {
        Ok,              // the block matched and transferred cleanly
        NoTape,          // nothing inserted
        EndOfTape,       // played past the last block
        FlagMismatch,    // wrong kind of block; the ROM will ask again
        TooShort,        // fewer bytes on tape than the caller asked for
        LengthMismatch,  // more bytes on tape than the caller asked for
        BadChecksum,     // transferred, but the checksum did not match
        VerifyMismatch,  // VERIFY found a byte that differs
    };

    struct LoadOutcome {
        LoadResult result = LoadResult::NoTape;
        /// Bytes actually stored (or compared, when verifying). Real hardware
        /// leaves IX advanced by this much even when the load fails.
        word transferred = 0;
    };

    /// Why a save attempt ended. SA-BYTES has no way to report failure to the
    /// ROM -- a real recorder cannot tell the Spectrum it was not plugged in --
    /// so these are for the host, not the emulated machine.
    enum class SaveResult {
        Ok,
        NoPath,       // the deck has no filename to write to
        TooLarge,     // the block cannot fit a 16-bit length prefix
        WriteFailed,  // the file could not be written
    };

    TapeDeck() = default;
    explicit TapeDeck(std::string path) : path_(std::move(path)) {}

    /// Reads the tape file and rewinds to the start. Returns false if the file
    /// is missing or malformed, with last_error() saying which; the deck is
    /// then empty and every load reports NoTape.
    bool insert();
    bool insert(std::string path);
    void eject();
    bool inserted() const { return inserted_; }

    void rewind() { position_ = 0; }
    size_t position() const { return position_; }
    size_t block_count() const { return tape_.size(); }
    bool at_end() const { return position_ >= tape_.size(); }

    /// Services one LD-BYTES call: `expected_flag` is the ROM's A, `dest` its
    /// IX, `length` its DE, and `verify` means it entered with carry clear.
    ///
    /// The position advances even when the block does not match. That is what
    /// turns the ROM's own retry loop -- LD-LOOK-H at 0x0767 ends in
    /// `JR NC,LD-LOOK-H` -- into "skip to the next block", which is how
    /// LOAD "" walks past files it was not asked for.
    ///
    /// Bytes are stored even when the outcome is a failure: real hardware
    /// writes each byte as it arrives and only tests the checksum at the end,
    /// so a bad load leaves partial data behind. Nothing is written when
    /// verifying.
    LoadOutcome load_block(::byte expected_flag, Memory& memory,
                           word dest, word length, bool verify);

    /// Services one SA-BYTES call: `flag` is the ROM's A, `src` its IX,
    /// `length` its DE. The block is built and the whole tape flushed to disk
    /// immediately, so an emulator killed mid-session still leaves a usable
    /// file.
    ///
    /// Recording always goes on the *end*, as it would on a cassette, so
    /// repeated SAVEs build a multi-file tape. Two consequences worth knowing:
    ///
    ///  - Saving never destroys what is already on the tape. That matters for
    ///    commercial images: a game that saves a high score would otherwise
    ///    wipe the tape it was loaded from.
    ///  - Repeated saves accumulate. Twenty saves of the same program leave
    ///    twenty copies, and a named load finds the oldest. new_tape() is the
    ///    way to start clean.
    ///
    /// The read head is deliberately left where it is, so saving and then
    /// loading back on a fresh tape needs no rewind.
    SaveResult save_block(::byte flag, Memory& memory, word src, word length);

    /// Starts a fresh, blank tape. The in-memory blocks are dropped and the
    /// head rewound, but the file on disk is left alone until the next save
    /// rewrites it -- so a mis-pressed "new tape" costs nothing and insert()
    /// brings the old contents straight back.
    void new_tape();

    const TapFile& tape() const { return tape_; }
    const std::string& path() const { return path_; }
    const std::string& last_error() const { return last_error_; }

private:
    std::string path_;
    TapFile tape_;
    size_t position_ = 0;
    bool inserted_ = false;
    std::string last_error_;
};

} // namespace z80
