#include "buffer.h"

static bcache_t buffer_cache = {
    .active_list = LIST_HEAD_INIT(buffer_cache.active_list),
    .inactive_list = LIST_HEAD_INIT(buffer_cache.inactive_list),
    .root = RB_ROOT,
    .lock = __SPIN_LOCK_UNLOCKED(buffer_cache.lock)
};

static inline buf_rbh_t* get_rbh(struct rb_node* node) {
    return container_of(node, buf_rbh_t, node);
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
        printk(KERN_ERR, "%s BUG: root or node is NULL", __func__);
        return;
    }
    rb_erase(node, root);
    buf_rbh_t* rbh = get_rbh(node);
    if(rbh->buf) {
        kfree(rbh->buf);
    }
    // delete the corresponding list item
    list_del(rbh->li);
    kfree(rbh->li);
    kfree(rbh);
}

/**
 * side effect: Insert the node into the rb tree and the corresponding list item into the inactive list 
 * @return buf_rbh_t* new allocated rbh. NULL if failed.
 */
buf_rbh_t* buf_alloc(uint64_t lpa, uint64_t size, struct list_head* lh) {
    buf_rbh_t* rbh = kmalloc(sizeof(buf_rbh_t), GFP_KERNEL);
    if(NULL == rbh) {
        printk(KERN_ERR, "%s BUG: kmalloc failed", __func__);
        return NULL;
    }
    rbh->lpa = lpa;
    rbh->size = size;
    rbh->buf = kmalloc(size, GFP_KERNEL);
    if(NULL == rbh->buf) {
        printk(KERN_ERR, "%s BUG: kmalloc failed", __func__);
        kfree(rbh);
        return NULL;
    }
    rbh->hit_times = 0;
    // create new node and insert into inactive list starting with lh
    rbh->li = kmalloc(sizeof(struct list_head), GFP_KERNEL);
    if(NULL == rbh->li) {
        printk(KERN_ERR, "%s BUG: kmalloc failed", __func__);
        kfree(rbh->buf);
        kfree(rbh);
        return NULL;
    }
    INIT_LIST_HEAD(rbh->li);
    list_add_tail(rbh->li, lh);
    return rbh;
}


/**
 * @brief allocate a new rb tree node and insert it into the rb tree and linked list
 * @param root rb tree root
 * @param lpa lpa of the new node
 * @param node the new node
 * @param size size of the buffer associated with the new node
 * @return 0 if success, -1 if failed
 */
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
            printk(KERN_ERR, "%s BUG: lpa already in rb tree", __func__);
            return -1;
        }
    }

    if(NULL == *new) {
        buf_rbh_t* rbh = buf_alloc(lpa, size, lh);
        // add it in the rb tree
        rb_link_node(&(rbh->node), parent, new);
        rb_insert_color(&(rbh->node), root);
        *node = &(rbh->node);
        
        ret = 0;
    } else {
        printk(KERN_ERR, "%s BUG: rbh already exists", __func__);
    }
    return ret;
}

/**
 * @brief sync from & to ssd 
 * using byte issue for buffer cache
 */
static int64_t sync_up(uint64_t lpa, uint64_t size, void* buf) {
    int64_t ret;
    //spin_unlock_irqrestore(&buffer_cache.lock, buffer_cache.lock_flags);
    ret = byte_issue(0, lpa, size, buf);
    //spin_lock_irqsave(&buffer_cache.lock, buffer_cache.lock_flags);

}

static int64_t sync_down(uint64_t lpa, uint64_t size, void* buf) {
    int64_t ret;
    //spin_unlock_irqrestore(&buffer_cache.lock, buffer_cache.lock_flags);
    ret = byte_issue(1, lpa, size, buf);
    //spin_lock_irqsave(&buffer_cache.lock, buffer_cache.lock_flags);
}


/**
 * @brief check whether evict is needed after we do a write. 
 * If needed, evict 10% total size of buf. This is ratio is subject to change.
 * buffer evicted is from inactive list and they're also deleted from rb tree
 * @return int64_t 
 */
static int64_t try_buf_evict(void) {

}

static int64_t hit_cache(buf_rbh_t* rbh, uint64_t inc_size, uint64_t inc_hit_times) {
    if(rbh->hit_times < LRU_TRANSFER_TIMES && rbh->hit_times + inc_hit_times >= LRU_TRANSFER_TIMES) {
        // move to active list
        list_del(rbh->li);
        list_add_tail(rbh->li, &(buffer_cache.active_list));
        buffer_cache.active_size += rbh->size;
        buffer_cache.inactive_size -= rbh->size;
    }
    rbh->hit_times += inc_hit_times;

    if(rbh->hit_times < LRU_TRANSFER_TIMES) buffer_cache.inactive_size += inc_size;
    else buffer_cache.active_size += inc_size;
    
}


/**
 * @brief starting from @param lpa we read the following @param size into @param buf.
 * Check RB tree -> exist -> read from buffer cache
 *               |
 *               -> not exist -> sync to buffer cache and read (insert to rb tree and list)
 * Merge can happen if two buffers are contigous, in which case we need to delete the second buffer (larger lpa) in both LRU and 
 * RB tree.
 * @todo : need a lock
 * @return int64_t - size read -1 if failed
 */
int64_t buf_rd(uint64_t lpa, uint64_t size, char* buf) {
    struct rb_node* node = NULL, *nxt_node = NULL;
    buf_rbh_t* rbh, *rbh_next;
    uint8_t l_contig, r_contig; // adjacent to left / right buffer 
    uint64_t bufl_l, bufl_r, bufr_l, bufr_r; // left and right buffer's boundaries
    uint64_t buf_l, buf_r; // buffer to be read's boundaries
    char* tmp_buf = NULL;
    uint64_t flags;

    //spin_lock_irqsave(&buffer_cache.lock, buffer_cache.lock_flags);
    node = rb_search(&buffer_cache.root, lpa);
    nxt_node = node == NULL ? NULL : rb_next(node);


    rbh = (node == NULL) ? NULL : get_rbh(node);
    rbh_next = (nxt_node == NULL) ? NULL :get_rbh(nxt_node);


    if(rbh) { 
        bufl_l = rbh->lpa;
        bufl_r = rbh->lpa + rbh->size - 1;
    }
    if(rbh_next) {
        bufr_l = rbh_next->lpa;
        bufr_r = rbh_next->lpa + rbh_next->size - 1;
    }


    buf_l = lpa;
    buf_r = lpa + size - 1;

    // l_contig = (bufl_r >= buf_l - 1);
    // r_contig = (bufr_l <= buf_r + 1);
    // less aggressive merge
    l_contig = rbh ? (bufl_r >= buf_l) : 0;
    r_contig = rbh_next ? (bufr_l <= buf_r) : 0;

    // INIT_RD_WR_VARS(lpa, rbh, rbh_next, l_contig, r_contig, bufl_l, bufl_r, bufr_l, bufr_r, buf_l, buf_r);
    

    // cache hit 
    if(rbh && buf_l >= bufl_l && buf_r <= bufl_r) {
        memcpy(buf, rbh->buf + (buf_l - bufl_l), size);
        hit_cache(rbh, 0, 1);
        goto read_finish;
    }
    if(!l_contig && !r_contig) { // no overlap
        if(0 > sync_up(lpa, size, buf)){
            goto err;
        }
        if(0 > rb_tree_alloc_and_insert(&buffer_cache.root, lpa, &node, size)) {
            goto err;
        }
        rbh = get_rbh(node);
        memcpy(rbh->buf, buf, size);
        hit_cache(rbh, size, 0);
        goto read_finish;
    }
    if(l_contig && r_contig) { // fill the hole between adjacent two buffers
        // small graph to show the situation
        // bufl_l buf_l bufl_r bufr_l buf_r bufr_r
        // |      |      | ------ |      |      |
        // -------------------------------------
        printk("BUG : unreasonable for metadata alignment : %s", __func__);
        if(0 > sync_up(bufl_r + 1, bufr_l - bufl_r - 1, buf + bufl_r - buf_l + 1)) {
            goto err;
        }
        // no need to allocate, just change the node, but need to delete the next node
        rbh->size = bufr_r - bufl_l + 1;
        tmp_buf = kmalloc(rbh->size, GFP_KERNEL);
        memcpy(tmp_buf, rbh->buf, bufl_r - bufl_l + 1);
        memcpy(tmp_buf + bufr_l - bufl_l, rbh_next->buf, bufr_r - bufr_l + 1);
        memcpy(tmp_buf + bufl_r - bufl_l + 1, buf + bufl_r - buf_l + 1, bufr_l - bufl_r - 1);
        kfree(rbh->buf);
        rbh->buf = tmp_buf;
        // read from buffer cache
        memcpy(buf, rbh->buf + buf_l - bufl_l, size);

        hit_cache(rbh, bufr_l - bufl_l - 1, rbh_next->hit_times + 1);
        
        // delete the next node
        rb_erase_and_free(nxt_node, &buffer_cache.root);
        buffer_cache.inactive_size += bufr_l - bufl_r - 1;
        goto read_finish;
    }
    if(l_contig) { // only overlap with the first buffer
        // small graph to show the situation
        // bufl_l buf_l bufl_r buf_r bufr_l bufr_r
        // |      |      | ------ |      |      |
        // -------------------------------------

        if(0 > sync_up(bufl_r + 1, buf_r - bufl_r, buf + bufl_r - buf_l + 1)) {
            goto err;
        }
        // no need to allocate, just change the node
        rbh->size = buf_r - bufl_l + 1;
        
        //buffer should be expanded
        tmp_buf = kmalloc(rbh->size, GFP_KERNEL);
        memcpy(tmp_buf, rbh->buf, bufl_r - bufl_l + 1);
        memcpy(tmp_buf + bufl_r - bufl_l + 1, buf + bufl_r - buf_l + 1, buf_r - bufl_r);
        kfree(rbh->buf);
        rbh->buf = tmp_buf;
        // read from buffer cache
        memcpy(buf, rbh->buf + buf_l - bufl_l, size);

        hit_cache(rbh, buf_r - bufl_r, 1);

        goto read_finish;
    }
    if(r_contig){ // only overlap with the next buffer
        // small graph to show the situation
        // bufl_l bufl_r buf_l bufr_l buf_r bufr_r
        // |      |      | ----- |       |      |
        // -------------------------------------
        if(0 > sync_up(buf_l, bufr_l - buf_l, buf)) {
            goto err;
        }
        // no need to allocate, just change the node
        rbh_next->size = bufr_r - buf_l + 1;

        //buffer should be expanded
        tmp_buf = kmalloc(rbh_next->size, GFP_KERNEL);
        memcpy(tmp_buf, buf, bufr_l - buf_l);
        memcpy(tmp_buf + bufr_l - buf_l, rbh_next->buf, bufr_r - bufr_l + 1);
        kfree(rbh_next->buf);
        rbh_next->buf = tmp_buf;

        // read from buffer cache
        memcpy(buf, rbh_next->buf, size);

        hit_cache(rbh_next, bufr_l - buf_l, 1);

        goto read_finish;
    }
read_finish:
    //spin_unlock_irqrestore(&buffer_cache.lock, buffer_cache.lock_flags);
    return size; 
err:
    //spin_unlock_irqrestore(&buffer_cache.lock, buffer_cache.lock_flags);
    printk(KERN_ERR "BUG: logical error at %s", __func__);
    return -1;
}

/**
 * @brief starting from @param lpa write @param size of metadata from @param buf to disk.
 * Check RB tree -> exist -> Update node
 *               |
 *               -> not exist -> Insert into RB tree, append into list 
 * Merge can happen if two buffers are contigous, in which case we need to delete the second buffer (larger lpa) in both LRU and 
 * RB tree.
 * @todo : need a lock
 * @return int64_t 
 */
int64_t buf_wr(uint64_t lpa, uint64_t size, char* buf) {
    struct rb_node* node = NULL, *nxt_node = NULL;
    buf_rbh_t* rbh, *rbh_next;
    uint8_t l_contig, r_contig; // adjacent to left / right buffer 
    uint64_t bufl_l, bufl_r, bufr_l, bufr_r; // left and right buffer's boundaries
    uint64_t buf_l, buf_r; // buffer to be read's boundaries
    char* tmp_buf = NULL;
    uint64_t flags;

    //spin_lock_irqsave(&buffer_cache.lock, buffer_cache.lock_flags);
    node = rb_search(&buffer_cache.root, lpa);
    nxt_node = node == NULL ? NULL : rb_next(node);


    rbh = (node == NULL) ? NULL : get_rbh(node);
    rbh_next = (nxt_node == NULL) ? NULL :get_rbh(nxt_node);


    if(rbh) { 
        bufl_l = rbh->lpa;
        bufl_r = rbh->lpa + rbh->size - 1;
    }
    if(rbh_next) {
        bufr_l = rbh_next->lpa;
        bufr_r = rbh_next->lpa + rbh_next->size - 1;
    }


    buf_l = lpa;
    buf_r = lpa + size - 1;

    // l_contig = (bufl_r >= buf_l - 1);
    // r_contig = (bufr_l <= buf_r + 1);
    // less aggressive merge
    l_contig = rbh ? (bufl_r >= buf_l) : 0;
    r_contig = rbh_next ? (bufr_l <= buf_r) : 0;


    // INIT_RD_WR_VARS(lpa, rbh, rbh_next, l_contig, r_contig, bufl_l, bufl_r, bufr_l, bufr_r, buf_l, buf_r);
    // cache hit 
    if(rbh && buf_l >= bufl_l && buf_r <= bufl_r) {
        memcpy(rbh->buf + (buf_l - bufl_l), buf, size);
        hit_cache(rbh, 0, 1);
        goto finish_write;
    }

    if(!l_contig && !r_contig) { // no overlap
        if(0 > rb_tree_alloc_and_insert(&buffer_cache.root, lpa, &node, size)) {
            printk(KERN_ERR "%s : bad rb tree insert", __func__);
            return -1;
        }
        rbh = get_rbh(node);
        memcpy(rbh->buf, buf, size);
        hit_cache(rbh, size, 0);
        goto finish_write;
    }
    if(l_contig && r_contig) { // fill the hole between adjacent two buffers
        // no need to allocate, just change the node, but need to delete the next node
        rbh->size = bufr_r - bufl_l + 1;
        tmp_buf = kmalloc(rbh->size, GFP_KERNEL);
        memcpy(tmp_buf, rbh->buf, bufl_r - bufl_l + 1);
        memcpy(tmp_buf + bufr_l - bufl_l, rbh_next->buf, bufr_r - bufr_l + 1);
        memcpy(tmp_buf + bufl_r - bufl_l + 1, buf + bufl_r - buf_l + 1, bufr_l - bufl_r - 1);
        kfree(rbh->buf);
        rbh->buf = tmp_buf;
        // write to buffer cache
        memcpy(rbh->buf + buf_l - bufl_l, buf, size);

        hit_cache(rbh, bufr_l - bufl_l - 1, rbh_next->hit_times + 1);

        // delete the next node
        rb_erase_and_free(nxt_node, &buffer_cache.root);
        goto finish_write;
    }
    if(l_contig) { // only overlap with the first buffer
        rbh->size = buf_r - bufl_l + 1;
        
        //buffer should be expanded
        tmp_buf = kmalloc(rbh->size, GFP_KERNEL);
        memcpy(tmp_buf, rbh->buf, bufl_r - bufl_l + 1);
        memcpy(tmp_buf + bufl_r - bufl_l + 1, buf + bufl_r - buf_l + 1, buf_r - bufl_r);
        kfree(rbh->buf);
        rbh->buf = tmp_buf;

        // write to buffer cache
        memcpy(rbh->buf + buf_l - bufl_l, buf, size);

        hit_cache(rbh, buf_r - bufl_r, 1);

        goto finish_write;
    }
    if(r_contig){ // only overlap with the next buffer
        rbh_next->size = bufr_r - buf_l + 1;

        //buffer should be expanded
        tmp_buf = kmalloc(rbh_next->size, GFP_KERNEL);
        memcpy(tmp_buf, buf, bufr_l - buf_l);
        memcpy(tmp_buf + bufr_l - buf_l, rbh_next->buf, bufr_r - bufr_l + 1);
        kfree(rbh_next->buf);
        rbh_next->buf = tmp_buf;

        // read from buffer cache
        memcpy(rbh_next->buf, buf, size);

        hit_cache(rbh_next, bufr_l - buf_l, 1);
        
        goto finish_write;
    }

    printk(KERN_ERR "BUG: logical error at %s", __func__);
    //spin_unlock_irqrestore(&buffer_cache.lock, buffer_cache.lock_flags);
    return -1;
finish_write:
    if(0 > sync_down(lpa, size, buf)) {
        printk(KERN_ERR "%s : bad sync down", __func__);
        return -1;
    }
    try_buf_evict();
    //spin_unlock_irqrestore(&buffer_cache.lock, buffer_cache.lock_flags);
    return size;
}


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
    uint8_t buf[100], buf2[100];
    
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
        if(buf2[i] != i + 200) {
            printk(KERN_ERR "buf2[%d] = %d failed at %s", i, buf2[i], __func__);
            return;
        } else {
            printk(KERN_ERR "buf2[%d] = %d", i, buf2[i]);
        }
    }
    printk(KERN_ERR "buffer read write test 3 passed");

    // fourth write from 1000 to 1100, then 950 to 1050
    // print start test header
    printk(KERN_ERR "start buffer read write test 4");

    for(i = 0; i < 100; i++) {
        buf[i] = i + 300;
    }
    buf_wr(1000, 100, buf);
    for(i = 0; i < 100; i++) {
        buf[i] = i + 400;
    }
    buf_wr(950, 100, buf);
    // then read from 950 to 1050
    buf_rd(950, 100, buf2);
    for(i = 0; i < 100 ; i++) {
        if(buf2[i] != i + 400) {
            printk(KERN_ERR "buffer read write test failed");
            return;
        }
    }
    // then read from 1000 to 1100
    buf_rd(1000, 100, buf2);
    for(i = 0; i < 50; i++) {
        if(buf2[i] != i + 400) {
            printk(KERN_ERR "buffer read write test failed");
            return;
        }
    }
    for(i = 50; i < 100; i++) {
        if(buf2[i] != i + 300) {
            printk(KERN_ERR "buffer read write test failed");
            return;
        }
    }
    printk(KERN_ERR "buffer read write test 4 passed");

    // fifth write from 1920 to 2020, then 2080 to 2180, then 2000 to 2100
    // print start test header
    printk(KERN_ERR "start buffer read write test 5");

    for(i = 0; i < 100; i++) {
        buf[i] = i + 500;
    }
    buf_wr(1920, 100, buf);
    for(i = 0; i < 100; i++) {
        buf[i] = i + 600;
    }
    buf_wr(2080, 100, buf);
    for(i = 0; i < 100; i++) {
        buf[i] = i + 700;
    }
    buf_wr(2000, 100, buf);

    // then read from 2000 to 2100
    buf_rd(2000, 100, buf2);
    for(i = 0; i < 100; i++) {
        if(buf2[i] != i + 700) {
            printk(KERN_ERR "buffer read write test failed");
            return;
        }
    }
    // then read from 1920 to 2020
    buf_rd(1920, 100, buf2);
    for(i = 0; i < 20; i++) {
        if(buf2[i] != i + 500) {
            printk(KERN_ERR "buffer read write test failed");
            return;
        }
    }
    for(i = 20; i < 100; i++) {
        if(buf2[i] != i + 700) {
            printk(KERN_ERR "buffer read write test failed");
            return;
        }
    }
    // then read from 2080 to 2180
    buf_rd(2080, 100, buf2);
    for(i = 0; i < 20; i++) {
        if(buf2[i] != i + 600) {
            printk(KERN_ERR "buffer read write test failed");
            return;
        }
    }
    for(i = 20; i < 100; i++) {
        if(buf2[i] != i + 700) {
            printk(KERN_ERR "buffer read write test failed");
            return;
        }
    }
    printk(KERN_ERR "buffer read write test 5 passed");

    // sixth write from 3000 to 3100, then 3100 to 3200 then 3200 to 3300
    // print start test header
    printk(KERN_ERR "start buffer read write test 6");

    for(i = 0; i < 100; i++) {
        buf[i] = i + 800;
    }
    buf_wr(3000, 100, buf);
    for(i = 0; i < 100; i++) {
        buf[i] = i + 900;
    }
    buf_wr(3100, 100, buf);
    for(i = 0; i < 100; i++) {
        buf[i] = i + 1000;
    }
    buf_wr(3200, 100, buf);

    // then read from 3000 to 3100
    buf_rd(3000, 100, buf2);
    for(i = 0; i < 100; i++) {
        if(buf2[i] != i + 800) {
            printk(KERN_ERR "buffer read write test failed");
            return;
        }
    }
    // then read from 3100 to 3200
    buf_rd(3100, 100, buf2);
    for(i = 0; i < 100; i++) {
        if(buf2[i] != i + 900) {
            printk(KERN_ERR "buffer read write test failed");
            return;
        }
    }
    // then read from 3200 to 3300
    buf_rd(3200, 100, buf2);
    for(i = 0; i < 100; i++) {
        if(buf2[i] != i + 1000) {
            printk(KERN_ERR "buffer read write test failed");
            return;
        }
    }
    printk(KERN_ERR "buffer read write test 6 passed");

} 
