/*
 * coop-fw SysEx file channel (milestone U1): read any file on the device over USB-MIDI while it runs, the way
 * kennel's development-update protocol does (third_party/kennel/synth_sdk/devupd/du_fw.c, docs/inventory.md). Disk
 * mode shows a synthesised FAT and never a yaffs file, so this is the way to get /yaffs2/user/coop_stats.txt and
 * anything else off the device without the tape-audio detour.
 *
 * Receive: the stock PlayerManager::tick -> midi_poll calls midi_rx_parse (CALL at 0x019E3CC8, timer context); the
 * package retargets that CALL at coop_sysex_rx_poll, which copies our SysEx messages (F0 00 20 76 7D cmd seq_lo seq_hi
 * <7-bit packed payload> F7) out of the USB-MIDI receive ring into a small queue and then runs the stock parser,
 * which ignores them. Service: coop_sysex_service runs from the main loop (the tape I/O step) and answers through the
 * stock usbmidi_tx_enqueue as USB-MIDI packets. Every message both ways stays at or below 96 bytes (the OP-1's USB-MIDI
 * input stops on longer SysEx: co-op's sysex.js), the host keeps one message in flight.
 *
 * Commands (cmd 0x20..; replies are cmd | 0x40, same seq, payload status + data):
 *   0x21 HELLO               -> status 0, "coop", u16 protocol (1), build tag text
 *   0x20 FILE_GET            payload: u32 offset (LE), path bytes (no NUL needed)
 *                            -> status (0 ok, 3 path, 7 io), u32 total size (LE), up to 64 bytes from offset
 * Host: tools/coop_sysex.html (Web MIDI, one file, no dependencies).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>

#define RX_RING   ((volatile uint8_t *)0x01AB0510u)                /* 1024 x 4-byte USB-MIDI packets (kennel du_fw.c) */
#define RX_WR     (*(volatile uint32_t *)0x01AB1510u)              /* free-running counters (& 0x3FF) */
#define RX_RD     (*(volatile uint32_t *)0x01AB1514u)
#define RX_PARSE  ((void (*)(void))0x019E31A4u)                    /* midi_rx_parse: the stock parser we stand in front of */
#define TX_ENQ    ((void (*)(int, const void *, unsigned))0x019E0D18u)   /* usbmidi_tx_enqueue(_, packets, count) */
#define Y_OPEN    ((int (*)(const char *, int, int))0x01A997F4u)
#define Y_CLOSE   ((int (*)(int))0x01A99908u)
#define Y_READ    ((int (*)(int, void *, int))0x01A99C90u)
#define Y_LSEEK   ((int32_t (*)(int, int32_t, int))0x01A9A0E4u)
#define Y_WRITE   ((int (*)(int, const void *, int))0x01A99F84u)
#define Y_MKDIR   ((int (*)(const char *, int))0x01A9A886u)        /* 11 stock callers: P0 = R0; LINK (kennel kn_files.c) */
#define Y_UNLINK  ((int (*)(const char *))0x01A9A3FCu)

#define MAX_MSG   128                                              /* a message we accept (the host sends <= 96) */
#define QN        4
#define CHUNK     64
#define PROTOCOL  1
#define BUILD_TAG "p4a"
enum { CX_FILE_GET = 0x20, CX_HELLO = 0x21, CX_PUT_BEGIN = 0x22, CX_PUT_DATA = 0x23, CX_PUT_END = 0x24, CX_DELETE = 0x25,
       CX_SLOT2 = 0x26, CX_REPLY = 0x40, ST_OK = 0, ST_FORMAT = 1, ST_STATE = 2, ST_PATH = 3, ST_SIZE = 4, ST_OFFSET = 5, ST_CRC = 6, ST_IO = 7 };
/* FILE_PUT (P1b): BEGIN path -> a file under /yaffs2/plugins or /yaffs2/user is created (truncated); DATA u32 offset +
 * bytes, in order, each acknowledged with the next offset; END u32 total + u32 crc32 (zlib) closes it and checks both.
 * DELETE path. The loader reads /yaffs2/plugins at boot: a new plugin plays after a power cycle. */
extern "C" int coop_slot2_set(int pos, int kind, int id, const uint16_t *knob);   /* plugin_host.cc, S2 */
extern "C" void coop_slot2_get(int pos, uint8_t *out10);
extern "C" int coop_slot2_save(void);
extern "C" int coop_plugin_list(char *out, int cap);
static int put_fd = -1;
static uint32_t put_off, put_crc;
static int path_ok(const char *p)
{
    static const char *const allowed[] = { "/yaffs2/plugins/", "/yaffs2/user/" };
    for (int a = 0; a < 2; a++) {
        const char *q = allowed[a]; int i = 0;
        while (q[i] && p[i] == q[i]) i++;
        if (!q[i] && p[i] && p[i] != '/') {
            for (int k = i; p[k]; k++) if (p[k] == '/' || (p[k] == '.' && p[k + 1] == '.')) return 0;
            return 1;
        }
    }
    return 0;
}
static uint32_t crc_update(uint32_t c, const uint8_t *d, int n)
{
    for (int i = 0; i < n; i++) { c ^= d[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
    return c;
}

volatile uint32_t g_sysex_polls, g_sysex_rx, g_sysex_drop, g_sysex_replies, g_sysex_errors;

static uint8_t q[QN][MAX_MSG];
static uint8_t qlen[QN];
static volatile uint32_t qw, qr;
static uint8_t as[MAX_MSG];
static int an = -1;                                                /* bytes assembled, -1: outside a message */
static uint32_t rx_pos;

static void rx_byte(uint8_t b)
{
    if (b == 0xF0) an = 0;
    if (an < 0) return;
    if (an >= MAX_MSG) { an = -1; return; }
    as[an++] = b;
    if (b != 0xF7) return;
    if (an > 8 && as[1] == 0 && as[2] == 0x20 && as[3] == 0x76 && as[4] == 0x7D) {
        if (qw - qr < QN) {
            uint8_t *d = q[qw % QN];
            for (int i = 0; i < an; i++) d[i] = as[i];
            qlen[qw % QN] = (uint8_t)an; qw++; g_sysex_rx++;
        } else g_sysex_drop++;
    }
    an = -1;
}

/* In place of the stock CALL midi_rx_parse (timer context): our messages out of the ring, then the stock parser. */
extern "C" void coop_sysex_rx_poll(void)
{
    g_sysex_polls++;
    uint32_t wr = RX_WR;
    if (wr - rx_pos > 1024) rx_pos = RX_RD;                        /* first call / lapped */
    for (; rx_pos != wr; rx_pos++) {
        volatile uint8_t *p = RX_RING + (rx_pos & 0x3FF) * 4;
        int cin = p[0] & 15, k = cin == 5 ? 1 : cin == 6 ? 2 : (cin == 4 || cin == 7) ? 3 : 0;
        for (int i = 0; i < k; i++) rx_byte(p[1 + i]);
    }
    RX_PARSE();
}

static int unpack7(const uint8_t *in, int n, uint8_t *out)
{
    int o = 0;
    for (int i = 0; i < n;) {
        uint8_t hi = in[i++];
        if (hi & 0x80) return -1;
        for (int k = 0; k < 7 && i < n; k++, i++) {
            if (in[i] & 0x80) return -1;
            out[o++] = (uint8_t)(in[i] | ((hi >> k & 1) << 7));
        }
    }
    return o;
}

static int pack7(const uint8_t *in, int n, uint8_t *out)
{
    int o = 0;
    for (int i = 0; i < n; i += 7) {
        int m = n - i < 7 ? n - i : 7;
        uint8_t hi = 0;
        for (int k = 0; k < m; k++) hi |= (uint8_t)((in[i + k] >> 7) << k);
        out[o++] = hi;
        for (int k = 0; k < m; k++) out[o++] = in[i + k] & 0x7F;
    }
    return o;
}

static void send_sysex(const uint8_t *m, int n)
{
    static uint32_t pk[MAX_MSG / 3 + 2];
    int np = 0;
    for (int i = 0; i < n; i += 3) {
        int k = n - i < 3 ? n - i : 3, cin = n - i > 3 ? 4 : 4 + k;   /* 4: continues, 5/6/7: ends with 1/2/3 */
        uint32_t w = (uint32_t)cin;
        for (int j = 0; j < k; j++) w |= (uint32_t)m[i + j] << (8 * (j + 1));
        pk[np++] = w;
    }
    TX_ENQ(0, pk, (unsigned)np);
}

static void reply(int cmd, int seq, const uint8_t *payload, int n)
{
    static uint8_t msg[MAX_MSG];
    int o = 0;
    msg[o++] = 0xF0; msg[o++] = 0x00; msg[o++] = 0x20; msg[o++] = 0x76; msg[o++] = 0x7D;
    msg[o++] = (uint8_t)((cmd | CX_REPLY) & 0x7F);
    msg[o++] = (uint8_t)(seq & 0x7F);
    msg[o++] = (uint8_t)((seq >> 7) & 0x7F);
    o += pack7(payload, n, msg + o);
    msg[o++] = 0xF7;
    send_sysex(msg, o);
    g_sysex_replies++;
}

static void handle(const uint8_t *m, int n)
{
    static uint8_t in[MAX_MSG], out[8 + CHUNK];
    int cmd = m[5], seq = m[6] | (m[7] << 7);
    int pn = unpack7(m + 8, n - 9, in);
    if (pn < 0) { out[0] = ST_FORMAT; reply(cmd, seq, out, 1); g_sysex_errors++; return; }
    if (cmd == CX_HELLO) {
        const char *tag = "coop" BUILD_TAG;
        int o = 0;
        out[o++] = ST_OK; out[o++] = 'c'; out[o++] = 'o'; out[o++] = 'o'; out[o++] = 'p';
        out[o++] = PROTOCOL & 0xFF; out[o++] = PROTOCOL >> 8;
        for (int i = 4; tag[i] && o < (int)sizeof out; i++) out[o++] = (uint8_t)tag[i];
        reply(cmd, seq, out, o);
        return;
    }
    if (cmd == CX_FILE_GET) {
        static char path[96];
        if (pn < 5 || pn - 4 >= (int)sizeof path) { out[0] = ST_FORMAT; reply(cmd, seq, out, 1); g_sysex_errors++; return; }
        uint32_t off = (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
        int pl = pn - 4;
        for (int i = 0; i < pl; i++) path[i] = (char)in[4 + i];
        path[pl] = 0;
        int fd = Y_OPEN(path, 0, 0);
        if (fd < 0) { out[0] = ST_PATH; reply(cmd, seq, out, 1); g_sysex_errors++; return; }
        int32_t total = Y_LSEEK(fd, 0, 2);
        int got = 0, ok = total >= 0;
        if (ok && (int32_t)off < total) {
            ok = Y_LSEEK(fd, (int32_t)off, 0) == (int32_t)off;
            if (ok) { got = Y_READ(fd, out + 5, CHUNK); if (got < 0) { got = 0; ok = 0; } }
        }
        Y_CLOSE(fd);
        out[0] = ok ? ST_OK : ST_IO;
        uint32_t t = ok ? (uint32_t)total : 0;
        out[1] = (uint8_t)t; out[2] = (uint8_t)(t >> 8); out[3] = (uint8_t)(t >> 16); out[4] = (uint8_t)(t >> 24);
        if (!ok) g_sysex_errors++;
        reply(cmd, seq, out, 5 + got);
        return;
    }
    if (cmd == CX_PUT_BEGIN || cmd == CX_DELETE) {
        static char path[96];
        if (pn < 1 || pn >= (int)sizeof path) { out[0] = ST_FORMAT; reply(cmd, seq, out, 1); g_sysex_errors++; return; }
        for (int i = 0; i < pn; i++) path[i] = (char)in[i];
        path[pn] = 0;
        if (!path_ok(path)) { out[0] = ST_PATH; reply(cmd, seq, out, 1); g_sysex_errors++; return; }
        if (cmd == CX_DELETE) { out[0] = Y_UNLINK(path) == 0 ? ST_OK : ST_PATH; reply(cmd, seq, out, 1); return; }
        if (put_fd >= 0) { Y_CLOSE(put_fd); put_fd = -1; }
        Y_MKDIR("/yaffs2/plugins", 0x1FF);                  /* exists already: ignored */
        put_fd = Y_OPEN(path, 0x242, 0x180);                  /* O_RDWR | O_CREAT | O_TRUNC */
        put_off = 0; put_crc = 0xFFFFFFFFu;
        out[0] = put_fd >= 0 ? ST_OK : ST_IO; reply(cmd, seq, out, 1);
        return;
    }
    if (cmd == CX_PUT_DATA) {
        if (put_fd < 0) { out[0] = ST_STATE; reply(cmd, seq, out, 1); return; }
        if (pn < 4) { out[0] = ST_FORMAT; reply(cmd, seq, out, 1); return; }
        uint32_t off = (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
        int n = pn - 4;
        if (off != put_off) { out[0] = ST_OFFSET; }
        else if (n && Y_WRITE(put_fd, in + 4, n) != n) { out[0] = ST_IO; Y_CLOSE(put_fd); put_fd = -1; }
        else { put_crc = crc_update(put_crc, in + 4, n); put_off += (uint32_t)n; out[0] = ST_OK; }
        out[1] = (uint8_t)put_off; out[2] = (uint8_t)(put_off >> 8); out[3] = (uint8_t)(put_off >> 16); out[4] = (uint8_t)(put_off >> 24);
        reply(cmd, seq, out, 5);
        return;
    }
    if (cmd == CX_PUT_END) {
        if (put_fd < 0) { out[0] = ST_STATE; reply(cmd, seq, out, 1); return; }
        if (pn < 8) { out[0] = ST_FORMAT; reply(cmd, seq, out, 1); return; }
        uint32_t total = (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
        uint32_t crc = (uint32_t)in[4] | ((uint32_t)in[5] << 8) | ((uint32_t)in[6] << 16) | ((uint32_t)in[7] << 24);
        int closed = Y_CLOSE(put_fd) == 0; put_fd = -1;
        out[0] = !closed ? ST_IO : total != put_off ? ST_SIZE : (~put_crc) != crc ? ST_CRC : ST_OK;
        reply(cmd, seq, out, 1);
        return;
    }
    if (cmd == CX_SLOT2) {
        /* S2 (plugin_host.cc): op 0 = get -> ST_OK + 10 bytes per slot (kind, id, 4 x u16 knob, Q15);
         * op 1 = set: pos (0 instrument, 1 master), kind (0 none, 1 stock type, 2 plugin), id, 4 x u16 knob -> saved
         * to /yaffs2/user/coop_slot2.bin, then the get reply; op 2 = the loaded plugins as "n=name;" text. */
        if (pn < 1) { out[0] = ST_FORMAT; reply(cmd, seq, out, 1); g_sysex_errors++; return; }
        if (in[0] == 2) { out[0] = ST_OK; int l = coop_plugin_list((char *)out + 1, (int)sizeof out - 1); reply(cmd, seq, out, 1 + l); return; }
        if (in[0] == 1) {
            if (pn < 12) { out[0] = ST_FORMAT; reply(cmd, seq, out, 1); g_sysex_errors++; return; }
            uint16_t k[4];
            for (int i = 0; i < 4; i++) k[i] = (uint16_t)(in[4 + 2 * i] | (in[5 + 2 * i] << 8));
            if (!coop_slot2_set(in[1], in[2], in[3], k)) { out[0] = ST_STATE; reply(cmd, seq, out, 1); g_sysex_errors++; return; }
            if (!coop_slot2_save()) { out[0] = ST_IO; reply(cmd, seq, out, 1); g_sysex_errors++; return; }
        }
        out[0] = ST_OK; coop_slot2_get(0, out + 1); coop_slot2_get(1, out + 11);
        reply(cmd, seq, out, 21);
        return;
    }
    out[0] = ST_FORMAT; reply(cmd, seq, out, 1); g_sysex_errors++;
}

/* From the main loop (coop_tape_io_step, mode 0) on every pass. */
extern "C" void coop_sysex_service(void)
{
    while (qr != qw) {
        handle(q[qr % QN], qlen[qr % QN]);
        qr++;
    }
}
