# LZ4 Scatter-Gather (SG) Buffers Support in Linux Kernel block layer

This project extends the LZ4 compression algorithm implementation in the Linux kernel to support SG-buffers (based on `struct bio_vec`), 
eliminating the need for additional data copying.

## Key Features

- Kernel-space implementation of LZ4 with SG buffers support
- Block device mapper for tests
- Test environment for validation
- API extantion in `include/lz4e.h`

## Installation

1. Build the module:
   ```bash
   make build
   ```
2. Insert the module into the kernel:
   ```bash
   sudo insmod lz4e.ko
   ```

## Block-dev usage
1. Create a compressed block device:
   ```bash
   echo -n "<path_to_underlying_device>" > /sys/module/lz4e/parameters/mapper
   ```

2. Remove the compressed block device:
   ```bash
   echo -n "unmap" > /sys/module/lz4e/parameters/unmapper
   ```

## Testing
1. Run the complete test suite with:
   ```bash
   test/test_all.sh
   ```

