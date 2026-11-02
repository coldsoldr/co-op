// Host test of the coop tape image metadata writer (core/sdram/tape_io.cc, COOP_HOST): the FAT16 layout a
// stereo-track image gets on the first boot. Captures the writes and checks chains, directories, headers and
// the data offsets the firmware computes from constants.
#define COOP_HOST 1
#include "../../core/sdram/tape_io.cc"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

static std::map<int32_t, std::vector<uint8_t>> g_writes;

static int capture(void *, int32_t off, const uint8_t *buf, int32_t len)
{
    g_writes[off] = std::vector<uint8_t>(buf, buf + len);
    return 0;
}

static uint8_t byte_at(int32_t off)
{
    for (auto &w : g_writes)
        if (w.first <= off && off < w.first + (int32_t)w.second.size()) return w.second[off - w.first];
    return 0;
}
static uint16_t le16(int32_t off) { return (uint16_t)(byte_at(off) | byte_at(off + 1) << 8); }
static uint32_t le32(int32_t off) { return le16(off) | (uint32_t)le16(off + 2) << 16; }
static uint32_t be32v(int32_t off) { return (uint32_t)byte_at(off) << 24 | byte_at(off + 1) << 16 | byte_at(off + 2) << 8 | byte_at(off + 3); }
static uint16_t fat(int cl) { return le16(FAT1 + cl * 2); }

#define CHECK(c) do { if (!(c)) { std::printf("tape_image_test: FAILED: %s (line %d)\n", #c, __LINE__); std::exit(1); } } while (0)

int main()
{
    static uint8_t buf[CLUSTER];
    CHECK(coop_tape_image_meta(capture, nullptr, buf) == 0);
    // layout constants
    CHECK(TRACK_BYTES == 63570032 && TRACK_CL == 7761);                       // 0x70 + 0xF28000 frames x 4 B
    CHECK(LAST_CL == 31046 && LAST_CL < 0x18000 / 2);                         // fits the 192-sector FAT
    CHECK(CL_OFF(LAST_CL + 1) <= IMG_SIZE);                                     // the data area ends inside the image
    // FAT: entries 0/1 zero as stock leaves them, TAPE dir a one-cluster chain, each track a contiguous chain
    CHECK(fat(0) == 0 && fat(1) == 0 && fat(TAPE_DIR) == 0xFFFF);
    for (int t = 0; t < TRACKS; t++) {
        int first = TRACK1 + t * TRACK_CL;
        for (int c = first; c < first + TRACK_CL - 1; c++) CHECK(fat(c) == c + 1);
        CHECK(fat(first + TRACK_CL - 1) == 0xFFFF);
    }
    CHECK(fat(LAST_CL + 1) == 0);
    // both FAT copies identical over the used part
    for (int32_t i = 0; i < (LAST_CL + 1) * 2; i += 2) CHECK(le16(FAT1 + i) == le16(FAT2 + i));
    // root: TAPE only
    CHECK(std::memcmp(&g_writes[ROOT][0], "TAPE       ", 11) == 0 && byte_at(ROOT + 11) == 0x10 && le16(ROOT + 26) == TAPE_DIR);
    CHECK(byte_at(ROOT + 32) == 0);
    // TAPE directory: ., .., TRACK_1..4 AIF with the stereo size and the chain starts
    int32_t dir = CL_OFF(TAPE_DIR);
    for (int t = 0; t < TRACKS; t++) {
        int32_t e = dir + 64 + 32 * t;
        char name[12] = "TRACK_1 AIF"; name[6] = (char)('1' + t);
        CHECK(std::memcmp(&g_writes[dir][64 + 32 * t], name, 11) == 0);
        CHECK(byte_at(e + 11) == 0x20 && le16(e + 26) == TRACK1 + t * TRACK_CL && le32(e + 28) == (uint32_t)TRACK_BYTES);
    }
    CHECK(byte_at(dir + 64 + 32 * TRACKS) == 0);
    // AIFC headers: stereo, 0xF28000 frames, sowt, SSND size
    for (int t = 0; t < TRACKS; t++) {
        int32_t h = CL_OFF(TRACK1 + t * TRACK_CL);
        CHECK(std::memcmp(&g_writes[h][0], "FORM", 4) == 0 && byte_at(h + 0x21) == 2 && be32v(h + 0x22) == TRACK_FRAMES);
        CHECK(std::memcmp(&g_writes[h][0x32], "sowt", 4) == 0 && be32v(h + 0x64) == TRACK_FRAMES * 4u + 8u);
        CHECK(be32v(h + 4) == AIFC_HDR + TRACK_FRAMES * 4u - 8u);
        // the frames start right after the 0x70-byte header, 4-byte aligned, and 485 chunks fit before the next track
        int32_t data = track_data_off(t);
        CHECK(data == h + AIFC_HDR && data % 4 == 0);
        CHECK(data + (int32_t)CHUNKS * FRAMES_PER_CHUNK * 4 == h + (int32_t)TRACK_BYTES);
        if (t + 1 < TRACKS) CHECK(h + (int32_t)TRACK_BYTES <= CL_OFF(TRACK1 + (t + 1) * TRACK_CL));
    }
    // nothing written beyond the headers (sparse image), and the last track ends inside the image
    for (auto &w : g_writes) CHECK(w.first + (int32_t)w.second.size() <= IMG_SIZE);
    CHECK(track_data_off(TRACKS - 1) + (int32_t)TRACK_FRAMES * 4 <= IMG_SIZE);
    std::printf("tape_image_test: OK (%zu writes, tracks at %#x %#x %#x %#x)\n", g_writes.size(),
                track_data_off(0), track_data_off(1), track_data_off(2), track_data_off(3));
    return 0;
}
