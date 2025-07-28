#include <asm-generic/errno-base.h>
#include <asm-generic/int-ll64.h>
#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/stat.h>
#include <linux/stddef.h>
#include <linux/sysfs.h>
#include <linux/bio.h>
#include <linux/mm.h>

#include "include/module/lz4e_req.h"
#include "include/module/lz4e_under_dev.h"

static void * src_alloc(struct bio *bio1) {
    void* src = kzalloc(bio1->bi_iter.bi_size, GFP_NOIO);

    if (!src) {
        pr_err("failed to allocate buffer (src)\n");
    }
    return src;
}

static struct bio *bio_read_alloc(struct bio *bio) {

    lz4e_under_dev *under_dev = lz4e_under_dev_alloc();
    return lz4e_alloc_new_bio(bio, under_dev);
}

static int count_of_pages(struct bio *bio) {
    return DIV_ROUND_UP(bio->bi_iter.bi_size, PAGE_SIZE);
}

static void add_buffer_to_bio(void *src, int size_of_data, struct bio *bio2) {
    size_t offset = 0;
    int cnt_of_pages = count_of_pages(bio2);
    int num_of_page = 0;

    while (offset < size_of_data && num_of_page < cnt_of_pages) {
        int len;

        if (num_of_page == 0) {
            len = PAGE_SIZE - ((unsigned long)((char *)src + offset) % PAGE_SIZE);
        } else if (num_of_page == cnt_of_pages - 1) {
            len = size_of_data - offset;
        } else {
            len = PAGE_SIZE;
        }
        struct page *page_ptr = virt_to_page(src + offset);

        if (bio_add_page(bio2, page_ptr, len, offset % PAGE_SIZE) != len) {
            pr_err("failed to add page number %d\n", num_of_page + 1);
            bio_put(bio2);

            return;
        }

        num_of_page++;
        offset += len;
    }
}

static void* dst_alloc(struct bio *bio1) {
    void *dst = kzalloc(LZ4_compressBound(bio1->bi_iter.bi_size), GFP_NOIO);
    if (!dst) {
        pr_err("failed to allocate buffer (dst)\n");
    }
    return dst;
}


//need to add implementation of src filling and LZ4E_COMPRESS LZ4E_DECOMPRESS
static void new_bi_end_io(struct bio *bio1, struct bio *bio2){
    void *src = src_alloc(bio1);
    LZ4E_COMPRESS(src, dst);
}

void req_submit(struct bio bio2){

    bio2 -> bi_end_io = new_bi_end_io;
    submit_bio_noacct(bio2);
}



