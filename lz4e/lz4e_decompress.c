/*
 * LZ4 - Fast LZ compression algorithm
 * Copyright (C) 2011 - 2016, Yann Collet.
 * BSD 2 - Clause License (http://www.opensource.org/licenses/bsd - license.php)
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *	* Redistributions of source code must retain the above copyright
 *	  notice, this list of conditions and the following disclaimer.
 *	* Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * You can contact the author at :
 *	- LZ4 homepage : http://www.lz4.org
 *	- LZ4 source repository : https://github.com/lz4/lz4
 *
 *	Changed for kernel usage by:
 *	Sven Schmidt <4sschmid@informatik.uni-hamburg.de>
 */

/*-************************************
 *	Dependencies
 **************************************/
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/unaligned.h>

#include "include/lz4e/lz4e.h"
#include "include/lz4e/lz4e_defs.h"

/*-*****************************
 *	Decompression functions
 *******************************/

#define DEBUGLOG(l, ...) \
	{                \
	} /* disabled */

#ifndef assert
#define assert(condition) ((void)0)
#endif

/*
 * LZ4_decompress_generic() :
 * This generic decompression function covers all use cases.
 * It shall be instantiated several times, using different sets of directives.
 * Note that it is important for performance that this function really get
 * inlined, in order to remove useless branches during compilation optimization.
 */
static FORCE_INLINE void LZ4E_iter_rollback(const struct bio_vec *bvecs,
					    struct bvec_iter *iter, int cnt)
{
	for (int i = 0; i < cnt; i++) {
		LZ4E_iter_rollback1(bvecs, iter);
	}
}

static FORCE_INLINE void LZ4E_memmove(struct bio_vec *dst,
				      struct bvec_iter *dstIter,
				      const struct bio_vec *src,
				      struct bvec_iter *srcIter, size_t count)
{
	struct bvec_iter src_cur = *srcIter;
	struct bvec_iter dst_cur = *dstIter;

	while (count > 0) {
		struct bio_vec src_seg = bvec_iter_bvec(src, src_cur);
		struct bio_vec dst_seg = bvec_iter_bvec(dst, dst_cur);

		size_t src_avail = src_seg.bv_len - src_cur.bi_bvec_done;
		size_t dst_avail = dst_seg.bv_len - dst_cur.bi_bvec_done;

		size_t chunk = min_t(size_t, count, min(src_avail, dst_avail));

		if (chunk == 0) {
			bvec_iter_advance_single(src, &src_cur, 0);
			bvec_iter_advance_single(dst, &dst_cur, 0);
			continue;
		}
		LZ4E_memcpy_to_sg(dst,
				  (const char *)page_address(src_seg.bv_page) +
					  src_seg.bv_offset +
					  src_cur.bi_bvec_done,
				  dst_cur, chunk);
		bvec_iter_advance_single(src, &src_cur, chunk);
		bvec_iter_advance_single(dst, &dst_cur, chunk);
		count -= chunk;
	}
	*srcIter = src_cur;
	*dstIter = dst_cur;
}

static inline const BYTE *bvec_iter_addr(const struct bio_vec *bv,
					 const struct bvec_iter *it)
{
	const struct bio_vec *vec = &bv[it->bi_idx];
	return (const BYTE *)vec->bv_page + vec->bv_offset + it->bi_bvec_done;
}

static FORCE_INLINE int LZ4E_decompress_generic_sg(
	const struct bio_vec *const src, struct bio_vec *const dst,
	struct bvec_iter *const srcIter, struct bvec_iter *const dstIter,
	endCondition_directive endOnInput, earlyEnd_directive partialDecoding,
	dict_directive dict, const BYTE *const lowPrefix,
	const BYTE *const dictStart, const size_t dictSize)
{
	const unsigned int srcSize = srcIter->bi_size;
	const unsigned int outputSize = dstIter->bi_size;

	const BYTE *const dictEnd = (const BYTE *)dictStart + dictSize;
	static const unsigned int inc32table[8] = { 0, 1, 2, 1, 0, 4, 4, 4 };
	static const int dec64table[8] = { 0, 0, 0, -1, -4, 1, 2, 3 };

	const int safeDecode = (endOnInput == endOnInputSize);
	const int checkOffset = (safeDecode && (dictSize < (size_t)(64 * KB)));

	assert(lowPrefix <= bvec_iter_addr(dst, dstIter));
	assert(src != NULL);

	size_t remainingOutput = 0;
	size_t remainingInput = 0;
	size_t cpy;

	if (endOnInput && unlikely(outputSize == 0)) {
		return ((srcSize == 1) && (LZ4E_read8(src, *srcIter) == 0)) ?
			       0 :
			       -1;
	}
	if (!endOnInput && unlikely(outputSize == 0)) {
		return (LZ4E_read8(src, *srcIter) == 0) ? 1 : -1;
	}
	if (endOnInput && unlikely(srcSize == 0)) {
		return -1;
	}

	const size_t shortInputLimit = endOnInput ? 14 : 8;
	const size_t shortOutputLimit = endOnInput ? 14 : 8;
	const BYTE *shortIEnd =
		bvec_iter_addr(src, srcIter) + srcSize - shortInputLimit - 2;
	const BYTE *shortOEnd = bvec_iter_addr(dst, dstIter) + outputSize -
				shortOutputLimit - 18;

	while (1) {
		size_t length;
		size_t offset;
		unsigned int token;
		struct bvec_iter matchIter;

		token = LZ4E_read8(src, *srcIter);
		bvec_iter_advance(src, srcIter, 1);
		remainingInput++;
		length = token >> ML_BITS;

		if ((endOnInput ? length != RUN_MASK : length <= 8) &&
		    likely((endOnInput ?
				    bvec_iter_addr(src, srcIter) < shortIEnd :
				    1) &
			   (bvec_iter_addr(dst, dstIter) <= shortOEnd))) {
			LZ4E_memcpy(dst, src, *dstIter, *srcIter,
				    (endOnInput ? 16 : 8));
			remainingInput += length;
			remainingOutput += length;

			length = token & ML_MASK;
			offset = LZ4E_readLE16(src, *srcIter);
			remainingInput += 2;

			matchIter = *dstIter;
			LZ4E_iter_rollback(dst, &matchIter, offset);

			if ((length != ML_MASK) && (offset >= 8) &&
			    (dict == withPrefix64k ||
			     bvec_iter_addr(dst, &matchIter) >= lowPrefix)) {
				LZ4E_memcpy(dst, dst, *dstIter, matchIter, 8);
				LZ4E_memcpy(dst, dst, *dstIter, matchIter, 8);
				LZ4E_memcpy(dst, dst, *dstIter, matchIter, 2);
				remainingOutput += (length + MINMATCH);
				continue;
			}
			goto _copy_match;
		}
		if (length == RUN_MASK) {
			unsigned int s;

			if (unlikely(endOnInput ? remainingInput >=
							  srcSize - RUN_MASK :
						  0)) {
				goto _output_error;
			}
			do {
				s = LZ4E_read8(src, *srcIter);
				remainingInput++;
				length += s;
			} while ((likely(endOnInput ?
						 remainingInput <
							 srcSize - RUN_MASK :
						 1) &
				  (s == 255)));

			if (safeDecode &&
			    unlikely((uptrval)bvec_iter_addr(dst, dstIter) +
					     length <
				     (uptrval)bvec_iter_addr(dst, dstIter))) {
				goto _output_error;
			}
			if (safeDecode &&
			    unlikely((uptrval)bvec_iter_addr(src, srcIter) +
					     length <
				     (uptrval)bvec_iter_addr(src, srcIter))) {
				goto _output_error;
			}
		}
		cpy = remainingOutput + length;

		if (((endOnInput) && ((cpy > outputSize - MFLIMIT) ||
				      (remainingInput + length >
				       srcSize - (2 + 1 + LASTLITERALS)))) ||
		    ((!endOnInput) && (cpy > outputSize - WILDCOPYLENGTH))) {
			if (partialDecoding) {
				if (cpy > outputSize) {
					cpy = outputSize;
					length = outputSize - remainingOutput;
				}
				if ((endOnInput) &&
				    (remainingInput + length > srcSize)) {
					goto _output_error;
				}
			} else {
				if ((!endOnInput) && (cpy != outputSize)) {
					goto _output_error;
				}
				if ((endOnInput) &&
				    ((remainingInput + length != srcSize) ||
				     (cpy > outputSize))) {
					goto _output_error;
				}
			}
			LZ4E_memmove(dst, dstIter, src, srcIter, length);
			remainingInput += length;
			remainingOutput += length;
			if ((!partialDecoding) || (cpy == outputSize) ||
			    (remainingInput >= (srcSize - 2))) {
				break;
			}
		} else {
			LZ4E_wildCopy(dst, src, *dstIter, *srcIter, length);
			remainingInput += length;
			remainingOutput = cpy;
		}

		offset = LZ4E_readLE16(src, *srcIter);
		remainingInput += 2;

		matchIter = *dstIter;
		LZ4E_iter_rollback(dst, &matchIter, offset);

	_copy_match:
		if (checkOffset &&
		    unlikely(bvec_iter_addr(dst, &matchIter) + dictSize <
			     lowPrefix)) {
			goto _output_error;
		}

		if (!partialDecoding) {
			assert(outputSize > remainingOutput);
			assert(outputSize - remainingOutput >= 4);

			LZ4E_write32(dst, (U32)offset, *dstIter);
		}

		if (length == ML_MASK) {
			unsigned int s;
			do {
				s = LZ4E_read8(src, *srcIter);

				if ((endOnInput) &&
				    (remainingInput > srcSize - LASTLITERALS)) {
					goto _output_error;
				}

				length += s;
			} while (s == 255);

			if (safeDecode) {
				if (unlikely((uptrval)bvec_iter_addr(dst,
								     dstIter) +
						     length <
					     (uptrval)bvec_iter_addr(
						     dst, dstIter))) {
					goto _output_error;
				}
			}
		}

		length += MINMATCH;
		if ((dict == usingExtDict) &&
		    (bvec_iter_addr(dst, &matchIter) < lowPrefix)) {
			/*if(unlikely(remainingOutput + length > outputSize -
			LASTLITERALS)){ if(!partialDecoding){ goto
			_output_error;
				}
				length = min(length, (size_t)(outputSize -
			remainingOutput));
			}

			if(length <= (size_t)(lowPrefix - bvec_iter_addr(dst,
			&matchIter))){

			}
			реализация для работы со сторонними словарями*/
		}

		cpy = remainingOutput + length;

		assert(remainingOutput <= outputSize);
		if (partialDecoding &&
		    (cpy > outputSize - MATCH_SAFEGUARD_DISTANCE)) {
			size_t const mlen = min(
				length, (size_t)(outputSize - remainingOutput));
			size_t copyEnd = remainingOutput + mlen;

			if (mlen > offset) {
				LZ4E_wildCopy(dst, dst, *dstIter, matchIter,
					      copyEnd - remainingOutput);
			} else {
				LZ4E_memcpy(dst, dst, *dstIter, matchIter,
					    mlen);
			}
			remainingOutput = copyEnd;
			if (remainingOutput == outputSize) {
				break;
			}
			continue;
		}

		if (unlikely(offset < 8)) {
			LZ4E_copy8(dst, dst, *dstIter, matchIter);
			LZ4E_copy8(dst, dst, *dstIter, matchIter);
			LZ4E_copy8(dst, dst, *dstIter, matchIter);
			LZ4E_copy8(dst, dst, *dstIter, matchIter);
			bvec_iter_advance(dst, &matchIter, inc32table[offset]);
			LZ4E_memcpy(dst, dst, *dstIter, matchIter, 4);
			LZ4E_iter_rollback(dst, &matchIter, dec64table[offset]);
		} else {
			LZ4E_copy8(dst, dst, *dstIter, matchIter);
		}
		remainingOutput += 8;

		if (unlikely(cpy > outputSize - MATCH_SAFEGUARD_DISTANCE)) {
			size_t oCopyLimit = outputSize - (WILDCOPYLENGTH - 1);

			if (cpy > outputSize - LASTLITERALS) {
				goto _output_error;
			}

			if (remainingOutput < oCopyLimit) {
				LZ4E_wildCopy(dst, dst, *dstIter, matchIter,
					      oCopyLimit);
				remainingOutput = oCopyLimit;
			}
			LZ4E_memcpy(dst, dst, *dstIter, matchIter,
				    cpy - remainingOutput);
		} else {
			LZ4E_copy8(dst, dst, *dstIter, matchIter);
			if (length > 16) {
				LZ4E_wildCopy(dst, dst, *dstIter, matchIter,
					      length);
			}
		}
		remainingOutput = cpy;

		if (endOnInput) {
			return (int)remainingOutput;
		} else {
			return (int)remainingInput;
		}
	}
_output_error:
	return (int)(-remainingInput) - 1;
}

int LZ4E_decompress_safe(const struct bio_vec *src, struct bio_vec *dst,
			 struct bvec_iter *srcIter, struct bvec_iter *dstIter)
{
	struct bio_vec cur_dst_seg = bvec_iter_bvec(dst, *dstIter);
	BYTE *dest_start = (BYTE *)page_address(cur_dst_seg.bv_page) +
			   cur_dst_seg.bv_offset;
	BYTE *current_dest = dest_start + dstIter->bi_bvec_done;
	return LZ4E_decompress_generic_sg(src, dst, srcIter, dstIter,
					  endOnInputSize, decode_full_block,
					  noDict, current_dest, NULL, 0);
}
