# Compression

This page describes the process of forming an [LZ4 block](BlockFormat.md).

## Standard LZ4

The standard LZ4 compression algorithm is based on LZ77 and uses hash-table for finding matches within a sliding window.
Since the source and destination buffers are contiguous,
[pointer arithmetic](https://www.gnu.org/software/c-intro-and-ref/manual/html_node/Pointer-Arithmetic.html)
is used for iteration and data access.

### The algorithm

As a preparation, the first byte is processed by adding a corresponding position to the hash table.
After that, an anchor is set as the position of the first byte in the source buffer that is not yet encoded.

After that, the main loop can be split into the following steps:
1) iterate through source buffer until found a match within a fixed-size window using a hash table;
2) rollback both source and match positions while possible to get the maximum match;
3) calculate literal length as distance from anchor to the source position, write it to token and following bytes;
4) copy bytes to destination buffer, starting from anchor up to the source position;
5) write offset as distance between source and match positions;
6) find length of the match, write it to token and following bytes after offset;
7) move source position to first byte after match, update anchor;
8) check source position for a match, go to step 5 if found, otherwise go to step 1.

This cycle breaks when the block end conditions are met during match searching.
In that case, last literals from anchor to source buffer end are copied to the destination.

![lz4-comp](https://github.com/bygu4/storage-svg/blob/main/compression/lz4_comp_wb.svg)

### Implementation details

In the Linux Kernel implementation of LZ4, hash table is a 16KB array that uses
hashed data as indices and stores positions relatively to the start of the source buffer.
To find a match, several bytes are read from buffer and then hashed to get a supposed match position from the hash table.
4 bytes are hashed on 32-bit architectures, while 8 bytes can be used within 64-bit systems.

Sizes of addresses stored in the hash table differ depending on the size of input: 4 bytes by default and 2 bytes if data fits within
[64 KB limit](https://elixir.bootlin.com/linux/v6.16.9/source/lib/lz4/lz4_compress.c#L42).
That way, hash table can store 8192 positions instead of 4096, which would reduce the number of collisions.

The sliding window has size of [64KB](https://elixir.bootlin.com/linux/v6.16.9/source/lib/lz4/lz4defs.h#L96),
so that the offset can be encoded with 2 bytes.
In the default compression function, initial step size during match searching is equal to 1 byte and gets incremented every 64 steps.

## Extended LZ4

While the essence of the algorithm stays the same, our modification replaces all uses of pointer arithmetic
with [advancing](https://elixir.bootlin.com/linux/v6.16.9/source/include/linux/bvec.h#L143) of iterators
(see `struct bvec_iter` in [API](API.md)). For calculating a length of literal sequence or a match,
we keep track of relative iterator position, which can also be obtained by `bi_size` manipulation.
For instance, here are the helper functions for rolling an iterator back:
```c
static FORCE_INLINE void LZ4E_iter_rollback1(const struct bio_vec *bvecs,
		struct bvec_iter *iter)
{
	unsigned idx = iter->bi_idx;
	unsigned done = iter->bi_bvec_done;

	if (done == 0) {
		BUG_ON(idx == 0);

		idx--;
		done = bvecs[idx].bv_len;
	}

	done--;

	iter->bi_idx = idx;
	iter->bi_bvec_done = done;
	iter->bi_size++;
}

static FORCE_INLINE void LZ4E_rollback1(const struct bio_vec *bvecs,
	struct bvec_iter *iter, U32 *pos)
{
	LZ4E_iter_rollback1(bvecs, iter);
	(*pos)--;
}
```

Then, all memory reading and writing was rewritten for handling scatter-gather buffers using Linux Kernel's
[page memcpy](https://elixir.bootlin.com/linux/v6.16.9/source/include/linux/highmem.h#L444)
and [in-flight bvec building](https://elixir.bootlin.com/linux/v6.16.9/source/include/linux/bvec.h#L136) helpers.
Each of implemented helper functions accept a buffer as array of bvecs and the start iterator. For example,
here are the helpers for writing to scatter-gather buffer from a contiguous one:
```c
static FORCE_INLINE void LZ4E_memcpy_to_bvec(struct bio_vec *to,
		const char *from, const size_t len)
{
	BUG_ON(len > to->bv_len || to->bv_offset + len > PAGE_SIZE);
	memcpy_to_page(to->bv_page, to->bv_offset, from, len);
}

static FORCE_INLINE void LZ4E_memcpy_to_sg(struct bio_vec *to, const char *from,
		struct bvec_iter iter, size_t len)
{
	struct bio_vec curBvec;
	size_t toWrite;

	BUG_ON(len > iter.bi_size);

	while (len) {
		curBvec = bvec_iter_bvec(to, iter);
		toWrite = min_t(size_t, len, curBvec.bv_len);

		LZ4E_memcpy_to_bvec(&curBvec, from, toWrite);
		bvec_iter_advance_single(to, &iter, (unsigned)toWrite);
		from += toWrite;
		len -= toWrite;
	}
}
```

Hash table also had to be adjusted as we want to obtain iterators for match positions.
For that, separate address types were added, as well as macros for transforming these addresses into iterators and the other way around.
Such addresses consist of:
- index inside the list of bvecs, relatively to the start iterator;
- offset inside the according bvec.

However, this would not be enough for calculating `bi_size` when creating an iterator.
For that reason, we store remaining size of the source buffer at the start of each bvec in a separate array.
The number of bvecs that can be handled by compression is limited to 256, which is the
[maximum number of vectors](https://elixir.bootlin.com/linux/v6.16.9/source/include/linux/bio.h#L13) in a single block layer I/O.
So, this array makes up for additional 1KB (4 * 256) of working memory.
The `bi_size` table gets filled by iterating over vectors before start of the main loop.

As well as the original, our modification supports different hash table address sizes, of which there are 3 types:
1) by 2 bytes, maximum of 16 vectors by 4KB, which is the standard page size;
2) by 4 bytes, maximum of 256 vectors by 16MB;
3) by 8 bytes, maximum of 256 vectors by 4GB, which is more than the size limit for compression.

The used address type is decided at runtime, depending on the input size and layout, by iterating over bvecs while filling the
`bi_size` table.
