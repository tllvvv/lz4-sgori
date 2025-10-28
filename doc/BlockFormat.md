# Block Format

Detailed LZ4 block format description can be found at: <https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md>.

Essentially, a single LZ4 block consists of multiple sequences, which are comprised of the following parts:
- literals, or uncompressed data;
- length of contained literal sequence;
- offset with which a match was found;
- length of the found match.

![block-format](https://github.com/ItIsMrLaG/storage-svg/blob/main/compression/lz4_bformat.svg)

Semantically, in the process of decompression literals are supposed to be copied first using the known length,
and the match position is found by subtracting the offset from the current position after copying.
After that, the match is copied using the encoded match length and the next sequence can be processed.
Note that literal length can be zero, which means that sequence contains only a match.
An LZ4 block is terminated using the following conditions:
- last sequence contains only literals, meaning no match;
- last 5 bytes of input are always literals;
- the last match must start at least 12 bytes before the end of block.
