// HALL: an 8-line FDN hall reverb in fixed point. See hall.h and
// te-op1-cfw docs/hall-port.md.
//
// coop port (plugins/cs_hall): te-op1-cfw ports/fx/src/hall/hall.cc with
// its compile-time double maths replaced by the integer literals it
// evaluated to (guide/plugin-guidelines.md section 2.1: no float or double in a
// coop plugin's files, not even constexpr). The literals were printed by
// the upstream build itself (g++ -std=c++17, the same constexpr code), and
// the coop host test (tests/host/cs_hall) checks this file against the
// upstream one bit for bit. The C run kernels are compiled for the host
// only; the Blackfin build runs hall_bfin.S (upstream checks asm = C in
// its simulator test, ports/fx/tests/test_hall.cc).

#include "hall.h"

#include <cstddef>

namespace op1fx {

namespace {

constexpr bool IsPrime(int32_t n) {
  if (n < 2) return false;
  for (int32_t d = 2; d * d <= n; ++d) {
    if (n % d == 0) return false;
  }
  return true;
}

constexpr int32_t kEarlyL[5] = { 179, 487, 857, 1291, 1777 };
constexpr int32_t kEarlyR[5] = { 263, 641, 1031, 1499, 1993 };

static_assert(IsPrime(HL_L0_LEN) && IsPrime(HL_L1_LEN) && IsPrime(HL_L2_LEN) &&
              IsPrime(HL_L3_LEN) && IsPrime(HL_L4_LEN) && IsPrime(HL_L5_LEN) &&
              IsPrime(HL_L6_LEN) && IsPrime(HL_L7_LEN), "line lengths");
static_assert(HL_AP2 == HL_AP1 + HL_AP1_LEN + 1 && HL_AP3 == HL_AP2 + HL_AP2_LEN + 1 &&
              HL_ER == HL_AP3 + HL_AP3_LEN + 1, "diffuser bases");
static_assert(kEarlyR[4] == HL_ER_REACH && kEarlyL[4] < HL_ER_REACH, "early reach");
static_assert(HL_L0 == HL_ER + HL_ER_REACH + 1 &&
              HL_L1 == HL_L0 + HL_L0_LEN + HL_EXC_MAX + 1 &&
              HL_L2 == HL_L1 + HL_L1_LEN + HL_EXC_MAX + 1 &&
              HL_L3 == HL_L2 + HL_L2_LEN + HL_EXC_MAX + 1 &&
              HL_L4 == HL_L3 + HL_L3_LEN + HL_EXC_MAX + 1 &&
              HL_L5 == HL_L4 + HL_L4_LEN + HL_EXC_MAX + 1 &&
              HL_L6 == HL_L5 + HL_L5_LEN + HL_EXC_MAX + 1 &&
              HL_L7 == HL_L6 + HL_L6_LEN + HL_EXC_MAX + 1, "line bases");
static_assert(HL_MAX_LINE >= HL_ER_REACH && HL_MAX_LINE >= HL_AP3_LEN, "the longest line is last");
static_assert(2 * HL_REACH <= 65534, "immediate offsets");

}  // namespace

// Size step s: the lines at NearestPrime(round(len_max x 0.2 x 5^(s / 15))),
// the early taps at round(tap_max x the same scale) (upstream's constexpr,
// evaluated).
constexpr HallSizeTable kHallSizesC[kHallNumSizes] = {
  { { 229, 271, 317, 367, 409, 479, 541, 617 }, { 36, 97, 171, 258, 355 }, { 53, 128, 206, 300, 399 } },   // size 0
  { { 257, 307, 353, 409, 457, 523, 601, 691 }, { 40, 108, 191, 287, 396 }, { 59, 143, 230, 334, 444 } },   // size 1
  { { 283, 337, 397, 449, 509, 587, 673, 769 }, { 44, 121, 212, 320, 440 }, { 65, 159, 256, 372, 494 } },   // size 2
  { { 317, 373, 439, 503, 569, 659, 751, 853 }, { 49, 134, 236, 356, 490 }, { 73, 177, 285, 414, 550 } },   // size 3
  { { 353, 419, 491, 557, 631, 733, 829, 947 }, { 55, 150, 263, 397, 546 }, { 81, 197, 317, 460, 612 } },   // size 4
  { { 397, 463, 547, 619, 709, 811, 929, 1051 }, { 61, 167, 293, 442, 608 }, { 90, 219, 353, 513, 682 } },   // size 5
  { { 439, 521, 607, 691, 787, 907, 1031, 1171 }, { 68, 185, 326, 492, 677 }, { 100, 244, 393, 571, 759 } },   // size 6
  { { 487, 577, 677, 773, 877, 1009, 1151, 1307 }, { 76, 206, 363, 547, 753 }, { 111, 272, 437, 635, 845 } },   // size 7
  { { 541, 641, 751, 859, 971, 1123, 1279, 1459 }, { 84, 230, 404, 609, 838 }, { 124, 302, 486, 707, 940 } },   // size 8
  { { 607, 719, 839, 953, 1087, 1249, 1423, 1621 }, { 94, 256, 450, 678, 933 }, { 138, 337, 542, 787, 1047 } },   // size 9
  { { 673, 797, 937, 1063, 1201, 1399, 1583, 1801 }, { 105, 285, 501, 755, 1039 }, { 154, 375, 603, 877, 1166 } },   // size 10
  { { 751, 887, 1039, 1187, 1327, 1549, 1759, 2011 }, { 117, 317, 558, 840, 1157 }, { 171, 417, 671, 976, 1298 } },   // size 11
  { { 839, 983, 1153, 1321, 1493, 1723, 1973, 2239 }, { 130, 353, 621, 936, 1288 }, { 191, 465, 747, 1086, 1444 } },   // size 12
  { { 929, 1097, 1289, 1471, 1663, 1913, 2179, 2503 }, { 144, 393, 691, 1042, 1434 }, { 212, 517, 832, 1209, 1608 } },   // size 13
  { { 1033, 1223, 1433, 1637, 1847, 2137, 2437, 2777 }, { 161, 437, 770, 1160, 1596 }, { 236, 576, 926, 1346, 1790 } },   // size 14
  { { 1153, 1361, 1597, 1823, 2063, 2381, 2711, 3089 }, { 179, 487, 857, 1291, 1777 }, { 263, 641, 1031, 1499, 1993 } },   // size 15
};
const HallSizeTable kHallSizes[kHallNumSizes] = {
  kHallSizesC[0],  kHallSizesC[1],  kHallSizesC[2],  kHallSizesC[3],
  kHallSizesC[4],  kHallSizesC[5],  kHallSizesC[6],  kHallSizesC[7],
  kHallSizesC[8],  kHallSizesC[9],  kHallSizesC[10], kHallSizesC[11],
  kHallSizesC[12], kHallSizesC[13], kHallSizesC[14], kHallSizesC[15],
};
static_assert(kHallSizesC[15].len[7] == HL_L7_LEN && kHallSizesC[15].len[0] == HL_L0_LEN &&
              kHallSizesC[15].len[6] == HL_L6_LEN && kHallSizesC[15].len[5] == HL_L5_LEN &&
              kHallSizesC[15].len[4] == HL_L4_LEN && kHallSizesC[15].len[3] == HL_L3_LEN &&
              kHallSizesC[15].len[2] == HL_L2_LEN && kHallSizesC[15].len[1] == HL_L1_LEN &&
              kHallSizesC[15].er_r[4] == HL_ER_REACH && kHallSizesC[15].er_l[4] == kEarlyL[4],
              "the layout is the largest size");
// The shortest line, modulated, stays well clear of its write.
static_assert(kHallSizesC[0].len[0] - HL_EXC_MAX > 2 * kHallMaxBlock, "modulation depth");
static_assert(kHallSizesC[0].er_l[0] > 2, "early taps");

// 0.85, -0.70, 0.58, -0.47, 0.38.
const int16_t kHallEarlyCoef[5] = { 27853, -22938, 19005, -15401, 12452 };
const int8_t kHallSlotLine[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };
const int8_t kHallSlotSign[8] = { 1, 1, 1, -1, -1, 1, -1, -1 };

namespace {

const int32_t kLineBase[8] = { HL_L0, HL_L1, HL_L2, HL_L3, HL_L4, HL_L5, HL_L6, HL_L7 };

// --- Parameter tables (upstream's constexpr, evaluated) ---

struct Table33 {
  int32_t v[33];
};
struct Table257 {
  int32_t v[257];
};

// T = 3 log2(10) / (fs RT60), Q24 (the exponent of 2 lost per sample of
// delay), at decay = 1024 i: RT60 = 0.3 x (20 / 0.3)^(decay / 32512).
// (ratio - 1), Q12, ratio = 10^(damp / 32767) at damp = 1024 i: the HF
// (Nyquist) decays up to 10 x faster than DC.
// 2^(-i / 256), Q30.
// The damping pole a(u) = (1 - 2^-u) / (1 + 2^-u), Q15, u = i / 16.
// The size crossfade's weight after 8 i samples (of 256), Q31 (kOne at the
// end): (1 - cos(pi i / 32)) / 2, a raised cosine.
constexpr Table33 kDecayTable = {{
    25276, 22144, 19400, 16997, 14891, 13046, 11429, 10013,
    8773, 7686, 6734, 5899, 5168, 4528, 3967, 3475,
    3045, 2668, 2337, 2048, 1794, 1572, 1377, 1206,
    1057, 926, 811, 711, 623, 545, 478, 419,
    367
}};
constexpr Table33 kRatioTable = {{
    0, 306, 634, 987, 1366, 1774, 2212, 2682,
    3188, 3731, 4315, 4943, 5617, 6342, 7121, 7957,
    8857, 9823, 10862, 11977, 13177, 14465, 15850, 17338,
    18938, 20656, 22503, 24487, 26620, 28911, 31374, 34020,
    36864
}};
constexpr Table257 kExp2Table = {{
    1073741824, 1070838486, 1067942999, 1065055341, 1062175491, 1059303428, 1056439131, 1053582579,
    1050733751, 1047892626, 1045059183, 1042233401, 1039415261, 1036604740, 1033801819, 1031006477,
    1028218693, 1025438448, 1022665720, 1019900489, 1017142735, 1014392438, 1011649578, 1008914134,
    1006186087, 1003465416, 1000752102, 998046124, 995347464, 992656100, 989972014, 987295185,
    984625594, 981963222, 979308048, 976660054, 974019220, 971385527, 968758955, 966139485,
    963527098, 960921775, 958323496, 955732243, 953147997, 950570738, 948000448, 945437108,
    942880699, 940331203, 937788600, 935252872, 932724001, 930201967, 927686753, 925178340,
    922676710, 920181844, 917693724, 915212331, 912737649, 910269657, 907808339, 905353676,
    902905651, 900464244, 898029440, 895601218, 893179563, 890764456, 888355878, 885953814,
    883558244, 881169153, 878786521, 876410331, 874040567, 871677210, 869320244, 866969651,
    864625413, 862287515, 859955938, 857630665, 855311680, 852998965, 850692504, 848392279,
    846098274, 843810471, 841528855, 839253408, 836984114, 834720956, 832463917, 830212982,
    827968132, 825729353, 823496627, 821269938, 819049271, 816834607, 814625932, 812423229,
    810226483, 808035676, 805850792, 803671817, 801498734, 799331526, 797170178, 795014675,
    792865000, 790721137, 788583072, 786450787, 784324269, 782203500, 780088465, 777979150,
    775875538, 773777614, 771685363, 769598769, 767517817, 765442492, 763372778, 761308661,
    759250125, 757197155, 755149737, 753107854, 751071493, 749040637, 747015274, 744995386,
    742980960, 740971982, 738968435, 736970306, 734977579, 732990241, 731008277, 729031671,
    727060411, 725094480, 723133865, 721178552, 719228525, 717283772, 715344277, 713410026,
    711481005, 709557200, 707638598, 705725183, 703816941, 701913860, 700015924, 698123120,
    696235434, 694352853, 692475362, 690602947, 688735596, 686873293, 685016026, 683163781,
    681316545, 679474303, 677637043, 675804750, 673977412, 672155015, 670337545, 668524990,
    666717336, 664914570, 663116678, 661323648, 659535466, 657752119, 655973594, 654199878,
    652430958, 650666822, 648907455, 647152846, 645402981, 643657847, 641917433, 640181724,
    638450708, 636724373, 635002706, 633285695, 631573326, 629865587, 628162466, 626463950,
    624770026, 623080683, 621395908, 619715688, 618040012, 616368866, 614702239, 613040119,
    611382493, 609729349, 608080675, 606436459, 604796689, 603161352, 601530438, 599903933,
    598281827, 596664106, 595050760, 593441776, 591837143, 590236848, 588640881, 587049229,
    585461881, 583878825, 582300049, 580725543, 579155293, 577589290, 576027521, 574469975,
    572916640, 571367506, 569822560, 568281792, 566745190, 565212742, 563684439, 562160268,
    560640218, 559124278, 557612438, 556104685, 554601009, 553101399, 551605844, 550114332,
    548626854, 547143398, 545663953, 544188508, 542717053, 541249576, 539786068, 538326517,
    536870912
}};
constexpr Table257 kPoleTable = {{
    0, 710, 1419, 2126, 2832, 3535, 4235, 4931,
    5622, 6308, 6989, 7663, 8330, 8991, 9643, 10287,
    10922, 11549, 12165, 12772, 13369, 13955, 14531, 15096,
    15649, 16191, 16722, 17241, 17749, 18244, 18728, 19200,
    19660, 20109, 20545, 20970, 21383, 21785, 22175, 22554,
    22922, 23279, 23625, 23961, 24286, 24601, 24905, 25200,
    25485, 25761, 26028, 26285, 26534, 26774, 27006, 27229,
    27445, 27653, 27854, 28047, 28233, 28413, 28585, 28752,
    28912, 29066, 29215, 29357, 29495, 29627, 29754, 29876,
    29993, 30106, 30215, 30319, 30419, 30515, 30607, 30696,
    30781, 30863, 30941, 31017, 31089, 31158, 31225, 31289,
    31350, 31409, 31465, 31520, 31572, 31621, 31669, 31715,
    31759, 31801, 31841, 31880, 31917, 31953, 31987, 32020,
    32051, 32081, 32110, 32137, 32164, 32189, 32213, 32237,
    32259, 32280, 32301, 32320, 32339, 32357, 32375, 32391,
    32407, 32422, 32437, 32451, 32464, 32477, 32489, 32501,
    32512, 32523, 32533, 32543, 32552, 32562, 32570, 32579,
    32586, 32594, 32601, 32608, 32615, 32622, 32628, 32634,
    32639, 32645, 32650, 32655, 32660, 32664, 32668, 32673,
    32677, 32680, 32684, 32688, 32691, 32694, 32697, 32700,
    32703, 32706, 32708, 32711, 32713, 32716, 32718, 32720,
    32722, 32724, 32726, 32727, 32729, 32731, 32732, 32734,
    32735, 32736, 32738, 32739, 32740, 32741, 32742, 32743,
    32744, 32745, 32746, 32747, 32748, 32749, 32750, 32750,
    32751, 32752, 32752, 32753, 32754, 32754, 32755, 32755,
    32756, 32756, 32757, 32757, 32757, 32758, 32758, 32759,
    32759, 32759, 32760, 32760, 32760, 32761, 32761, 32761,
    32761, 32762, 32762, 32762, 32762, 32762, 32763, 32763,
    32763, 32763, 32763, 32763, 32764, 32764, 32764, 32764,
    32764, 32764, 32764, 32765, 32765, 32765, 32765, 32765,
    32765, 32765, 32765, 32765, 32765, 32765, 32765, 32766,
    32766, 32766, 32766, 32766, 32766, 32766, 32766, 32766,
    32766
}};
constexpr Table33 kXfadeCurve = {{
    0, 5170203, 20631018, 46233551, 81731235, 126782208, 180952603, 243720732,
    314482101, 392555242, 477188266, 567566111, 662818389, 762027769, 864238811, 968467165,
    1073709056, 1178950947, 1283179301, 1385390343, 1484599723, 1579852001, 1670229846, 1754862870,
    1832936011, 1903697380, 1966465509, 2020635904, 2065686877, 2101184561, 2126787094, 2142247909,
    2147418112
}};

inline int32_t Lookup33(const Table33& t, int32_t x) {
  const int32_t i = x >> 10;
  const int32_t f = x & 1023;
  return t.v[i] + static_cast<int32_t>((static_cast<int64_t>(t.v[i + 1] - t.v[i]) * f) >> 10);
}

inline int32_t Clamp(int32_t x, int32_t lo, int32_t hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}

// 2^(-e), e Q24 >= 0 -> Q15 (<= 32767).
inline int32_t Exp2Neg(int32_t e) {
  const int32_t ip = e >> 24;
  if (ip >= 30) return 0;
  const int32_t i = (e >> 16) & 255;
  const int32_t f = e & 0xffff;
  const int32_t v = kExp2Table.v[i] +
                    static_cast<int32_t>((static_cast<int64_t>(kExp2Table.v[i + 1] - kExp2Table.v[i]) * f) >> 16);
  const int32_t g = ((v >> ip) + (1 << 14)) >> 15;
  return g > 32767 ? 32767 : g;
}

// a(u), u Q24 >= 0.
inline int32_t Pole(int64_t u) {
  if (u >= (static_cast<int64_t>(256) << 20)) return kPoleTable.v[256];
  const int32_t uu = static_cast<int32_t>(u);
  const int32_t i = uu >> 20;
  const int32_t f = (uu >> 4) & 0xffff;
  return kPoleTable.v[i] + (((kPoleTable.v[i + 1] - kPoleTable.v[i]) * f) >> 16);
}

// 65536 / n for even n (n = 2 j).
constexpr int32_t kRecip[kHallMaxBlock / 2 + 1] = {
  0, 32768, 16384, 10923, 8192, 6554, 5461, 4681, 4096, 3641, 3277, 2979, 2731, 2521, 2341,
  2185, 2048, 1928, 1820, 1725, 1638, 1560, 1489, 1425, 1365, 1311, 1260, 1214, 1170, 1130,
  1092, 1057, 1024,
};
constexpr int8_t kShift[kHallMaxBlock / 2 + 1] = {
  -1, 1, 2, -1, 3, -1, -1, -1, 4, -1, -1, -1, -1, -1, -1, -1, 5,
  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 6,
};

// delta / n, per sample (floored; the caller tracks start + step n).
inline int32_t Ramp(int32_t delta, int32_t n) {
  const int32_t shift = kShift[n >> 1];
  if (shift >= 0) return delta >> shift;
  return static_cast<int32_t>((static_cast<int64_t>(delta) * kRecip[n >> 1]) >> 16);
}

// Parabolic sine of a phase, Q15.
inline int32_t Sine(uint32_t phase) {
  const int32_t x = static_cast<int32_t>(phase) >> 16;
  const int32_t a = x < 0 ? -x : x;
  const int32_t y = (x * (32768 - a)) >> 13;
  return Clamp(y, -32767, 32767);
}

// Smoothing per call (Q15 << 8): an eighth of the way each call (~12 ms).
inline int32_t Smooth(int32_t s, int32_t target_q15) {
  const int32_t t = target_q15 << 8;
  const int32_t d = t - s;
  if (d < 2048 && d > -2048) return t;
  return s + (d >> 3);
}

const uint32_t kLfoIncA = 118818;                 // 0.61 Hz at 22050 Hz
const uint32_t kLfoIncB = 169461;                 // 0.87 Hz
const int32_t kInputGain = 0x20000000;            // 0.25, Q31
const int32_t kLateGain = 22938;                  // 0.7 (the lines' sum, x 2 at the output)
const int32_t kDiffusion = 19661;                 // 0.6
const int32_t kPreSlew = 16384;                   // pre-delay glide: <= 1/4 sample per sample

// --- The Blackfin's 16-bit MAC arithmetic ---

// A fractional product into an accumulator: 2 a b, exactly. (-1 x -1 is
// +2^31: the 40-bit accumulator holds it; only a product written straight
// to a 32-bit register saturates, and the kernels have none. Measured in
// bfin-elf-run.)
inline int64_t Fr(int32_t a, int32_t b) {
  return static_cast<int64_t>(a * b) * 2;
}
inline int32_t Sat16(int64_t x) {
  return static_cast<int32_t>(x > 32767 ? 32767 : (x < -32768 ? -32768 : x));
}
// The accumulator's high half, rounded as the MAC does by default
// (unbiased: ties to even), saturated.
inline int32_t Rnd(int64_t acc) {
  int64_t q = acc >> 16;
  const int32_t r = static_cast<int32_t>(acc & 0xffff);
  if (r > 0x8000 || (r == 0x8000 && (q & 1))) ++q;
  return Sat16(q);
}
inline int32_t Lo(int32_t x) { return static_cast<int16_t>(x & 0xffff); }
inline int32_t Hi(int32_t x) { return static_cast<int16_t>(static_cast<uint32_t>(x) >> 16); }
inline int32_t Pack(int32_t lo, int32_t hi) {
  return (lo & 0xffff) | static_cast<int32_t>(static_cast<uint32_t>(hi) << 16);
}

// --- The run kernels (C references; hall_bfin.S matches them) ---
// coop: host builds only (the host test); the device runs hall_bfin.S.
#if !defined(__bfin__)

#define WRITE(index, value)                           \
  {                                                   \
    p[index] = static_cast<int16_t>(value);           \
    p[(index) + alias] = static_cast<int16_t>(value); \
  }
#define ALLPASS(base, length, rs, ws)                         \
  {                                                           \
    const int32_t previous = p[(base) + (length)];            \
    acc += Fr(previous, rs);                                  \
    const int32_t w = Rnd(acc);                               \
    WRITE(base, w)                                            \
    acc = (static_cast<int64_t>(previous) << 16) + Fr(w, ws); \
  }

enum Mode { kStereo, kMono, kXfade };

template <Mode mode>
void Run(HallKernel* k, const int16_t* x, int16_t* l, int16_t* r, int16_t* p, int32_t n,
         int32_t alias_bytes) {
  const int32_t alias = alias_bytes / 2;
  int32_t gain = k->gain;
  const int32_t gstep = k->gstep;
  const int32_t mg = Lo(k->dg), g = Hi(k->dg);
  int32_t pos[2] = { k->posa, k->posb };
  const int32_t step[2] = { k->stepa, k->stepb };
  int32_t xf = k->xf;
  int32_t o[8], xo[8], eo[10], xeo[10], ca[8], cb[8], z[8], base[8], sign[8];
  for (int32_t s = 0; s < 8; ++s) {
    o[s] = k->off[s] / 2;
    xo[s] = k->xoff[s] / 2;
    const int32_t i = s >> 1;
    ca[s] = (s & 1) ? Hi(k->ca[i]) : Lo(k->ca[i]);
    cb[s] = (s & 1) ? Hi(k->cb[i]) : Lo(k->cb[i]);
    z[s] = (s & 1) ? Hi(k->z[i]) : Lo(k->z[i]);
    base[s] = kLineBase[kHallSlotLine[s]];
    sign[s] = kHallSlotSign[s];
  }
  for (int32_t j = 0; j < 10; ++j) {
    eo[j] = k->eroff[j] / 2;
    xeo[j] = k->xeroff[j] / 2;
  }
  const int32_t c[5] = { Lo(k->c01), Hi(k->c01), Lo(k->c23), Hi(k->c23), Hi(k->glc4) };
  const int32_t gl = Lo(k->glc4);
  const int32_t wa = Lo(k->width), wb = Hi(k->width);
  const int32_t dk_lo = Lo(k->dk), dk_hi = Hi(k->dk);
  uint32_t rng = k->rng;

  for (int32_t i = 0; i < n; ++i) {
    --p;
    // The dither: xorshift32, each half >> 1 (+-1/2 LSB once x dk = 1).
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    const int32_t r_lo = Lo(static_cast<int32_t>(rng)) >> 1;
    const int32_t r_hi = Hi(static_cast<int32_t>(rng)) >> 1;
    // Input: gain (freeze gates it), the early line, three allpasses, x 1/2.
    const int32_t xg = Rnd(Fr(x[i], Hi(gain)));
    gain += gstep;
    WRITE(HL_ER, xg)
    int64_t acc = static_cast<int64_t>(xg) << 16;
    ALLPASS(HL_AP1, HL_AP1_LEN, mg, g)
    ALLPASS(HL_AP2, HL_AP2_LEN, mg, g)
    ALLPASS(HL_AP3, HL_AP3_LEN, mg, g)
    acc >>= 1;
    const int32_t d = Rnd(acc);

    // The crossfade's weights.
    const int32_t xb = xf >> 16, xa = 32767 - xb;
    if (mode == kXfade) xf += k->xfstep;

    // The lines' reads: slots 0, 1 at +A, 2, 3 at -A, 4, 5 at +B, 6, 7 at -B.
    int32_t y[8];
    for (int32_t m = 0; m < 2; ++m) {
      const int32_t ip = pos[m] >> 16, f = (pos[m] & 0xffff) >> 1;
      pos[m] += step[m];
      for (int32_t s = 4 * m; s < 4 * m + 4; ++s) {
        const bool neg = (s & 2) != 0;
        const int32_t q = neg ? -ip - 1 : ip;
        const int32_t w0 = neg ? f : 32767 - f, w1 = neg ? 32767 - f : f;
        y[s] = Rnd(Fr(p[o[s] + q], w0) + Fr(p[o[s] + q + 1], w1));
        if (mode == kXfade) {
          const int32_t yn = Rnd(Fr(p[xo[s] + q], w0) + Fr(p[xo[s] + q + 1], w1));
          y[s] = Rnd(Fr(y[s], xa) + Fr(yn, xb));
        }
      }
    }
    // Damping (the filter's state is its output).
    // Stochastic rounding: + the dither (low lines r_lo, high lines r_hi).
    for (int32_t s = 0; s < 8; ++s) {
      const int64_t dither = (s & 1) ? Fr(r_hi, dk_hi) : Fr(r_lo, dk_lo);
      z[s] = Rnd(Fr(z[s], ca[s]) + Fr(y[s], cb[s]) + dither);
    }
    // The output's sums, halved: even lines (0 2 | 4 6), odd (1 3 | 5 7).
    const int32_t e_lo = (y[0] + y[2]) >> 1, e_hi = (y[1] + y[3]) >> 1;
    const int32_t o_lo = (y[4] + y[6]) >> 1, o_hi = (y[5] + y[7]) >> 1;
    // Householder: z - (2 / 8) sum z, + the input.
    int64_t sum = 0;
    for (int32_t s = 0; s < 8; ++s) sum += Fr(z[s], 8192);
    const int32_t mm = Rnd(sum);
    for (int32_t s = 0; s < 8; ++s) {
      const int32_t v = Sat16(z[s] - mm);
      WRITE(base[s], Sat16(sign[s] > 0 ? v + d : v - d))
    }

    // Output.
    int64_t acc_l, acc_r = 0;
    if (mode == kXfade) {
      int64_t ol = 0, nl = 0, orr = 0, nr = 0;
      for (int32_t j = 0; j < 5; ++j) {
        ol += Fr(p[eo[j]], c[j]);
        nl += Fr(p[xeo[j]], c[j]);
        orr += Fr(p[eo[5 + j]], c[j]);
        nr += Fr(p[xeo[5 + j]], c[j]);
      }
      const int32_t er_l = Rnd(Fr(Rnd(ol), xa) + Fr(Rnd(nl), xb));
      const int32_t er_r = Rnd(Fr(Rnd(orr), xa) + Fr(Rnd(nr), xb));
      acc_l = static_cast<int64_t>(er_l) * 65536 + Fr(e_lo, gl) + Fr(e_hi, gl);
      acc_r = static_cast<int64_t>(er_r) * 65536 + Fr(o_lo, gl) + Fr(o_hi, gl);
    } else {
      acc_l = Fr(e_lo, gl) + Fr(e_hi, gl);
      for (int32_t j = 0; j < 5; ++j) acc_l += Fr(p[eo[j]], c[j]);
      if (mode == kStereo) {
        acc_r = Fr(o_lo, gl) + Fr(o_hi, gl);
        for (int32_t j = 0; j < 5; ++j) acc_r += Fr(p[eo[5 + j]], c[j]);
      }
    }
    if (mode == kMono) {
      l[i] = static_cast<int16_t>(Rnd(acc_l * 2));
    } else {
      const int32_t ml = Rnd(acc_l), mr = Rnd(acc_r);
      l[i] = static_cast<int16_t>(Rnd((Fr(ml, wa) + Fr(mr, wb)) * 2));
      r[i] = static_cast<int16_t>(Rnd((Fr(ml, wb) + Fr(mr, wa)) * 2));
    }
  }
  for (int32_t s = 0; s < 4; ++s) k->z[s] = Pack(z[2 * s], z[2 * s + 1]);
  k->gain = gain;
  k->rng = rng;
  k->posa = pos[0];
  k->posb = pos[1];
  if (mode == kXfade) k->xf = xf;
}

#undef ALLPASS
#undef WRITE
#endif  // !__bfin__

}  // namespace

#if !defined(__bfin__)
void HallRunC(HallKernel* k, const int16_t* x, int16_t* l, int16_t* r, int16_t* p, int32_t n,
              int32_t alias_bytes) {
  if (r) {
    Run<kStereo>(k, x, l, r, p, n, alias_bytes);
  } else {
    Run<kMono>(k, x, l, nullptr, p, n, alias_bytes);
  }
}

void HallXfadeRunC(HallKernel* k, const int16_t* x, int16_t* l, int16_t* r, int16_t* p, int32_t n,
                   int32_t alias_bytes) {
  Run<kXfade>(k, x, l, r, p, n, alias_bytes);
}

// The two-tap interpolation (Rnd(Fr + Fr)) in 32 bits: round(acc / 2^15),
// ties to even.
void HallPreRunC(const int16_t* l, const int16_t* r, int16_t* ring, int16_t* x, int32_t w,
                 int32_t d, int32_t step, int32_t n) {
  for (int32_t i = 0; i < n; ++i, ++w) {
    ring[w] = static_cast<int16_t>((l[i] + r[i]) >> 1);
    const int32_t ip = d >> 16, f = (d & 0xffff) >> 1;
    d += step;
    int32_t r0 = w - ip;
    r0 += (r0 >> 31) & HL_PRE_WORDS;
    int32_t r1 = r0 - 1;
    r1 += (r1 >> 31) & HL_PRE_WORDS;
    const int32_t acc = ring[r0] * (32767 - f) + ring[r1] * f;
    x[i] = static_cast<int16_t>((acc + 0x3fff + ((acc >> 15) & 1)) >> 15);
  }
}
#endif  // !__bfin__

// --- Control ---

void HallControl::Init() {
  started_ = false;
  steady_ = false;
  size_fade_ = true;
  phase_[0] = 0;
  phase_[1] = 0x40000000u;
  size_ = size_new_ = kHallNumSizes - 1;
  xf_ = 0;
  xs_ = 0;
  xfading_ = false;
  gain_ = 0;
  pre_ = 0;
  freeze_ = 0;
  mod_[0] = mod_[1] = 0;
  decay_ = damp_ = early_ = motion_ = width_ = 0;
}

namespace {

bool SameParams(const HallParams& a, const HallParams& b) {
  return a.size == b.size && a.decay == b.decay && a.damp == b.damp && a.predelay == b.predelay &&
         a.early == b.early && a.motion == b.motion && a.width == b.width && a.freeze == b.freeze;
}

}  // namespace

// Modulation: two parabolic-sine LFOs (0.61 and 0.87 Hz), excursion
// MOTION x 16 samples; ramped per sample to the value at the call's end.
void HallControl::UpdateMod(int32_t n, HallCoefs* c) {
  const int32_t exc = motion_ >> 8;   // Q11 samples
  const uint32_t inc[2] = { kLfoIncA, kLfoIncB };
  for (int32_t j = 0; j < 2; ++j) {
    phase_[j] += inc[j] * static_cast<uint32_t>(n);
    const int32_t target = (exc * Sine(phase_[j])) >> 10;   // Q16
    c->mod0[j] = mod_[j];
    c->mod_step[j] = Ramp(target - mod_[j], n);
    mod_[j] += c->mod_step[j] * n;
  }
}

// Jot's per-line damping: DC gain g = 2^(-L T), Nyquist gain g^ratio, as
// the one-pole z' = a z + b y with a = (1 - q) / (1 + q), q = 2^(-L T
// (ratio - 1)), b = g (1 - a). At infinite decay g = 1 and the highs
// keep 20 s's ratio. Freeze blends to a = 0, b = 1.
void HallControl::LineCoefs(int32_t size, int32_t* a_out, int32_t* b_out) const {
  const int32_t decay = decay_ >> 8, damp = damp_ >> 8;
  const bool infinite = decay >= kHallInfinite;
  const int32_t t = Lookup33(kDecayTable, infinite ? kHallInfinite - 1 : decay);
  const int32_t rm1 = Lookup33(kRatioTable, damp);
  const HallSizeTable& s = kHallSizes[size];
  const int32_t fz = freeze_;
  for (int32_t j = 0; j < 8; ++j) {
    const int32_t e = s.len[j] * t;   // Q24 (< 2^27)
    const int32_t gq = infinite ? 32767 : Exp2Neg(e);
    const int32_t a = Pole((static_cast<int64_t>(e) * rm1) >> 12);
    const int32_t b = (gq * (32767 - a)) >> 15;
    a_out[j] = (a * (32768 - fz)) >> 15;
    b_out[j] = b + (((32767 - b) * fz) >> 15);
  }
}

// The size-independent coefficients.
void HallControl::Coefficients(HallCoefs* c) const {
  const int32_t early = early_ >> 8;
  const int32_t ge = early;
  const int32_t gm = early <= 16383 ? 32767 : 2 * (32767 - early);
  for (int32_t j = 0; j < 5; ++j) c->c[j] = (kHallEarlyCoef[j] * ge) >> 15;
  c->gl = (kLateGain * gm) >> 15;
  const int32_t w = width_ >> 8;
  c->wa = (32767 + w) >> 1;
  c->wb = (32767 - w) >> 1;
  c->dg = kDiffusion;
  // The dither: off where the loop must be lossless.
  c->dither = (freeze_ != 0 || (decay_ >> 8) >= kHallInfinite) ? 0 : 1;
}

void HallControl::Update(const HallParams& p, int32_t n, HallCoefs* c) {
  if (steady_ && SameParams(p, last_)) {
    *c = cached_;
    UpdateMod(n, c);
    return;
  }
  const int32_t decay_t = Clamp(p.decay, 0, 32767);
  const int32_t damp_t = Clamp(p.damp, 0, 32767);
  const int32_t early_t = Clamp(p.early, 0, 32767);
  // Frozen, the modulation parks (its interpolation would slowly low-pass
  // the held tail).
  const int32_t motion_t = p.freeze ? 0 : Clamp(p.motion, 0, 32767);
  const int32_t width_t = Clamp(p.width, 0, 32767);
  const int32_t size_t_ = Clamp(p.size, 0, 32767) >> 11;
  const int32_t freeze_t = p.freeze ? 32768 : 0;
  const int32_t gain_t = p.freeze ? 0 : kInputGain;
  // Integer samples: a still pre-delay reads without interpolation.
  const int32_t pre_t = ((Clamp(p.predelay, 0, 32767) * 8820) + 32768) & ~0xffff;

  const bool first = !started_;
  if (first) {
    started_ = true;
    decay_ = decay_t << 8;
    damp_ = damp_t << 8;
    early_ = early_t << 8;
    motion_ = motion_t << 8;
    width_ = width_t << 8;
    freeze_ = freeze_t;
    size_ = size_new_ = size_t_;
    gain_ = gain_t;
    pre_ = pre_t;
  } else {
    decay_ = Smooth(decay_, decay_t);
    damp_ = Smooth(damp_, damp_t);
    early_ = Smooth(early_, early_t);
    motion_ = Smooth(motion_, motion_t);
    width_ = Smooth(width_, width_t);
    // Freeze: to its target within 4 calls.
    if (freeze_ < freeze_t) freeze_ = freeze_ + 8192 > freeze_t ? freeze_t : freeze_ + 8192;
    if (freeze_ > freeze_t) freeze_ = freeze_ - 8192 < freeze_t ? freeze_t : freeze_ - 8192;
  }

  // Input gain: to its target within 4 calls (freeze gates it).
  int32_t gain_end = gain_t;
  if (gain_end > gain_ + kInputGain / 4) gain_end = gain_ + kInputGain / 4;
  if (gain_end < gain_ - kInputGain / 4) gain_end = gain_ - kInputGain / 4;
  c->gain0 = gain_;
  c->gain_step = Ramp(gain_end - gain_, n);
  gain_ += c->gain_step * n;

  // Pre-delay: glides an eighth of the way each call, <= 1/4 sample per sample.
  int32_t d = pre_t - pre_;
  int32_t pre_end;
  if (d < 4096 && d > -4096) {
    pre_end = pre_t;
  } else {
    d >>= 3;
    const int32_t slew = kPreSlew * n;
    pre_end = pre_ + Clamp(d, -slew, slew);
  }
  c->pre0 = pre_;
  c->pre_step = Ramp(pre_end - pre_, n);
  pre_ += c->pre_step * n;
  if (pre_end == pre_t && pre_ != pre_t && c->pre_step == 0) pre_ = pre_t;

  // Size: crossfade every size-dependent read to the new size over
  // kXfadeSamples (the output included). A size reached while one runs
  // starts the next when it ends. Without the fade (tests): at once.
  if (!size_fade_) {
    size_ = size_new_ = size_t_;
    xfading_ = false;
  } else if (!xfading_ && size_ != size_t_) {
    xfading_ = true;
    size_new_ = size_t_;
    xf_ = 0;
    xs_ = 0;
  }
  c->size = size_;
  c->size_new = size_new_;
  c->xfade = xfading_;
  Coefficients(c);
  // The lines' losses follow their lengths: during a crossfade they step
  // from the old size's to the new one's, by the crossfade's weight at
  // the end of each call (8 steps instead of one jump).
  LineCoefs(size_, c->a, c->b);
  if (xfading_) {
    xs_ = xs_ + n < kXfadeSamples ? xs_ + n : kXfadeSamples;
    const int32_t i = xs_ >> 3, f = xs_ & 7;
    const int32_t xf_end = i >= 32 ? kOne
                                   : kXfadeCurve.v[i] + static_cast<int32_t>(
                                         (static_cast<int64_t>(kXfadeCurve.v[i + 1] - kXfadeCurve.v[i]) * f) >> 3);
    c->xf0 = xf_;
    c->xf_step = Ramp(xf_end - xf_, n);
    xf_ += c->xf_step * n;
    int32_t an[8], bn[8];
    LineCoefs(size_new_, an, bn);
    const int32_t w = xf_end >> 16;
    for (int32_t j = 0; j < 8; ++j) {
      c->a[j] += ((an[j] - c->a[j]) * w) >> 15;
      c->b[j] += ((bn[j] - c->b[j]) * w) >> 15;
    }
    if (xs_ == kXfadeSamples) {
      size_ = size_new_;   // the new size from the next call on
      xfading_ = false;
    }
  } else {
    c->xf0 = c->xf_step = 0;
  }

  if (first) {
    const int32_t exc = motion_ >> 8;
    mod_[0] = (exc * Sine(phase_[0])) >> 10;
    mod_[1] = (exc * Sine(phase_[1])) >> 10;
  }
  UpdateMod(n, c);

  // Settled: the next calls with the same knobs take these coefficients
  // (bit-identical: every smoother sits on its target, every ramp's step
  // is 0).
  steady_ = !xfading_ && !c->xfade && size_ == size_t_ && c->gain_step == 0 &&
            c->pre_step == 0 && gain_ == gain_t && pre_ == pre_t && freeze_ == freeze_t &&
            decay_ == decay_t << 8 && damp_ == damp_t << 8 && early_ == early_t << 8 &&
            motion_ == motion_t << 8 && width_ == width_t << 8;
  if (steady_) {
    last_ = p;
    cached_ = *c;
  }
}

// --- The reverb ---

static_assert(offsetof(HallKernel, off) == HK_OFF && offsetof(HallKernel, xoff) == HK_XOFF &&
              offsetof(HallKernel, eroff) == HK_EROFF && offsetof(HallKernel, xeroff) == HK_XEROFF &&
              offsetof(HallKernel, z) == HK_Z && offsetof(HallKernel, ca) == HK_CA &&
              offsetof(HallKernel, cb) == HK_CB && offsetof(HallKernel, posa) == HK_POSA &&
              offsetof(HallKernel, stepa) == HK_STEPA && offsetof(HallKernel, posb) == HK_POSB &&
              offsetof(HallKernel, stepb) == HK_STEPB && offsetof(HallKernel, gain) == HK_GAIN &&
              offsetof(HallKernel, gstep) == HK_GSTEP && offsetof(HallKernel, dg) == HK_DG &&
              offsetof(HallKernel, c01) == HK_C01 && offsetof(HallKernel, c23) == HK_C23 &&
              offsetof(HallKernel, glc4) == HK_GLC4 && offsetof(HallKernel, width) == HK_WIDTH &&
              offsetof(HallKernel, xf) == HK_XF && offsetof(HallKernel, xfstep) == HK_XFSTEP &&
              offsetof(HallKernel, e) == HK_E && offsetof(HallKernel, o) == HK_O &&
              offsetof(HallKernel, rng) == HK_RNG && offsetof(HallKernel, dk) == HK_DK &&
              sizeof(HallKernel) == HK_BYTES, "kernel block");

namespace {

void SizeOffsets(int32_t size, int32_t* off, int32_t* eroff) {
  const HallSizeTable& t = kHallSizes[size];
  for (int32_t s = 0; s < 8; ++s) {
    const int32_t line = kHallSlotLine[s];
    off[s] = 2 * (kLineBase[line] + t.len[line]);
  }
  for (int32_t j = 0; j < 5; ++j) {
    eroff[j] = 2 * (HL_ER + t.er_l[j]);
    eroff[5 + j] = 2 * (HL_ER + t.er_r[j]);
  }
}

}  // namespace

void HallReverb::Init() {
  InitKeepBuffers();
  for (int32_t i = 0; i < HL_PRE_WORDS; ++i) pre_[i] = 0;
  for (int32_t i = 0; i < HL_WORDS; ++i) buf_[i] = 0;
}

void HallReverb::InitKeepBuffers() {
  static_assert(offsetof(HallReverb, buf_) == offsetof(HallReverb, pre_) + 2 * HL_PRE_WORDS,
                "ClearBase: the pre-delay ring and the tank are one run of words");
  control_.Init();
  for (int32_t i = 0; i < kHallMaxBlock; ++i) x_[i] = r_[i] = 0;
  for (int32_t i = 0; i < 4; ++i) k_.z[i] = 0;
  for (int32_t i = 0; i < 8; ++i) k_.off[i] = k_.xoff[i] = 2 * HL_L0;
  for (int32_t i = 0; i < 10; ++i) k_.eroff[i] = k_.xeroff[i] = 2 * HL_ER;
  k_.xf = k_.xfstep = 0;
  k_.e = k_.o = 0;
  k_.rng = 0x9e3779b9u;
  k_.dk = 0;
  k_size_ = -1;
  ptr_ = HL_SIZE;
  pre_w_ = 0;
#if defined(__bfin__) && !defined(OP1FX_NO_ASM)
  use_asm_ = true;
#else
  use_asm_ = false;
#endif
}

// The mono sum into the pre-delay ring, and the delayed signal into x_; the
// write's wrap splits the run.
void HallReverb::Pre(const int16_t* in_l, const int16_t* in_r, int32_t n) {
  const int16_t* const r = in_r ? in_r : in_l;   // mono: (l + l) >> 1 = l
  int32_t w = pre_w_;
  int32_t d = coefs_.pre0;
  const int32_t step = coefs_.pre_step;
  int32_t i = 0;
  while (i < n) {
    int32_t m = HL_PRE_WORDS - w;
    if (m > n - i) m = n - i;
#if defined(__bfin__)
    HallPreRunAsm(in_l + i, r + i, pre_, x_ + i, w, d, step, m);
#else
    HallPreRunC(in_l + i, r + i, pre_, x_ + i, w, d, step, m);
#endif
    d += step * m;
    w += m;
    if (w == HL_PRE_WORDS) w = 0;
    i += m;
  }
  pre_w_ = w;
}

void HallReverb::Process(const int16_t* in_l, const int16_t* in_r, int16_t* wet_l,
                         int16_t* wet_r, int32_t n, const HallParams& params) {
  control_.Update(params, n, &coefs_);
  const HallCoefs& c = coefs_;
  if (c.size != k_size_) {
    k_size_ = c.size;
    SizeOffsets(c.size, k_.off, k_.eroff);
  }
  if (c.xfade) {
    SizeOffsets(c.size_new, k_.xoff, k_.xeroff);
    k_.xf = c.xf0;
    k_.xfstep = c.xf_step;
  }
  for (int32_t i = 0; i < 4; ++i) {
    const int32_t lo = kHallSlotLine[2 * i], hi = kHallSlotLine[2 * i + 1];
    k_.ca[i] = Pack(c.a[lo], c.a[hi]);
    k_.cb[i] = Pack(c.b[lo], c.b[hi]);
  }
  k_.posa = c.mod0[0];
  k_.stepa = c.mod_step[0];
  k_.posb = c.mod0[1];
  k_.stepb = c.mod_step[1];
  k_.gain = c.gain0;
  k_.gstep = c.gain_step;
  k_.dg = Pack(-c.dg, c.dg);
  k_.c01 = Pack(c.c[0], c.c[1]);
  k_.c23 = Pack(c.c[2], c.c[3]);
  k_.glc4 = Pack(c.gl, c.c[4]);
  k_.width = Pack(c.wa, c.wb);
  k_.dk = Pack(c.dither, c.dither);

  Pre(in_l, in_r, n);

  // Runs end where p wraps and where the mirroring starts.
  int32_t done = 0;
  while (done < n) {
    const int32_t next = ptr_ - 1;
    int32_t m = next + 1;
    const bool alias = next < HL_MAX_LINE;
    if (!alias && m > next - HL_MAX_LINE + 1) m = next - HL_MAX_LINE + 1;
    if (m > n - done) m = n - done;
    const int32_t alias_bytes = alias ? 2 * HL_SIZE : 0;
    int16_t* const p = buf_ + ptr_;
    int16_t* const l = wet_l + done;
    if (c.xfade) {
      int16_t* const r = wet_r ? wet_r + done : r_ + done;
#if defined(__bfin__)
      HallXfadeRunAsm(&k_, x_ + done, l, r, p, m, alias_bytes);
#else
      HallXfadeRunC(&k_, x_ + done, l, r, p, m, alias_bytes);
#endif
    } else {
      int16_t* const r = wet_r ? wet_r + done : nullptr;
#if defined(__bfin__)
      // coop: stereo out at every position (the glue always passes wet_r;
      // the mono kernels are not built, hall_bfin.S HALL_MONO_KERNELS).
      if (alias) {
        HallRunAsm(&k_, x_ + done, l, r, p, m, alias_bytes);
      } else {
        HallRunNmAsm(&k_, x_ + done, l, r, p, m, 0);
      }
#else
      HallRunC(&k_, x_ + done, l, r, p, m, alias_bytes);
#endif
    }
    ptr_ -= m;
    if (ptr_ == 0) ptr_ = HL_SIZE;
    done += m;
  }
}

}  // namespace op1fx
