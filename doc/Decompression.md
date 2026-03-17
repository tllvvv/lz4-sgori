# Decompression

This page describes the process of forming an [LZ4 block](BlockFormat.md).

## Standard LZ4

The standard LZ4 decompression algorithm is based on LZ77 and reads tokens sequentially, uses [pointer arithmetic](https://www.gnu.org/software/c-intro-and-ref/manual/html_node/Pointer-Arithmetic.html) to iterate through source and destination buffers, copying literal data and referencing previously decompressed data via offsets.

### The algorithm

The main loop can be split into the following steps:

1) read token byte and extract literal length from first 4 bits;
2) copy literals from source to destination buffer;
3) read offset value and extract match length from second 4 bits;
4) copy match from destination buffer using offset;
5) check if source buffer is exhausted; if not, go to step 1;


This cycle breaks when the source buffer end is reached during token reading.
In that case, all data has been successfully decompressed to the destination buffer.

### Implementation details

Variable-length encoding is employed when literal length or match length values equal 15, requiring the decoder to sequentially read additional bytes from the source buffer and accumulate them until a byte value less than 255 is encountered. The offset parameter is decoded as a 2-byte little-endian integer, representing the backward distance from the current destination buffer position to the match location.

In cases where the offset magnitude is smaller than the match length, the copy operation references data that is concurrently being written to the destination buffer, necessitating byte-by-byte copying semantics rather than block memory operations to preserve data integrity.

Buffer boundary conditions must be enforced to ensure that literal and match copy operations remain within the destination buffer capacity, and that token and offset reads do not exceed the source buffer boundaries.

Decompression terminates upon complete consumption of the source buffer during token reading, signifying successful processing of all encoded blocks.

## Extended LZ4

While the essence of the algorithm stays the same, our modification replaces all uses of pointer arithmetic
with [advancing](https://elixir.bootlin.com/linux/v6.16.9/source/include/linux/bvec.h#L143) of iterators
(see `struct bvec_iter` in [API](API.md)). For calculating a length of literal sequence or a match,
we keep track of relative iterator position, which can also be obtained by `bi_size` manipulation.
For instance, here are the helper functions for advance iterator:
```c
static FORCE_INLINE void LZ4E_iter_advance1(const struct bio_vec *bvecs,
		struct bvec_iter *iter)
{
	unsigned idx = iter->bi_idx;
	unsigned done = iter->bi_bvec_done;

	BUG_ON(iter->bi_size == 0);

	done++;

	if (done == bvecs[idx].bv_len) {
		idx++;
		done = 0;
	}

	iter->bi_idx = idx;
	iter->bi_bvec_done = done;
	iter->bi_size--;
}

static FORCE_INLINE void LZ4E_advance1(
	const struct bio_vec *bvecs,
	struct bvec_iter *iter,
	U32 *pos)
{
	LZ4E_iter_advance1(bvecs, iter);
	(*pos)++;
}
```
Then, all memory reading and writing was rewritten for handling scatter-gather buffers using Linux Kernel's
[page memcpy](https://elixir.bootlin.com/linux/v6.16.9/source/include/linux/highmem.h#L444)
and [in-flight bvec building](https://elixir.bootlin.com/linux/v6.16.9/source/include/linux/bvec.h#L136) helpers.
Each of implemented helper functions accept a buffer as array of bvecs and the start iterator. For example,
here are the helpers for writing to scatter-gather buffer from a contiguous one:

```c
static FORCE_INLINE void LZ4E_memcpy_from_bvec(char *to,
		const struct bio_vec *from, const size_t len)
{
	BUG_ON(len > from->bv_len || from->bv_offset + len > PAGE_SIZE);
	memcpy_from_page(to, from->bv_page, from->bv_offset, len);
}

static FORCE_INLINE void LZ4E_memcpy_from_sg(char *to, const struct bio_vec *from,
		struct bvec_iter iter, size_t len)
{
	struct bio_vec curBvec;
	size_t toRead;

	BUG_ON(len > iter.bi_size);

	while (len) {
		curBvec = bvec_iter_bvec(from, iter);
		toRead = min_t(size_t, len, curBvec.bv_len);

		LZ4E_memcpy_from_bvec(to, &curBvec, toRead);
		bvec_iter_advance_single(from, &iter, (unsigned)toRead);
		to += toRead;
		len -= toRead;
	}
}
```


