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
#include <linux/export.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/unaligned.h>

#include "include/lz4e.h"
#include "include/lz4e_defs.h"

/*-*****************************
 *	Decompression functions
 *******************************/

#define DEBUGLOG(l, ...) \
	{                \
	} /* disabled */

#ifndef assert
#define assert(condition) ((void)0)
#endif

static FORCE_INLINE int LZ4E_decompress_generic_sg(
	const struct bio_vec *src, struct bio_vec *dst,
	struct bvec_iter *srcIter, struct bvec_iter *dstIter,
	endCondition_directive endOnInput, earlyEnd_directive partialDecoding,
	dict_directive dict, const BYTE *const dictStart, const size_t dictSize)
{
	const U32 srcSize = (U32)srcIter->bi_size;
	U32 srcCur = 0;

	const U32 outputSize = (U32)dstIter->bi_size;
	U32 outputCur = 0;

	U32 cpy;
	struct bvec_iter copyIter;

	//	TODO:(tlv): dict impl
	//
	//	const BYTE * const dictEnd = (const BYTE *)dictStart + dictSize;
	static const unsigned int inc32table[8] = { 0, 1, 2, 1, 0, 4, 4, 4 };
	static const int dec64table[8] = { 0, 0, 0, -1, -4, 1, 2, 3 };
	//
	//	const int safeDecode = (endOnInput == endOnInputSize);
	//	const int checkOffset = ((safeDecode) && (dictSize < (int)(64 *
	// KB)));
	//

	/* Set up the "end" pointers for the shortcut. */
	const size_t shortiend =
		srcSize - (endOnInput ? 14 : 8) /*maxLL*/ - 2 /*offset*/;
	const size_t shortoend =
		outputSize - (endOnInput ? 14 : 8) /*maxxLL*/ - 18 /*maxML*/;

	/* Empty output buffer */
	const BYTE ip = LZ4E_read8(src, *srcIter);

	if ((endOnInput) && (unlikely(outputSize == 0))) {
		return ((srcSize == 1) && (ip == 0)) ? 0 : -1;
	}
	if ((!endOnInput) && (unlikely(outputSize == 0))) {
		return (ip == 0 ? 1 : -1);
	}
	if ((endOnInput) && unlikely(srcSize == 0)) {
		return -1;
	}

	while (1) {
		U32 length;
		size_t offset;
		U32 match;
		struct bvec_iter matchIter;

		/* get literal length */
		unsigned int const token = LZ4E_read8(src, *srcIter);
		LZ4E_advance1(src, srcIter, &srcCur);
		length = token >> ML_BITS;

		/*
		 * A two-stage shortcut for the most common case:
		 * 1) If the literal length is 0..14, and there is enough
		 * space, enter the shortcut and copy 16 bytes on behalf
		 * of the literals (in the fast mode, only 8 bytes can be
		 * safely copied this way).
		 * 2) Further if the match length is 4..18, copy 18 bytes
		 * in a similar manner; but we ensure that there's enough
		 * space in the output for those 18 bytes earlier, upon
		 * entering the shortcut (in other words, there is a
		 * combined check for both stages).
		 *
		 * The & in the likely() below is intentionally not && so that
		 * some compilers can produce better parallelized runtime code
		 */

		if ((endOnInput ? length != RUN_MASK : length <= 8)
		    /*
		     * strictly "less than" on input, to re-enter
		     * the loop with at least one byte
		     */
		    && likely((endOnInput ? srcCur < shortiend : 1) &
			      (outputCur <= shortoend))) {
			/* Copy the literals */
			LZ4E_memcpy(dst, src, *dstIter, *srcIter,
				    (endOnInput ? 16 : 8));
			LZ4E_advance(dst, dstIter, &outputCur, length);
			LZ4E_advance(src, srcIter, &srcCur, length);
			/*
			 * The second stage:
			 * prepare for match copying, decode full info.
			 * If it doesn't work out, the info won't be wasted.
			 */
			length = token & ML_MASK; /* match length */
			offset = LZ4E_readLE16(src, *srcIter);
			LZ4E_advance(src, srcIter, &srcCur, 2);

			match = outputCur;
			matchIter = *dstIter;
			for (int i = 0; i < offset; i++) {
				LZ4E_rollback1(dst, &matchIter, &match);
			}

			// TODO:(tlv): dict impl
			//
			// if ((length != ML_MASK) &&
			//	(offset >= 8) &&
			//	(dict == withPrefix64k || match >= lowPrefix)) {
			//	/* Copy the match. */
			//	LZ4_memcpy(op + 0, match + 0, 8);
			//	LZ4_memcpy(op + 8, match + 8, 8);
			//	LZ4_memcpy(op + 16, match + 16, 2);
			//	op += length + MINMATCH;
			//	/* Both stages worked, load the next token. */
			//	continue;
			// }
			//

			/*
			 * The second stage didn't work out, but the info
			 * is ready. Propel it right to the point of match
			 * copying.
			 */
			goto _copy_match;
		}

		/* decode literal length */
		if (length == RUN_MASK) {
			unsigned int s;

			if (unlikely(endOnInput ? srcCur >= srcSize - RUN_MASK :
						  0)) {
				/* overflow detection */
				goto _output_error;
			}
			do {
				s = LZ4E_read8(src, *srcIter);
				LZ4E_advance1(src, srcIter, &srcCur);
				length += s;
			} while (likely(endOnInput ?
						srcCur < srcSize - RUN_MASK :
						1) &
				 (s == 255));
		}

		/* copy literals */
		cpy = outputCur;
		copyIter = *dstIter;
		LZ4E_advance(dst, &copyIter, &cpy, length);

		LZ4_STATIC_ASSERT(MFLIMIT >= WILDCOPYLENGTH);

		if (((endOnInput) &&
		     ((cpy > outputSize - MFLIMIT) ||
		      (srcCur + length > srcSize - (2 + 1 + LASTLITERALS)))) ||
		    ((!endOnInput) && (cpy > outputSize - WILDCOPYLENGTH))) {
			if (partialDecoding) {
				if (cpy > outputSize) {
					/*
					 * Partial decoding :
					 * stop in the middle of literal segment
					 */
					cpy = outputSize;
					length = outputSize - outputCur;
				}
				if ((endOnInput) &&
				    (srcCur + length > srcSize)) {
					/*
					 * Error :
					 * read attempt beyond
					 * end of input buffer
					 */
					goto _output_error;
				}
			} else {
				if ((!endOnInput) && (cpy != outputSize)) {
					/*
					 * Error :
					 * block decoding must
					 * stop exactly there
					 */
					goto _output_error;
				}
				if ((endOnInput) &&
				    ((srcCur + length != srcSize) ||
				     (cpy > outputSize))) {
					/*
					 * Error :
					 * input must be consumed
					 */
					goto _output_error;
				}
			}

			/*
			 * supports overlapping memory regions; only matters
			 * for in-place decompression scenarios
			 */
			LZ4E_memcpy(dst, src, *dstIter, *srcIter, length);
			LZ4E_advance(src, srcIter, &srcCur, length);
			LZ4E_advance(dst, dstIter, &outputCur, length);

			/* Necessarily EOF when !partialDecoding.
			 * When partialDecoding, it is EOF if we've either
			 * filled the output buffer or
			 * can't proceed with reading an offset for following
			 * match.
			 */
			if (!partialDecoding || (cpy == outputSize) ||
			    (srcCur >= (srcSize - 2))) {
				break;
			}
		} else {
			/* may overwrite up to WILDCOPYLENGTH beyond cpy */
			LZ4E_wildCopy(dst, src, *dstIter, *srcIter, length);
			LZ4E_advance(src, srcIter, &srcCur, length);
			outputCur = cpy;
			*dstIter = copyIter;
		}

		/* get offset */
		offset = LZ4E_readLE16(src, *srcIter);
		LZ4E_advance(src, srcIter, &srcCur, 2);

		match = outputCur;
		matchIter = *dstIter;
		for (int i = 0; i < offset; i++) {
			LZ4E_rollback1(dst, &matchIter, &match);
		}

		/* get matchlength */
		length = token & ML_MASK;

	_copy_match:
		// TODO(tlv) dict
		//
		// if ((checkOffset) && (unlikely(match + dictSize <
		// lowPrefix))) {
		//	/* Error : offset outside buffers */
		//	goto _output_error;
		// }
		//

		if (!partialDecoding) {
			LZ4E_write32(dst, (U32)offset, *dstIter);
		}

		if (length == ML_MASK) {
			unsigned int s;

			do {
				s = LZ4E_read8(src, *srcIter);
				LZ4E_advance1(src, srcIter, &srcCur);

				if ((endOnInput) &&
				    (srcCur > srcSize - LASTLITERALS)) {
					goto _output_error;
				}
				length += s;
			} while (s == 255);
		}

		length += MINMATCH;

		// if ((dict == usingExtDict) && (match < lowPrefix)) {
		//	if (unlikely(op + length > oend - LASTLITERALS)) {
		//		/* doesn't respect parsing restriction */
		//		if (!partialDecoding)
		//			goto _output_error;
		//		length = min(length, (size_t)(oend - op));
		//	}
		//	if (length <= (size_t)(lowPrefix - match)) {
		//		/*
		//		 * match fits entirely within external
		//		 * dictionary : just copy
		//		 */
		//		memmove(op, dictEnd - (lowPrefix - match),
		//			length);
		//		op += length;
		//	} else {
		//		/*
		//		 * match stretches into both external
		//		 * dictionary and current block
		//		 */
		//		size_t const copySize = (size_t)(lowPrefix -
		// match); 		size_t const restSize = length -
		// copySize;
		//
		//		LZ4_memcpy(op, dictEnd - copySize, copySize);
		//		op += copySize;
		//		if (restSize > (size_t)(op - lowPrefix)) {
		//			/* overlap copy */
		//			BYTE * const endOfMatch = op + restSize;
		//			const BYTE *copyFrom = lowPrefix;
		//
		//			while (op < endOfMatch)
		//				*op++ = *copyFrom++;
		//		} else {
		//			LZ4_memcpy(op, lowPrefix, restSize);
		//			op += restSize;
		//		}
		//	}
		//	continue;
		// }
		//

		/* copy match within block */
		cpy = outputCur;
		copyIter = *dstIter;
		LZ4E_advance(dst, &copyIter, &cpy, length);

		/*
		 * partialDecoding :
		 * may not respect endBlock parsing restrictions
		 */

		if (partialDecoding &&
		    (cpy > outputSize - MATCH_SAFEGUARD_DISTANCE)) {
			size_t const mlen =
				min(length, (outputSize - outputCur));
			const size_t matchEnd = match + mlen;
			const size_t CopyEnd = outputCur + mlen;

			struct bvec_iter copyEndIter = *dstIter;
			bvec_iter_advance(dst, &copyEndIter, mlen);

			if (matchEnd > outputCur) {
				/* overlap copy */
				while (outputCur < CopyEnd) {
					LZ4E_copy8(dst, dst, *dstIter,
						   matchIter);
					LZ4E_advance1(dst, dstIter, &outputCur);
					LZ4E_advance1(dst, &matchIter, &match);
				}
			} else {
				LZ4E_memcpy(dst, dst, *dstIter, matchIter,
					    mlen);
			}
			outputCur = CopyEnd;
			*dstIter = copyEndIter;

			if (outputCur == outputSize) {
				break;
			}
			continue;
		}

		if (unlikely(offset < 8)) {
			LZ4E_copy32(dst, dst, *dstIter, matchIter);
			LZ4E_advance(dst, dstIter, &outputCur, 4);

			LZ4E_advance(dst, &matchIter, &match,
				     inc32table[offset]);

			LZ4E_memcpy(dst, dst, *dstIter, matchIter, 4);

			for (int i = 0; i < dec64table[offset]; i++) {
				LZ4E_rollback1(dst, &matchIter, &match);
			}
			for (int i = 0; i < 4; i++) {
				LZ4E_rollback1(dst, dstIter, &outputCur);
			}
		} else {
			LZ4E_copy64(dst, dst, *dstIter, matchIter);
			LZ4E_advance(dst, &matchIter, &match, 8);
		}

		LZ4E_advance(dst, dstIter, &outputCur, 8);

		if (unlikely(cpy > outputSize - MATCH_SAFEGUARD_DISTANCE)) {
			const size_t oCopyLimit =
				outputSize - (WILDCOPYLENGTH - 1);
			struct bvec_iter oCopyLimitIter = *dstIter;
			bvec_iter_advance(dst, &oCopyLimitIter,
					  oCopyLimit - outputCur);

			if (cpy > outputSize - LASTLITERALS) {
				/*
				 * Error : last LASTLITERALS bytes
				 * must be literals(uncompressed)
				 */
				goto _output_error;
			}

			if (outputCur < oCopyLimit) {
				LZ4E_wildCopy(dst, dst, *dstIter, matchIter,
					      oCopyLimit - outputCur);
				LZ4E_advance(dst, &matchIter, &match,
					     oCopyLimit - outputCur);

				outputCur = oCopyLimit;
				*dstIter = oCopyLimitIter;
			}
			while (outputCur < cpy) {
				LZ4E_copy8(dst, dst, *dstIter, matchIter);
				LZ4E_advance1(dst, dstIter, &outputCur);
				LZ4E_advance1(dst, &matchIter, &match);
			}
		} else {
			LZ4E_copy64(dst, dst, *dstIter, matchIter);

			if (length > 16) {
				LZ4E_advance(dst, dstIter, &outputCur, 8);
				LZ4E_advance(dst, &matchIter, &match, 8);

				LZ4E_wildCopy(dst, dst, *dstIter, matchIter,
					      cpy - outputCur);

				for (int i = 0; i < 8; i++) {
					LZ4E_rollback1(dst, dstIter,
						       &outputCur);
					LZ4E_rollback1(dst, &matchIter, &match);
				}
			}
		}
		outputCur = cpy;
		*dstIter = copyIter; /* wildcopy correction */
	}

	if (endOnInput) {
		/* Nb of output bytes decoded */
		return (int)outputCur;
	} else {
		/* Nb of input bytes read */
		return (int)srcCur;
	}
_output_error:
	return (int)(-(srcCur)) - 1;
}

int LZ4E_decompress_safe(const struct bio_vec *src, struct bio_vec *dst,
			 struct bvec_iter *srcIter, struct bvec_iter *dstIter)
{
	return LZ4E_decompress_generic_sg(src, dst, srcIter, dstIter,
					  endOnInputSize, decode_full_block,
					  noDict, NULL, 0);
}
EXPORT_SYMBOL(LZ4E_decompress_safe);

MODULE_AUTHOR("");
MODULE_DESCRIPTION("LZ4 decompression for scatter-gather buffers");
MODULE_LICENSE("GPL");
