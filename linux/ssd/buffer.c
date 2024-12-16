#include "buffer.h"

static bcache_t buffer_cache = {
    .active_list = LIST_HEAD_INIT(buffer_cache.active_list),
    .inactive_list = LIST_HEAD_INIT(buffer_cache.inactive_list),
    .root = RB_ROOT,
    .lock = __SPIN_LOCK_UNLOCKED(buffer_cache.lock),
    .active_size = 0,
    .inactive_size = 0,
    .page_count = 0
};

static inline buf_rbh_t* get_rbh(struct rb_node* node) {
    return container_of(node, buf_rbh_t, node);
}

static inline buf_rbh_t* get_rbh_from_li(struct list_head* li) {
    return container_of(li, buf_rbh_t, li);
}


/**
 * @brief return the node with LARGEST lpa in @param root whose lpa <= @param lpa 
 * 
 */
static struct rb_node* rb_search(struct rb_root* root, uint64_t lpa) {
    struct rb_node* node = root->rb_node;
    struct rb_node* ret = NULL;
    while(node) {
        buf_rbh_t* rbh = get_rbh(node);
        if(lpa < rbh->lpa) {
            node = node->rb_left;
        } else if(lpa > rbh->lpa) {
            ret = node;
            node = node->rb_right;
        } else {
            return node;
        }
    }
    return ret;
}

// free a rb tree node
static void rb_erase_and_free(struct rb_node* node, struct rb_root* root) {
    if(NULL == root || NULL == node){
        printk(KERN_ERR "%s BUG: root or node is NULL", __func__);
        return;
    }
    rb_erase(node, root);
    buf_rbh_t* rbh = get_rbh(node);
    if(rbh->page) {
        kfree(rbh->page);
    }
    // delete the corresponding list item
    list_del(&(rbh->li));
    kfree(&(rbh->li));
    kfree(rbh);
}

/**
 * side effect: Insert the node into the rb tree and the corresponding list item into the inactive list 
 * @return buf_rbh_t* new allocated rbh. NULL if failed.
 */
buf_rbh_t* buf_alloc(uint64_t lpa, struct list_head* lh) {
    buf_rbh_t* rbh = kmalloc(sizeof(buf_rbh_t), GFP_KERNEL);
    if(NULL == rbh) {
        printk(KERN_ERR "%s BUG: kmalloc failed", __func__);
        return NULL;
    }
    rbh->lpa = lpa;
    rbh->page = kmalloc(PG_SIZE, GFP_KERNEL);
    if (!rbh->page) {
        printk(KERN_ERR "%s alloc_page failed", __func__);
        return NULL;
    }
    

    rbh->flags = BH_RBH_FLAG_INIT;

    rbh->hit_times = 0;
    // create new node and insert into inactive list starting with lh
    INIT_LIST_HEAD(&(rbh->li));
    list_add_tail(&(rbh->li), lh);
    return rbh;
}


static int64_t rb_tree_alloc_and_insert(struct rb_root* root, uint64_t lpa, struct rb_node** node, 
uint64_t size) {
    struct list_head* lh = &(buffer_cache.inactive_list);
    struct rb_node** new;
    struct rb_node* parent = NULL;
    int64_t ret = -1;

    if(NULL == root) return -1;

    new = &(root->rb_node);

    while(*new) {
        buf_rbh_t* rbh = get_rbh(*new);
        parent = *new;
        if(lpa < rbh->lpa) {
            new = &((*new)->rb_left);
        } else if(lpa > rbh->lpa) {
            new = &((*new)->rb_right);
        } else {
            printk(KERN_ERR "%s BUG: lpa already in rb tree", __func__);
            return -1;
        }
    }

    if(NULL == *new) {
        buf_rbh_t* rbh = buf_alloc(lpa, lh);
        // add it in the rb tree
        rb_link_node(&(rbh->node), parent, new);
        rb_insert_color(&(rbh->node), root);
        *node = &(rbh->node);
        
        ret = 0;
    } else {
        printk(KERN_ERR "%s BUG: rbh already exists", __func__);
    }
    return ret;
}



static int64_t sync_up(uint64_t lpa, uint64_t size, void* buf) {
    printk(KERN_ERR "%s: lpa %lu, size %lu", __func__, lpa, size);
    return nvme_issue(0, lpa / PG_SIZE, 1, buf);

}


/**
 * @brief used for 64 byte aligned issue
 * @note the reason why we need to use 64 byte issue here is that we already have one buffer
 * so we don't need read modify write.
 */
static int64_t sync_down_64(uint64_t lpa, uint64_t size, char* buf) {
    uint64_t C0 = lpa, C1 = lpa + size;
	uint64_t S0 = C0 / LOG_SIZE * LOG_SIZE; 
	uint64_t S2 = C1 / LOG_SIZE * LOG_SIZE, S3 = C1 / LOG_SIZE * LOG_SIZE + LOG_SIZE;
    uint64_t start, end;
    start = S0;
    end = C1 % LOG_SIZE ? S3 : S2;
    return byte_issue(1, start, end - start, buf + start % PG_SIZE);
}

static int64_t sync_down(uint64_t lpa, uint64_t size, char* buf) {
    return byte_issue(1, lpa, size, buf);
}


static void hit_cache(buf_rbh_t* rbh, uint64_t inc_size, uint64_t inc_hit_times) {
    // inc_size != 0 <= cache miss
    if(rbh->hit_times < LRU_TRANSFER_TIMES) buffer_cache.inactive_size += inc_size;
    else{
        buffer_cache.active_size += inc_size;
        if(inc_size) // sanity check
            printk(KERN_ERR "%s BUG: active size should not be increased", __func__);
    }

    if(rbh->hit_times < LRU_TRANSFER_TIMES && 
     rbh->hit_times + inc_hit_times >= LRU_TRANSFER_TIMES) {
        // move to active list
        list_del(&(rbh->li));
        list_add_tail(&(rbh->li), &(buffer_cache.active_list));
        buffer_cache.active_size += PG_SIZE;
        buffer_cache.inactive_size -= PG_SIZE;
    }
    rbh->hit_times += inc_hit_times;
}

static void move_to_inactive(void) {
    // move all pages in active list to inactive list
    struct list_head* lh = &(buffer_cache.active_list);
    struct list_head* pos, *q;
    uint64_t flags;
    spin_lock_irqsave(&buffer_cache.lock, flags);
    list_for_each_safe(pos, q, lh) {
        buf_rbh_t* rbh = get_rbh_from_li(pos);
        list_del(&(rbh->li));
        list_add_tail(&(rbh->li), &(buffer_cache.inactive_list));
        buffer_cache.active_size -= PG_SIZE;
        buffer_cache.inactive_size += PG_SIZE;

        // clear hit times
        rbh->hit_times = 0;
    }
    spin_unlock_irqrestore(&buffer_cache.lock, flags);
}

/**
 * @note this hasn't been added, but is tested.
 * @note caller needs to release lock before entering this function
 */
static void try_evict(void) {
    // go through inactive list and evict all pages
    struct list_head* lh = &(buffer_cache.inactive_list);
    struct list_head* pos, *q;
    uint64_t flags;
    spin_lock_irqsave(&buffer_cache.lock, flags);
    list_for_each_safe(pos, q, lh) {
        buf_rbh_t* rbh = get_rbh_from_li(pos);
        rb_erase_and_free(&(rbh->node), &(buffer_cache.root));
        buffer_cache.inactive_size -= PG_SIZE;
    }
    spin_unlock_irqrestore(&buffer_cache.lock, flags);
}


/**
 * @brief rd from @param st to @param st + @param size using buffer cache
 * @return int64_t number of pages found. -1 on failure.
 */
int64_t buf_rd(uint64_t st, uint64_t size, char* buf) {
    // note that in file_table.c, struct file is zeroed out when it is allocated

    int ret = 0;
    uint64_t flags;
    uint64_t i, off, end; 
    buf_rbh_t* rbh;
    struct rb_node* node;

    end = st + size;

    spin_lock_irqsave(&buffer_cache.lock, flags);

    off = 0;
    for(i = st; i < end; i = i / PG_SIZE * PG_SIZE + PG_SIZE) {
        uint64_t lpa = i / PG_SIZE * PG_SIZE;
        uint64_t l_padding, r_padding;

        l_padding = i % PG_SIZE;
        r_padding = (lpa + PG_SIZE) > end ? (PG_SIZE - end % PG_SIZE) : 0;

        rbh = NULL;

        // search in rb tree with key lpa
        node = rb_search(&(buffer_cache.root), lpa);
        if(NULL == node) {

            if(-1 == rb_tree_alloc_and_insert(&(buffer_cache.root), lpa, &node, PG_SIZE)) {
                printk(KERN_ERR "%s rb_tree_alloc_and_insert failed", __func__);
                spin_unlock_irqrestore(&buffer_cache.lock, flags);
                return -1;
            }
            buffer_cache.page_count++;
            rbh = get_rbh(node);
            hit_cache(rbh, PG_SIZE, 1);
            // for read, we must sync the page if it's a hole
            spin_unlock_irqrestore(&buffer_cache.lock, flags);
            if(0 > sync_up(rbh->lpa, PG_SIZE, rbh->page)) {
                printk(KERN_ERR "%s sync_rbh_rd failed", __func__);
                return -1;
            }
            spin_lock_irqsave(&buffer_cache.lock, flags);
        } else {
            rbh = get_rbh(node);
            hit_cache(rbh, 0, 1);
        }
        if(0 > memcpy(buf + off, 
        (char*)(rbh->page) + l_padding, PG_SIZE - l_padding - r_padding)) {
            printk(KERN_ERR "%s copy_to_user failed", __func__);
            spin_unlock_irqrestore(&buffer_cache.lock, flags);
            return -1;
        }
        off += PG_SIZE - l_padding - r_padding;
        ret++;
    }
    // printk(KERN_ERR "bytefs_pgcache_rd: %d pages found", buffer_cache.page_count);
    spin_unlock_irqrestore(&buffer_cache.lock, flags);
    return ret;
}
EXPORT_SYMBOL(buf_rd);


/**
 * @brief wr from @param st to @param st + @param size using buffer cache
 * @todo : result should be stored in somewhere pointed by parameter
 * @return int64_t 
 */
int64_t buf_wr(uint64_t st, uint64_t size, char* buf) {
    int ret = 0;
    uint64_t flags;
    uint64_t i, off, end; 
    buf_rbh_t* rbh;
    struct rb_node* node;

    end = st + size;

    spin_lock_irqsave(&buffer_cache.lock, flags);

    off = 0;
    for(i = st; i < end; i = i / PG_SIZE * PG_SIZE + PG_SIZE) {
        uint64_t lpa = i / PG_SIZE * PG_SIZE;
        uint64_t l_padding, r_padding;

        l_padding = i % PG_SIZE;
        r_padding = (lpa + PG_SIZE) > end ? (PG_SIZE - end % PG_SIZE) : 0;

        rbh = NULL;

        // search in rb tree with key lpa
        node = rb_search(&(buffer_cache.root), lpa);
        if(NULL == node) {

            if(-1 == rb_tree_alloc_and_insert(&(buffer_cache.root), lpa, &node, PG_SIZE)) {
                printk(KERN_ERR "%s rb_tree_alloc_and_insert failed", __func__);
                spin_unlock_irqrestore(&buffer_cache.lock, flags);
                return -1;
            }
            buffer_cache.page_count++;
            rbh = get_rbh(node);
            hit_cache(rbh, PG_SIZE, 1);
            // for read, we must sync the page if it's a hole
            if(0 > sync_up(rbh->lpa, PG_SIZE, rbh->page)) {
                printk(KERN_ERR "%s sync_rbh_rd failed", __func__);
                spin_unlock_irqrestore(&buffer_cache.lock, flags);
                return -1;
            }
        } else {
            rbh = get_rbh(node);
            hit_cache(rbh, 0, 1);
        }

        if(0 > memcpy((char*)(rbh->page) + l_padding, buf + off, 
        PG_SIZE - l_padding - r_padding)) {
            printk(KERN_ERR "%s copy_to_user failed", __func__);
            spin_unlock_irqrestore(&buffer_cache.lock, flags);
            return -1;
        }
        #if (BYTE_ISSUE_64_ALIGN == 1)
            if(0 > sync_down_64(i, PG_SIZE - l_padding - r_padding, rbh->page)) {
                printk(KERN_ERR "%s sync_rbh_wr failed", __func__);
            }
        #endif
        spin_unlock_irqrestore(&buffer_cache.lock, flags);
        spin_lock_irqsave(&buffer_cache.lock, flags);

        off += PG_SIZE - l_padding - r_padding;
        ret++;
    }
#if (BYTE_ISSUE_64_ALIGN == 0)
    // write through
    if(0 > sync_down(st, size, buf)) {
        printk(KERN_ERR "%s sync_rbh_wr failed", __func__);
    }
#endif
    spin_unlock_irqrestore(&buffer_cache.lock, flags);
    return ret;
}
EXPORT_SYMBOL(buf_wr);

/**
 * @brief test buffer read write
 * test 1 : wr & rd hit
 * test 2 : wr hit
 * test 3 : l_contig wr, rd hit
 * test 4 : r_contig wr, rd hit
 * test 5 : l_contig & r_contig wr, rd hit
 * test 6 : wr miss (no overlap)
 */
void test_buffer_rdwr(void) {
    static int tested = 0;
    uint8_t i;
    uint8_t buf[100], buf2[100], res_buf[4096], verify_buf[4096];
    
    if(tested) return;
    tested = 1;
    // first write from 0 to 100
    // print start test header
    printk(KERN_ERR "start buffer read write test 1");
    for(i = 0; i < 100; i++) {
        buf[i] = i;
    }
    buf_wr(0, 100, buf);
    // then read from 0 to 100
    buf_rd(0, 100, buf2);
    for(i = 0; i < 100; i++) {
        if(buf[i] != buf2[i]) {
            printk(KERN_ERR "buffer read write test failed");
            return;
        }
    }
    printk(KERN_ERR "buffer read write test 1 passed");

    // second write from 0 to 100
    // print start test header
    printk(KERN_ERR "start buffer read write test 2");
    for(i = 0; i < 100; i++) {
        buf[i] = i + 100; //  ( i + 100 ) % 256
    }
    buf_wr(0, 100, buf);
    // then read from 0 to 100
    buf_rd(0, 100, buf2);
    for(i = 0; i < 100; i++) {
        if(buf[i] != buf2[i]) {
            printk(KERN_ERR "buffer read write test failed");
            return;
        }
    }
    printk(KERN_ERR "buffer read write test 2 passed");

    // third write from 50 to 150

    // print start test header
    printk(KERN_ERR "start buffer read write test 3");
    for(i = 0; i < 100; i++) {
        buf[i] = i + 200;
    }
    buf_wr(50, 100, buf);
    // then read from 0 to 100
    buf_rd(0, 100, buf2);
    
    printk(KERN_ERR "checkpoint1");

    for(i = 0; i < 50; i++) {
        printk(KERN_ERR "buf2[%d] = %d", i, buf2[i]);
        if(buf2[i] != i + 100) {
            printk(KERN_ERR "buffer read write test failed");
            return;
        } else {
            printk(KERN_ERR "buf2[%d] = %d", i, buf2[i]);
        }
    }
    printk(KERN_ERR "checkpoint2");
    for(i = 50; i < 100; i++) {
        printk(KERN_ERR "buf2[%d] = %d", i, buf2[i]);
        if(buf2[i] != i + 150) {
            printk(KERN_ERR "buf2[%d] = %d failed at %s", i, buf2[i], __func__);
            return;
        } else {
            printk(KERN_ERR "buf2[%d] = %d", i, buf2[i]);
        }
    }
    printk(KERN_ERR "buffer read write test 3 passed");

    // fourth write from 4000 to 4100 then from 4100 to 4200
 
    // print start test header
    printk(KERN_ERR "start buffer read write test 4");

    for(i = 0; i < 100; i++) {
        buf[i] = i;
        res_buf[i] = i;
    }

    buf_wr(4000, 100, buf);

    for(i = 0; i < 100; i++) {
        buf[i] = i + 100;
        res_buf[i + 100] = i + 100;
    }

    buf_wr(4100, 100, buf);

    // then read from 4000 to 4200
    buf_rd(4000, 200, verify_buf);

    // verify
    for(i = 0; i < 200; i++) {
        printk(KERN_ERR "verify_buf[%d] = %d", i, verify_buf[i]);
        if(verify_buf[i] != res_buf[i]) {
            printk(KERN_ERR "buffer read write test failed");
            return;
        }
    }
    printk(KERN_ERR "buffer read write test 4 passed");
    try_evict();
    // first write from 0 to 100
    // print start test header
    printk(KERN_ERR "start buffer read write test 5");
    for(i = 0; i < 100; i++) {
        buf[i] = i;
    }
    buf_wr(0, 100, buf);
    // then read from 0 to 100
    buf_rd(0, 100, buf2);
    for(i = 0; i < 100; i++) {
        if(buf[i] != buf2[i]) {
            printk(KERN_ERR "buffer read write test failed");
            return;
        }
    }
    printk(KERN_ERR "buffer read write test 5 passed");

    // second write from 0 to 100
    // print start test header
    printk(KERN_ERR "start buffer read write test 6");
    for(i = 0; i < 100; i++) {
        buf[i] = i + 100; //  ( i + 100 ) % 256
    }
    buf_wr(0, 100, buf);
    // then read from 0 to 100
    buf_rd(0, 100, buf2);
    for(i = 0; i < 100; i++) {
        if(buf[i] != buf2[i]) {
            printk(KERN_ERR "buffer read write test failed");
            return;
        }
    }
    printk(KERN_ERR "buffer read write test 6 passed");

    // third write from 50 to 150

    // print start test header
    printk(KERN_ERR "start buffer read write test 7");
    for(i = 0; i < 100; i++) {
        buf[i] = i + 200;
    }
    buf_wr(50, 100, buf);
    // then read from 0 to 100
    buf_rd(0, 100, buf2);
    
    printk(KERN_ERR "checkpoint1");

    for(i = 0; i < 50; i++) {
        printk(KERN_ERR "buf2[%d] = %d", i, buf2[i]);
        if(buf2[i] != i + 100) {
            printk(KERN_ERR "buffer read write test failed");
            return;
        } else {
            printk(KERN_ERR "buf2[%d] = %d", i, buf2[i]);
        }
    }
    printk(KERN_ERR "checkpoint2");
    for(i = 50; i < 100; i++) {
        printk(KERN_ERR "buf2[%d] = %d", i, buf2[i]);
        if(buf2[i] != i + 150) {
            printk(KERN_ERR "buf2[%d] = %d failed at %s", i, buf2[i], __func__);
            return;
        } else {
            printk(KERN_ERR "buf2[%d] = %d", i, buf2[i]);
        }
    }
    printk(KERN_ERR "buffer read write test 7 passed");

    // fourth write from 4000 to 4100 then from 4100 to 4200
 
    // print start test header
    printk(KERN_ERR "start buffer read write test 8");

    for(i = 0; i < 100; i++) {
        buf[i] = i;
        res_buf[i] = i;
    }

    buf_wr(4000, 100, buf);

    for(i = 0; i < 100; i++) {
        buf[i] = i + 100;
        res_buf[i + 100] = i + 100;
    }

    buf_wr(4100, 100, buf);

    // then read from 4000 to 4200
    buf_rd(4000, 200, verify_buf);

    // verify
    for(i = 0; i < 200; i++) {
        printk(KERN_ERR "verify_buf[%d] = %d", i, verify_buf[i]);
        if(verify_buf[i] != res_buf[i]) {
            printk(KERN_ERR "buffer read write test failed");
            return;
        }
    }
    printk(KERN_ERR "buffer read write test 8 passed");

    printk(KERN_ERR "buffer read write test passed");
} 
