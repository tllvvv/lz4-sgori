# API

This page describes the interface of extended LZ4 implementation for the Linux Kernel and differentiates it from the standard one.

## Standard LZ4

The standard implementation of LZ4 in the Linux Kernel only handles contiguous buffers. For handling fragmented data there are several workarounds:
1) Copying the data into a preallocated contiguous buffer.

    This would require allocating a big enough chunk of memory, which could cause issues for large data.
    Although this method is easy to implement, allocation followed by copying would take an effect on performance and memory consumption.

2) Running LZ4 for each of contiguous blocks.

    When compressing a single block, we usually want to access as much of preceding data as possible to find matches.
    In case of fragmented data we would use [LZ4 Streaming API](https://github.com/lz4/lz4/blob/dev/examples/streaming_api_basics.md).
    The limitation here is that we are only able to use the previous block as a dictionary, and that can decrease the compression ratio.

Described downsides become significant for I/O operations in particular, in which LZ4 is often used for its speed.

Signature of the most commonly used [LZ4 function for compression](https://elixir.bootlin.com/linux/v6.16.9/source/include/linux/lz4.h#L175) is as follows:
```c
/*
 * src: pointer to the start of source buffer
 * dst: pointer to the start of destination buffer
 * inputSize: number of bytes from 'src' to compress
 * maxOutputSize: size of 'dst' in bytes
 * wrkmem: pointer to the allocated working memory
 * returns: number of bytes written to 'dst', or 0 if compression fails
 */
int LZ4_compress_default(const char *src, char *dst, int inputSize,
		int maxOutputSize, void *wrkmem);
```

Compression requires roughly 16 KB of additional memory (`wrkmem`), as provided by macro
[`LZ4_MEM_COMPRESS`](https://elixir.bootlin.com/linux/v6.16.9/source/include/linux/lz4.h#L156).
Size limit for compressed data can be found from the input size using macro
[`LZ4_COMPRESSBOUND`](https://elixir.bootlin.com/linux/v6.16.9/source/include/linux/lz4.h#L61).

## Extended LZ4

This repo provides an implementation of LZ4 compression for the Linux Kernel, extended for managing scatter-gather buffers.
This means that instead of only handling contiguous buffers, this implementation rather works with sequences of
contiguous segments that can be arbitrarily located. In the Linux Kernel, a single segment as such is represented with
[`struct bio_vec`](https://elixir.bootlin.com/linux/v6.16.9/source/include/linux/bvec.h#L19):
```c
struct bio_vec {
	struct page  *bv_page;	 /* the first of physical pages on which this buffer resides */
	unsigned int  bv_len;	 /* length (in bytes) of this buffer */
	unsigned int  bv_offset; /* offset (in bytes) of this buffer within the page */
};
```

So, a scatter-gather buffer can be represented as an array of such structures.
A single `bio_vec` may not necessarily lie on a single page, but rather on several
pages that are contiguous: in that case it is called a [multi-page bvec](https://lwn.net/Articles/755629/).
`struct bio_vec` is not particularly useful on its own and is rather a part of a bigger structure called
[`bio`](https://elixir.bootlin.com/linux/v6.16.9/source/include/linux/blk_types.h#L214).
In the Linux Kernel it is used to represent I/O within the block layer:
```c
struct bio {
	struct bio_vec		*bi_io_vec;	/* the actual list of bio_vec's */
	unsigned short		 bi_vcnt;	/* number of bio_vec's */
	blk_opf_t			 bi_opf;	/* request operation and flags */
	struct block_device *bi_bdev;	/* associated block device */
	struct bvec_iter	 bi_iter;	/* iterator through bio_vec's */
	...
};
```

An other significant part of `struct bio` is an iterator, represented with
[`struct bvec_iter`](https://elixir.bootlin.com/linux/v6.16.9/source/include/linux/bvec.h#L80).
This structure is helpful for iterating through scatter-gather buffers to perform the actual work,
and is also used to store info about I/O completion, such as remaining size:
```c
struct bvec_iter {
	unsigned int bi_size;	   /* residual I/O size in bytes */
	unsigned int bi_idx;	   /* current index into bio_vec array */
	unsigned int bi_bvec_done; /* number of bytes completed in current bio_vec */
	...
};
```

Finally, signature of the modified LZ4 compression function is as follows:
```c
/*
 * src: source buffer as a list of bio_vec's
 * dst: destination buffer as a list of bio_vec's
 * srcIter: iterator into 'src' at the start of data to compress
 * dstIter: iterator into 'dst' starting from which the bytes are written
 * wrkmem: pointer to the allocated working memory
 * returns: number of bytes written to 'dst', or 0 if compression fails
 */
int LZ4E_compress_default(const struct bio_vec *src, struct bio_vec *dst,
		struct bvec_iter *srcIter, struct bvec_iter *dstIter, void *wrkmem);
```

Input and maximum output sizes here are discovered through `srcIter` and `dstIter` respectively.
For I/O, iterators can be created and modified before calling the function and are changed at
runtime to contain current position after the function is called.
Compression requires additional 1KB of working memory on top of existing 16KB, the exact size is in macro `LZ4E_MEM_COMPRESS`.

Described signatures and macros can be found at [lz4e.h](../lz4e/include/lz4e.h).
