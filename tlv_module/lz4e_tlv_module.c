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


//надо решить проблему с тасканием за собой bio1 и bio2

static void src_alloc(struct bio *bio1, void **src){
    *src = kzalloc(bio1->bi_iter.bi_size, GFP_KERNEL);

    if(!(*src)){
        printk(KERN_ERR "Failed to allocate buffer (src)\n");
    }
}

static struct bio *bio_read_alloc(gfp_t gfp_mask, int cnt_pages, struct bio bio1){
    struct bio *bio2 = bio_alloc(gfp_mask, cnt_pages);

    if(!bio2){
        printk(KERN_ERR "Failed to allocate bio\n");
    }

    bio2.iter.bio_sector = bio1.iter.bio_sector;
    return bio2;
}

static int count_of_pages(struct bio *bio1){
    return (bio1->bi_iter.bi_size + PAGE_SIZE - 1) / PAGE_SIZE;
}

static void add_buffer_to_bio(void *src, int size_of_data, struct bio *bio2, int cnt_pages){
    size_t offset = 0;

    for(int i = 0; i < cnt_pages; i++){
        struct page *page = virt_to_page(src + offset);
        size_t len;

        if(i == cnt_pages - 1){
            len = size_of_data - offset;
        }else{
            len = PAGE_SIZE;
        }

        if(bio_add_page(bio2, page, len, offset % PAGE_SIZE) != len){
            printk(KERN_ERR "Failed to add page number %d\n", i + 1);
            bio_put(bio2);
            return;
        }

        offset += len;
    }
}

static void new_bi_end_io(struct bio *bio1, struct bio *bio2){ //вот в эту байду как раз таки нужно впихивать lz4
    void *data = bio2->bi_private;

    bio1->bi_status = bio2->bi_status;

    bio_endio(bio1);
    kfree(data);
    bio_put(bio2);
}

void req_submit(struct bio bio2){

    bio2 -> bi_end_io = new_bi_end_io;
    submit_bio_noacct(bio2);
}


