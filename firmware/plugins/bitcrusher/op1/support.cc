// Minimal runtime for injected code: GCC may emit calls to these for struct copies and array clears (no libc in a
// plugin, guidelines §2.1). Everything else comes from libgcc. From te-op1-cfw's ports/rings/op1/support.cc, which
// the kennel bitcrusher linked (its plugin.json "sources").
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.

#include <cstddef>
#include <cstdint>

extern "C" {

// Word at a time when both pointers are word-aligned.
void* memset(void* dst, int value, size_t n) {
  unsigned char* d = static_cast<unsigned char*>(dst);
  if ((reinterpret_cast<uintptr_t>(d) & 3) == 0) {
    uint32_t w = static_cast<unsigned char>(value) * 0x01010101u;
    uint32_t* dw = reinterpret_cast<uint32_t*>(d);
    for (; n >= 4; n -= 4) *dw++ = w;
    d = reinterpret_cast<unsigned char*>(dw);
  }
  while (n--) *d++ = static_cast<unsigned char>(value);
  return dst;
}

void* memcpy(void* dst, const void* src, size_t n) {
  unsigned char* d = static_cast<unsigned char*>(dst);
  const unsigned char* s = static_cast<const unsigned char*>(src);
  if (((reinterpret_cast<uintptr_t>(d) | reinterpret_cast<uintptr_t>(s)) & 3) == 0) {
    uint32_t* dw = reinterpret_cast<uint32_t*>(d);
    const uint32_t* sw = reinterpret_cast<const uint32_t*>(s);
    for (; n >= 4; n -= 4) *dw++ = *sw++;
    d = reinterpret_cast<unsigned char*>(dw);
    s = reinterpret_cast<const unsigned char*>(sw);
  }
  while (n--) *d++ = *s++;
  return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
  unsigned char* d = static_cast<unsigned char*>(dst);
  const unsigned char* s = static_cast<const unsigned char*>(src);
  if (d < s) {
    while (n--) *d++ = *s++;
  } else {
    d += n;
    s += n;
    while (n--) *--d = *--s;
  }
  return dst;
}

}  // extern "C"
