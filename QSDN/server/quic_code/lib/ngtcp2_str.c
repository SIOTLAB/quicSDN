/*
 * ngtcp2
 *
 * Copyright (c) 2017 ngtcp2 contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "ngtcp2_str.h"

#include <string.h>

uint8_t *ngtcp2_cpymem(uint8_t *dest, const uint8_t *src, size_t n) {
  memcpy(dest, src, n);
  return dest + n;
}

#define LOWER_XDIGITS "0123456789abcdef"

uint8_t *ngtcp2_encode_hex(uint8_t *dest, const uint8_t *data, size_t len) {
  size_t i;
  uint8_t *p = dest;

  for (i = 0; i < len; ++i) {
    *p++ = (uint8_t)LOWER_XDIGITS[data[i] >> 4];
    *p++ = (uint8_t)LOWER_XDIGITS[data[i] & 0xf];
  }

  *p = '\0';

  return dest;
}
