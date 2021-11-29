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
#ifndef NGTCP2_GAPTR_H
#define NGTCP2_GAPTR_H

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <ngtcp2/ngtcp2.h>

#include "ngtcp2_mem.h"
#include "ngtcp2_range.h"

struct ngtcp2_gaptr_gap;
typedef struct ngtcp2_gaptr_gap ngtcp2_gaptr_gap;

/*
 * ngtcp2_gaptr_gap represents the gap, which is the range of stream
 * data that is not received yet.
 */
struct ngtcp2_gaptr_gap {
  /* next points to the next gap.  This singly linked list is ordered
     by range.begin in the increasing order, and they never
     overlap. */
  ngtcp2_gaptr_gap *next;
  /* range is the range of this gap. */
  ngtcp2_range range;
};

/*
 * ngtcp2_gaptr_gap_new allocates new ngtcp2_gaptr_gap object, and
 * assigns its pointer to |*pg|.  The caller should call
 * ngtcp2_gaptr_gap_del to delete it when it is no longer used.  The
 * range of the gap is [begin, end).  |mem| is custom memory allocator
 * to allocate memory.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
int ngtcp2_gaptr_gap_new(ngtcp2_gaptr_gap **pg, uint64_t begin, uint64_t end,
                         ngtcp2_mem *mem);

/*
 * ngtcp2_gaptr_gap_del deallocates |g|.  It deallocates the memory
 * pointed by |g| it self.  |mem| is custom memory allocator to
 * deallocate memory.
 */
void ngtcp2_gaptr_gap_del(ngtcp2_gaptr_gap *g, ngtcp2_mem *mem);

/*
 * ngtcp2_gaptr maintains the gap in the range [0, UINT64_MAX).
 */
typedef struct {
  /* gap maintains the range of offset which is not received
     yet. Initially, its range is [0, UINT64_MAX). */
  ngtcp2_gaptr_gap *gap;
  /* mem is custom memory allocator */
  ngtcp2_mem *mem;
} ngtcp2_gaptr;

/*
 * ngtcp2_gaptr_init initializes |gaptr|.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.
 */
int ngtcp2_gaptr_init(ngtcp2_gaptr *gaptr, ngtcp2_mem *mem);

/*
 * ngtcp2_gaptr_free frees resources allocated for |gaptr|.
 */
void ngtcp2_gaptr_free(ngtcp2_gaptr *gaptr);

/*
 * ngtcp2_gaptr_push adds new data of length |datalen| at the stream
 * offset |offset|.
 *
 * This function returns 0 if it succeeds, or one of the following
 * negative error codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory
 */
int ngtcp2_gaptr_push(ngtcp2_gaptr *gaptr, uint64_t offset, size_t datalen);

/*
 * ngtcp2_gaptr_first_gap_offset returns the offset to the first gap.
 * If there is no gap, it returns UINT64_MAX.
 */
uint64_t ngtcp2_gaptr_first_gap_offset(ngtcp2_gaptr *gaptr);

#endif /* NGTCP2_GAPTR_H */
