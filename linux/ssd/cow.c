/** Notes : haor2
 * Although there are one layer of cache in 2B SSD. We can still exploit the DRAM to use page cache.
 * When we perform a read / write issue to 2B SSD, we first check the page cache. 
 * 
 * Upon each issue, we cache them into pages. When read, we first check page cache. We directly issue them(implict fsync) upon cache miss.
 * When write, we write to page cache. When mmap, we return the pages upon cache hit, otherwise, we fsync, put it to page cache, 
 * then return the associated pages.
 *      The pages are indexed by their lpa and are stored in address_space mapping by a radix tree. 
 * We have two fsync strategies.
 * 1. The old way.
 *      Upon fsync, we check whether the pages are dirty, if so we issue every page.
 * 2. New way.
 *      Upon fsync, we check which part of the page are dirty, if so we issue every part.
 *      In order to implement that, each time the page cache is created or sync-ed(that is, every time it's clean), we need to keep a copy of clean version
 * and modify the page.
 * 
 * Trade-offs:
 *      The new way : 
 *          1. can byte issue instead of block issue
 * --------------------------------------------------------------------
 *          2. need to have duplicate page for each dirty page
 *          3. need to scan page again
 *  Implementation : 
 *          1. Instead of using the built-in page cache, we use a radix tree.
 *          2. Each page has a connected duplicate clean page.
*/
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/buffer_head.h>
#include <asm/pgtable.h>
#include <linux/kernel.h>
#include <linux/raid/xor.h>
#include <linux/printk.h>
#include <linux/semaphore.h>
#include <linux/llist.h>


#include "cow.h"
#include "ftl.h"
#include "ssd_stat.h"

#define MAX_INODE_NUM   100000000L

#define PAGE_CACHE_CAPACITY_HI_PAGE             (4194304L)
#define PAGE_CACHE_CAPACITY_LO_PAGE             (3932160L)
#define PAGE_CACHE_RW_BLOCK_THRESHOLD_PAGE      (4456448L)

// struct list_head filemap_list;
// int inited = 0;

struct llist_head global_active_list;
struct llist_head global_inactive_list;

fmapping_t **inode_fmapping_arr;
int rw_block = 0;

void* thread_id;

static atomic64_t pagecount_cur = ATOMIC_INIT(0);

static atomic64_t listcount_cur = ATOMIC_INIT(0);

static atomic64_t tofreecount_cur = ATOMIC_INIT(0);

// spinlock to protect filp allocation
static DEFINE_SPINLOCK(list_lock);

static DEFINE_SPINLOCK(evict_lock);

// static DEFINE_SPINLOCK(filp_lock);
// spinlock to protect filp list
// static DEFINE_SPINLOCK(filp_listlock);

static inline rbh_t* get_rbh(struct rb_node* node) {
    return container_of(node, rbh_t, rb_node);
}

static inline rbh_t* get_rbh_from_li(struct llist_node* li) {
    return container_of(li, rbh_t, li);
}

// static inline fmapping_t* get_filemap_from_li(struct list_head* li) {
//     return container_of(li, fmapping_t, li);
// }


static void _bytefs_init_pgcache(fmapping_t* fmap) {
    fmap->root = RB_ROOT;
    fmap->page_count = 0;
    // initialize spinlock for fmap
    spin_lock_init(&fmap->lock);
    // initialize spinlock for rbt
    init_rwsem(&fmap->rbt_sem);
    spin_lock_init(&fmap->rbt_lock);
    // initialize spinlock for fmap eviction
    spin_lock_init(&fmap->fmap_evict_lock);
    // initalize active / inactive list
    // INIT_LIST_HEAD(&fmap->active_list);
    // INIT_LIST_HEAD(&fmap->inactive_list);
    // INIT_LIST_HEAD(&fmap->li);
    atomic64_set(&fmap->remaining_refs, 0);
    fmap->active_size = 0;
    fmap->inactive_size = 0;
    // list_add_tail(&fmap->li, &filemap_list);
}

// allocate and initialize
static int64_t bytefs_alloc_fmap(struct file* filp) {
    // don't have page cache for this file
    fmapping_t* fmap;
    unsigned long fino = filp->f_inode->i_ino;

    if (fino > MAX_INODE_NUM) {
        bytefs_err("Inode at %d, exceeding maximum of %d", fino, MAX_INODE_NUM);
        return -1;
    }

    fmap = inode_fmapping_arr[fino];
    if (!fmap) {
        fmap = kzalloc(sizeof(fmapping_t), GFP_KERNEL);
        if (!fmap) {
            bytefs_err("%s kzalloc failed", __func__);
            return -1;
        }
        _bytefs_init_pgcache(fmap);
        fmap->file = filp;
        inode_fmapping_arr[fino] = fmap;
    } else {
        // fmap cached
        fmap->file = filp;
    }
    
    bytefs_wr_private(filp->private_data, fmap);
    return 0;
}

// // not used now, see separate alloc and init
// static rbh_t* bytefs_alloc_init_rbh(uint64_t lpa, struct list_head* lh, fmapping_t* fmap) {
//     unsigned long flag;
//     rbh_t* rbh = kmalloc(sizeof(rbh_t), GFP_ATOMIC);
//     if (!rbh) {
//         printk(KERN_ERR "%s kmalloc failed", __func__);
//         return NULL;
//     }
//     rbh->lpa = lpa;
//     rbh->page = bytefs_alloc_page(GFP_ATOMIC);
//     atomic64_inc(&pagecount_cur);
//     if (!rbh->page) {
//         printk(KERN_ERR "%s alloc_page failed", __func__);
//         return NULL;
//     }
//     rbh->dup_page = NULL;
//     rbh->flags = RBH_FLAGS_INIT;

//     // initialize list head, add it to inactive list
//     INIT_LIST_HEAD(&rbh->li);
//     // spin_lock_irqsave(&list_lock,flag);
//     // TODO: might move this also into lock region
//     llist_add(&rbh->li, &lh);
//     // spin_unlock_irqrestore(&evict_lock,flag);
    
//     rbh->hit_times = 0;
//     rbh->locality = 0;
//     // rbh->dirty_ratio = 0;
//     rbh->fmap = fmap;
//     return rbh;
// }


static rbh_t* bytefs_alloc_rbh(uint64_t lpa, fmapping_t* fmap) {
    unsigned long flag;
    rbh_t* rbh = kzalloc(sizeof(rbh_t), GFP_KERNEL);
    if (!rbh) {
        printk(KERN_ERR "%s kmalloc failed", __func__);
        return NULL;
    }
    rbh->lpa = lpa;
    rbh->page = NULL;
    rbh->dup_page = NULL;
    RBH_SET_INVALID(rbh);
    init_rwsem(&rbh->in_use);
    down_read(&rbh->in_use); // TODO: check this
    rbh->hit_times = 0;
    rbh->locality = 0;
    // rbh->dirty_ratio = 0;
    rbh->fmap = fmap;
    return rbh;
}

static uint64_t bytefs_dealloc_rbh(rbh_t* rbh) {
    if (rbh)
        kfree(rbh);
    return 0;
}

static uint64_t bytefs_init_rbh(rbh_t* rbh, struct llist_head* lh) {
    rbh->page = bytefs_alloc_page(GFP_ATOMIC);
    if (!rbh->page) {
        printk(KERN_ERR "%s alloc_page failed", __func__);
        return -1;
    }
    atomic64_inc(&pagecount_cur);

    bytefs_assert(rbh->fmap);
    atomic64_inc(&rbh->fmap->remaining_refs);
    
    // initialize list head, add it to inactive list
    rbh->li.next = NULL;
    llist_add(&rbh->li, lh);
    atomic64_inc(&listcount_cur);

    return 0;
}


// sync rbh from disk 
static int64_t sync_rbh_rd(rbh_t* rbh) {
    if (NULL == rbh || NULL == rbh->page) return -1;
    // read entire page from disk
    bytefs_assert((rbh->flags & RBH_FLAGS_DIRTY) == 0);
    // RBH_SET_CLEAN(rbh);
    SSD_STAT_ATOMIC_ADD(block_data_traffic_r, PG_SIZE);
    // currently only use block issue to sync page
    return nvme_issue(0, rbh->lpa / PG_SIZE, 1, rbh->page);
    return 0;
}
#if (PG_CACHE_METHOD == PG_CACHE_METHOD_OLD)
// sync rbh to disk 
static int64_t sync_rbh_wr(rbh_t* rbh) {
    if(NULL == rbh || NULL == rbh->page) return -1;
    // currently only use block issue to sync page
    RBH_SET_CLEAN(rbh);
    return nvme_issue(1, rbh->lpa / PG_SIZE, 1, rbh->page);
}
#endif

// static void try_move_page_inactive(rbh_t* rbh) {
//     if(NULL == rbh) {
//         printk(KERN_ERR "%s BUG: rbh is NULL", __func__);
//         return;
//     }
//     if(rbh->hit_times <= LRU_TRANSFER_TIMES) return ; // already in inactive list
//     list_del(&rbh->li);
//     list_add_tail(&rbh->li, &rbh->fmap->inactive_list);
//     rbh->fmap->inactive_size += PG_SIZE;
//     rbh->fmap->active_size -= PG_SIZE;
// }
#if (1 == BYTE_ISSUE_64_ALIGN)
/**
 * @note @param buf should be PG_SIZE cache.
 * @brief because @param buf always have enough space. No need for read modify write.
 * @note the reason why we need to use 64 byte issue here is that we already have one buffer
 * so we don't need read modify write.
 */
static int64_t sync_down_64(uint64_t lpa, uint64_t size, void* buf) {
    uint64_t C0 = lpa, C1 = lpa + size;
	uint64_t S0 = C0 / 64 * 64; 
	uint64_t S2 = C1 / 64 * 64, S3 = C1 / 64 * 64 + 64;
    uint64_t start, end;
    start = S0;
    end = C1 % 64 ? S3 : S2;
    return byte_issue(1, start, end - start, buf + start % PG_SIZE);
}

static int64_t sync_down_64_aligned(uint64_t lpa, uint64_t size, void* buf) {
    bytefs_assert(lpa % 64 == 0 && size % 64 == 0);
    return byte_issue(1, start, size, buf + lpa % PG_SIZE);
}
#endif

static int64_t sync_rbh_wr_new(rbh_t* rbh) {
    int32_t i,word;
    int32_t st, ed;
    if (NULL == rbh || NULL == rbh->page || NULL == rbh->dup_page) 
        return -1;
    // currently only use block issue to sync page
    st = ed = 0;
    uint64_t bitmap = 0;
    uint64_t dest = 0;
    // void** ptr = kmalloc(sizeof(void*)*2, GFP_ATOMIC);  
    int modified = 0;

    for (i = 0; i < PG_SIZE; i += 64) {
        // ptr[0] = (void*)(rbh->page + i);
        // ptr[1] = (unsigned long*)(rbh->dup_page + i);
        // xor_blocks(2, 8, &dest, ptr);
        for (word = 0; word < 8; word++) {
            dest += ((uint64_t*)(rbh->page + i))[word] ^ ((uint64_t*)(rbh->dup_page + i))[word];
            if (dest) break;
        }

        if (dest) {
            bitmap |= (1UL << (i / 64));
            modified += 1;
        }
        dest = 0;
        // printk_ratelimited(" dest: %lx",  dest);
    }
    // kfree(ptr);
    SSD_STAT_ATOMIC_ADD(page_cache_flush_traffic, PG_SIZE);

    // TODO: change this metric
    // rbh->locality = (modified * 2 * ALPHA + (LOC_MAX - ALPHA) * rbh->locality) / LOC_MAX;
    // if( rbh->locality > LOC_MAX * 5 / 6 || modified > 48) {
    if (modified > 16) {
        // write entire page
        nvme_issue(1, rbh->lpa / PG_SIZE, 1, rbh->page);
        // SSD_STAT_ATOMIC_INC(locality_info[modified][rbh->locality][0]);
        SSD_STAT_ATOMIC_ADD(page_cache_actuall_w_traffic, PG_SIZE);
        SSD_STAT_ATOMIC_ADD(block_data_traffic_w, PG_SIZE);
    } else {
        for (i = 0; i < PG_SIZE; i += 64) {
            if (bitmap & (1UL << (i / 64))) {
                // sync_down_64(rbh->lpa + i, 64, rbh->page);
                sync_down_64_aligned(rbh->lpa + i, 64, rbh->page);
            }
        }
        // SSD_STAT_ATOMIC_INC(locality_info[modified][rbh->locality][1]);
        SSD_STAT_ATOMIC_ADD(page_cache_actuall_w_traffic,modified * 64);
        SSD_STAT_ATOMIC_ADD(byte_data_traffic_w,modified * 64);
    }
    // reset rbh status
    RBH_SET_CLEAN(rbh);
    rbh->hit_times = 0;
    // try_move_page_inactive(rbh);

    // free and remove comparasion buffer
    kfree(rbh->dup_page);
    rbh->dup_page = NULL;
    atomic64_dec(&pagecount_cur);
    return 0;
sync_failed:
    printk(KERN_ERR "%s BUG: sync failed", __func__);
        return -1;
    }


// allocate and assign value to duplicated page. 
// This function must be called when page is clean.
static int64_t get_dup_page(rbh_t* rbh) {
    if (NULL == rbh) {
        printk(KERN_ERR, "%s BUG: rbh is NULL", __func__);
        return -1;
    }
    if (NULL == rbh->dup_page) {
        rbh->dup_page = bytefs_alloc_page(GFP_ATOMIC);
        if (NULL == rbh->dup_page) {
            printk(KERN_ERR, "%s BUG: alloc_page failed", __func__);
            return -1;
        }
        atomic64_inc(&pagecount_cur);
        // copy page to dup page
        memcpy(rbh->dup_page, rbh->page, PG_SIZE);
    } else {
        printk(KERN_ERR, "%s BUG: dup_page is not NULL", __func__);
        return -1;
    }
    return 0;
}

/** 
 * @brief this function should only be called when the page with @param lpa is not in the cache.
 * @todo we should use a lock to protect the rb tree.
 * @return int64_t  0 if success, -1 if failed
 */
static int64_t rb_tree_alloc_and_insert(struct rb_root* root, uint64_t lpa, 
 struct rb_node** node, struct llist_head* lh, fmapping_t* fmap) {
    struct rb_node** new;
    struct rb_node* parent = NULL;
    int64_t ret = -1;

    if (NULL == root) return -1;
    bytefs_assert(lpa % PG_SIZE == 0);

    // new = &(root->rb_node);

    // while (*new) {
    //     rbh_t* rbh = get_rbh(*new);
    //     parent = *new;
    //     if(lpa < rbh->lpa) {
    //         new = &((*new)->rb_left);
    //     } else if(lpa > rbh->lpa) {
    //         new = &((*new)->rb_right);
    //     } else {
    //         printk(KERN_ERR "%s BUG: lpa already in rb tree", __func__);
    //         return -1;
    //     }
    // }

    // if(NULL == *new) {
    //     rbh_t* rbh = bytefs_alloc_init_rbh(lpa, lh, fmap);
    //     // add it in the rb tree
    //     rb_link_node(&(rbh->rb_node), parent, new);
    //     rb_insert_color(&(rbh->rb_node), root);
    //     *node = &(rbh->rb_node);
        
    //     ret = 0;
    // } else {
    //     printk(KERN_ERR "%s BUG: rbh already exists", __func__);
    // }
    
    // alloc
    rbh_t* rbh = bytefs_alloc_rbh(lpa, fmap);

    // find and insert
    if (down_write_trylock(&fmap->rbt_sem) == 0) 
        goto rbh_lock_failed;
    // spin_lock(&fmap->rbt_lock);
    new = &(root->rb_node);
    while (*new) {
        rbh_t* rbh = get_rbh(*new);
        parent = *new;
        if (lpa < rbh->lpa) {
            new = &((*new)->rb_left);
        } else if(lpa > rbh->lpa) {
            new = &((*new)->rb_right);
        } else {
            printk(KERN_ERR "%s BUG: lpa already in rb tree", __func__);
            goto rbh_insert_failed;
        }
    }
    if (NULL == *new) {
        // add it in the rb tree
        rb_link_node(&(rbh->rb_node), parent, new);
        rb_insert_color(&(rbh->rb_node), root);
        *node = &(rbh->rb_node);
        ret = 0;
    } else {
        printk(KERN_ERR "%s BUG: rbh already exists", __func__);
        goto rbh_insert_failed;
    }
    // init
    bytefs_init_rbh(rbh, lh);
    up_write(&fmap->rbt_sem);
    // spin_unlock(&fmap->rbt_lock);
    return ret;

rbh_lock_failed:
    bytefs_err("rbh lock failed");
    bytefs_dealloc_rbh(rbh);
    return -1;
    
rbh_insert_failed:
    up_write(&fmap->rbt_sem);
    bytefs_err("rbh insertion failed");
    // spin_unlock(&fmap->rbt_lock);
    bytefs_dealloc_rbh(rbh);
    return -1;
}

// find a rb tree node
static struct rb_node* rb_search(fmapping_t* fmap, uint64_t lpa) {
    struct rb_root* root = &fmap->root;

    if (down_read_trylock(&fmap->rbt_sem) == 0)
        return NULL;
    // spin_lock(&fmap->rbt_lock);
    struct rb_node* node = root->rb_node;
    while (node) {
        rbh_t* rbh = get_rbh(node);
        if (lpa < rbh->lpa) {
            node = node->rb_left;
        } else if(lpa > rbh->lpa) {
            node = node->rb_right;
        } else {
            up_read(&fmap->rbt_sem);
            // spin_unlock(&fmap->rbt_lock);
            return node;
        }
    }
    up_read(&fmap->rbt_sem);
    // spin_unlock(&fmap->rbt_lock);
    return NULL;
}

// free a rb tree node
// static void rb_erase_and_free(fmapping_t* fmap, struct rb_node* node) {
//     struct rb_root* root = &fmap->root;
//
//     if (NULL == root || NULL == node) {
//         printk(KERN_ERR, "%s BUG: root or node is NULL", __func__);
//         return;
//     }
//
//     // remove target rbh from file rb tree
//     // down_write(&fmap->rbt_sem);
//     rb_erase(node, root);
//     // up_write(&fmap->rbt_sem);
//
//     // free rbh
//     rbh_t* rbh = get_rbh(node);
//     if (rbh->page) {
//         kfree(rbh->page);
//         rbh->page = NULL;
//         atomic64_dec(&pagecount_cur);
//     }
//     if (rbh->dup_page) {
//         kfree(rbh->dup_page);
//         rbh->dup_page = NULL;
//         atomic64_dec(&pagecount_cur);
//     }
//     kfree(rbh);
// }

// free a rb tree node
static int rb_erase_from_tree(fmapping_t* fmap, struct rb_node* node) {
    struct rb_root* root = &fmap->root;

    if (NULL == root || NULL == node) {
        printk(KERN_ERR, "%s BUG: root or node is NULL", __func__);
        return;
    }
    // remove target rbh from file rb tree

    if (down_write_trylock(&fmap->rbt_sem) == 0) {
        return -1;
    }
    // spin_lock(&fmap->rbt_lock);
    rb_erase(node, root);
    up_write(&fmap->rbt_sem);
    // spin_unlock(&fmap->rbt_lock);
    return 0;
}

// free a rb tree node
static void rb_free_node(struct rb_node* node) {
    // free rbh
    rbh_t* rbh = get_rbh(node);
    if (rbh->page) {
        kfree(rbh->page);
        rbh->page = NULL;
        atomic64_dec(&pagecount_cur);
    }
    if (rbh->dup_page) {
        kfree(rbh->dup_page);
        rbh->dup_page = NULL;
        atomic64_dec(&pagecount_cur);
    }
    kfree(rbh);
}

// static void hit_cache(rbh_t* rbh, uint64_t inc_size, uint6list_lock4_t inc_hit_times) {
//     // inc_size != 0 <= cache miss
//     // if(rbh->hit_times < LRU_TRANSFER_TIMES) rbh->fmap->inactive_size += inc_size;
//     // else{
//     //     rbh->fmap->active_size += inc_size;
//     //     if(inc_size) // sanity check
//     //         printk(KERN_ERR "%s BUG: active size should not be increased", __func__);
//     // }

//     if(rbh->hit_times < LRU_TRANSFER_TIMES && 
//      rbh->hit_times + inc_hit_times >= LRU_TRANSFER_TIMES) {
//         // move to active list
//         list_del(&(rbh->li));
//         list_add_tail(&(rbh->li), &(rbh->fmap->active_list));
//     }
//     rbh->hit_times += inc_hit_times;
// }




// /**
//  * @note caller needs to release lock before entering this function
//  */
// static void try_evict(struct list_head* lh) {
//     // go through inactive list and evict all pages
//     struct list_head* pos, *q;
//     uint64_t flags;
//     struct inode* inode;

//     // spin_lock_irqsave(&list_lock, flags);

//     list_for_each_safe(pos, q, lh) {
//         if(pagecount_cur <= 1500000) break;
//         rbh_t* rbh = get_rbh_from_li(pos);
//         if(RBH_IS_INVLAID(rbh) ){
//             list_del(&(rbh->li));
//             kfree(rbh);
//             continue;
//         }
//         inode = rbh->fmap->file->f_mapping->host;

//         if (inode_trylock(inode)) {
//             list_del(&(rbh->li));
//             if(RBH_IS_INVLAID(rbh)){
//                 kfree(rbh);
//             }else{
//                 if(RBH_IS_DIRTY(rbh)) {
//                     sync_rbh_wr_new(rbh);
//                 }
//                 rb_erase_and_free(rbh->fmap, &rbh->rb_node);
//             }
//             inode_unlock(inode);
//         }
//     }

//     // spin_unlock_irqrestore(&list_lock, flags);
// }



static long starting_iter = 1000000;
static const long iter_step = 50000;
static const long num_rbh_free_hi_threshold = 100000;

/**
 * @note caller needs to release lock before entering this function
 * @TODO: change llist to something that is append atomic and FIFO, now using FILO
 */
static void try_evict(struct llist_head* lh) {
    // go through inactive list and evict all pages
    struct llist_node *pos, *pos_next, *q;
    struct llist_node **pos_ptr;
    uint64_t flags, invalid;
    struct inode *inode;
    long iter_count = 0;
    rbh_t *rbh;
    fmapping_t *fmap_tmp;
    // long target_list_count;

    long dirty_down_sync = 0, num_rbh_delayed_free = 0, num_rbh_free = 0, num_fmap_free = 0;
    long listcount, tofreecount;
    int hold_post_ptr = 0;

    // listcount = listcount_cur.counter;
    // tofreecount = tofreecount_cur.counter;
    // bytefs_err("[Evict thread START] PG count %8ld LST count %8ld ToFree count %8ld Real %8d", 
    //         pagecount_cur.counter, listcount, tofreecount, listcount - tofreecount);

    // target_list_count = (long) listcount_cur.counter;
    iter_count = 0;
    pos_ptr = &lh->first;
    while (*pos_ptr != NULL && iter_count < starting_iter) {
        pos_ptr = &(*pos_ptr)->next;
        iter_count++;
    }
    // bytefs_log("[Evict thread] list head %d skipped", iter_count);
    while (*pos_ptr != NULL) {
        pos = *pos_ptr;
        pos_next = pos->next;
        hold_post_ptr = 0;

        // sufficient space left, exit
        if ((long) pagecount_cur.counter <= PAGE_CACHE_CAPACITY_LO_PAGE) {
            bytefs_log("[Evict thread END] page count satisfied: %ld", pagecount_cur.counter);
            break;
        }
        // // approaching list end
        // if (iter_count > target_list_count) {
        //     bytefs_log("[Evict thread END] list drained");
        //     break;
        // }

        rbh = get_rbh_from_li(pos);
        fmap_tmp = rbh->fmap;

        bytefs_assert(rbh);
        bytefs_assert(fmap_tmp);

        if (spin_trylock(&(fmap_tmp->fmap_evict_lock))) {
            if (down_write_trylock(&rbh->in_use)) {
                if (RBH_CAN_FREE(rbh)) {
                    bytefs_err("Wrong path");
                    // rbh is not used anymore, free it
                    num_rbh_delayed_free++;
                    atomic64_dec(&tofreecount_cur);
                    if (atomic64_dec_and_test(&rbh->fmap->remaining_refs)) {
                        // if no other references to fmap, free it (Should be only place to do it)
                        kfree(rbh->fmap);
                        rbh->fmap = NULL;
                        up_write(&rbh->in_use); //
                        kfree(rbh);
                        // fmap is already gone, no need to unlock
                        num_fmap_free++;
                    } else {
                        // only the rbh can be freed
                        up_write(&rbh->in_use); //
                        kfree(rbh);
                        spin_unlock(&fmap_tmp->fmap_evict_lock);
                    }
                } else {
                    if (rb_erase_from_tree(rbh->fmap, &rbh->rb_node) == 0) {
                        num_rbh_free++;
                        // if the rbh dirty, sync
                        if (RBH_IS_DIRTY(rbh)) {
                            dirty_down_sync++;
                            sync_rbh_wr_new(rbh);
                        }
                        rb_free_node(&rbh->rb_node);
                    }
                    up_write(&rbh->in_use);
                    spin_unlock(&fmap_tmp->fmap_evict_lock);
                }
                // remove rbh from list
                *pos_ptr = pos_next;      // no atomic needed, only this thread is performing list removal
                hold_post_ptr = 1;
                atomic64_dec(&listcount_cur);
            } else {
                spin_unlock(&fmap_tmp->fmap_evict_lock);
            }
        }

        iter_count++;
        if (hold_post_ptr == 0)
            pos_ptr = &pos->next;
    }

    // listcount = listcount_cur.counter;
    // tofreecount = tofreecount_cur.counter;
    // bytefs_err("[Evict thread END] PG count %8ld LST count %8ld ToFree count %8ld Real %8d", 
    //         pagecount_cur.counter, listcount, tofreecount, listcount - tofreecount);
    // bytefs_err("[Evict thread END] Dirty sync: %8ld Num rbh delayed free: %8ld",
    //         dirty_down_sync, num_rbh_delayed_free);
    // bytefs_err("[Evict thread END] Num rbh free: %8ld Num fmap free: %8ld", 
    //         num_rbh_free, num_fmap_free);
    bytefs_log("[Evict thread END] PG count %8ld Num iter: %8ld Num rbh free: %8ld", 
            pagecount_cur.counter, iter_count, num_rbh_free);

    if (num_rbh_free == 0) {
        starting_iter -= iter_step;
        bytefs_log("[Evict thread] evict step forward to %ld", starting_iter);
    } else if (num_rbh_free > num_rbh_free_hi_threshold) {
    	starting_iter += iter_step;
        bytefs_log("[Evict thread] evict step backward to %ld", starting_iter);
    }
}

// evict main thread
int evict_thread(void* arg) {
    printk(KERN_INFO "evict thread started");
    while (!kthread_should_stop()) {
        schedule();

        rw_block = pagecount_cur.counter > PAGE_CACHE_RW_BLOCK_THRESHOLD_PAGE;
        if (pagecount_cur.counter <= PAGE_CACHE_CAPACITY_HI_PAGE)
            continue;
            
        bytefs_log("[Evict thread] start operation");
        while (pagecount_cur.counter > PAGE_CACHE_CAPACITY_HI_PAGE) {
            try_evict(&global_inactive_list);
            rw_block = pagecount_cur.counter > PAGE_CACHE_RW_BLOCK_THRESHOLD_PAGE;
        }
        bytefs_log("[Evict thread] seize operation");
    }
    return 0;
}



int64_t bytefs_cache_init(void) {
    long tmp;

    // create thread here
    // initialize page cache
    init_llist_head(&global_active_list);
    init_llist_head(&global_inactive_list);

    thread_id = kzalloc(sizeof(struct task_struct), GFP_ATOMIC);
    thread_id = kthread_create(evict_thread, NULL, "evict_thread");
    kthread_bind(thread_id, 4);
    if (thread_id == 0)
        printk(KERN_ERR "Failed to create cache evict thread\n");
    else
        wake_up_process(thread_id);

    // printk(KERN_INFO "\n");
    inode_fmapping_arr = (fmapping_t **) vmalloc(sizeof(fmapping_t *) * MAX_INODE_NUM);
    if (inode_fmapping_arr == NULL)
        bytefs_err("Inode fmapping array allocation failed");
    else
        memset(inode_fmapping_arr, 0, sizeof(fmapping_t *) * MAX_INODE_NUM);

    bytefs_log("ByteFS cache init done");

    tmp = (1000 * PAGE_CACHE_CAPACITY_HI_PAGE * 4096) / (1024 * 1024);
    bytefs_log("Cache capacity hi: %15ld pg %10d.%03d MB", 
            PAGE_CACHE_CAPACITY_HI_PAGE, tmp / 1000, tmp % 1000);
    
    tmp = (1000 * PAGE_CACHE_CAPACITY_LO_PAGE * 4096) / (1024 * 1024);
    bytefs_log("Cache capacity lo: %15ld pg %10d.%03d MB", 
            PAGE_CACHE_CAPACITY_LO_PAGE, tmp / 1000, tmp % 1000);

    return 0;
}
EXPORT_SYMBOL(bytefs_cache_init);

int64_t bytefs_cache_reset(void) {
    // kill the thread first
    if (thread_id && !kthread_stop(thread_id)) {
        thread_id = 0;
        bytefs_log("cache evict thread stopped");
    } else {
        bytefs_err("cache evict thread stop failed");
    }

    // drain list
    bytefs_log("ByteFS reset drain active & inactive list");
    init_llist_head(&global_active_list);
    init_llist_head(&global_inactive_list);

    thread_id = kzalloc(sizeof(struct task_struct), GFP_ATOMIC);
    thread_id = kthread_create(evict_thread, NULL, "evict_thread");
    kthread_bind(thread_id, 4);
    if (thread_id == 0)
        printk(KERN_ERR "Failed to create FTL thread\n");
    else {
        wake_up_process(thread_id);
        bytefs_log("cache evict thread started");
    }
   
    bytefs_log("ByteFS cache reset done");
    return 0;
}
EXPORT_SYMBOL(bytefs_cache_reset);


int64_t bytefs_open_init(struct file* filp) {
    filp->private_data = kmalloc(sizeof(filp_private_t), GFP_ATOMIC);
    if (NULL == filp->private_data) {
        return -1;
    }
    if (-1 == bytefs_alloc_fmap(filp)) {
        printk(KERN_ERR "%s bytefs_alloc_fmap failed", __func__);
        return -1;
    }

    // rbh_t *pos, *q;
    // fmapping_t *fmap = ((filp_private_t*) (filp->private_data))->fmap;
    // int rbt_count = 0;
    // rbtree_postorder_for_each_entry_safe(pos, q, &(fmap->root), rb_node) {
    //     rbt_count++;
    // }
    // bytefs_err("rbt count: %d", rbt_count);

    return 0;
}
EXPORT_SYMBOL(bytefs_open_init);

/**
 * @brief bytefs version of getting a contiguous (with respect to file mapping) mapped pages,
 * starting from lpa @param st, ending at @param ed. Currently implementation is simply 
 * @return int64_t number of pages found. -1 on failure.
 */
int64_t bytefs_pgcache_rd(struct file* filp, uint64_t st, uint64_t ed, 
char *buf) {
    // note that in file_table.c, struct file is zeroed out when it is allocated

    int ret = 0;
    uint64_t flags;
    uint64_t i, off;
    struct rb_node* node;
    rbh_t* rbh = NULL;

    if (!buf) return -1;

    fmapping_t* fmap = ((filp_private_t*)(filp->private_data))->fmap;

    while (rw_block) schedule();
    
    // spin_lock_irqsave(&fmap->lock, flags);

    // every time try to serarh in rb tree.
    // if not found, we will alloc a new rbh and insert it in the rb tree, then copy it into buf
    // if found, we will copy it into buf

    /**
     * @todo we should have a lock
     */
    off = 0;
    for (i = st; i < ed; i = i / PG_SIZE * PG_SIZE + PG_SIZE) {
        uint64_t lpa = i / PG_SIZE * PG_SIZE;
        uint64_t l_padding, r_padding;

        l_padding = i % PG_SIZE;
        r_padding = (lpa + PG_SIZE) > ed ? (PG_SIZE - ed % PG_SIZE) : 0;

        // search in rb tree with key lpa
        if (down_read_trylock(&fmap->rbt_sem)) {
            node = rb_search(fmap, lpa);
            if (node) {
                rbh = get_rbh(node);
                if (down_read_trylock(&rbh->in_use) == 0)
                    node = NULL; // contension, the node is under cleanning now, mark as does not exist 
            }
            up_read(&fmap->rbt_sem);
        } else {
            bytefs_warn("rd fail");
            return -1;
        }
        // down_read(&fmap->rbt_sem);
        // // spin_lock(&fmap->rbt_lock);
        // node = rb_search(fmap, lpa);
        // if (node) {
        //     rbh = get_rbh(node);
        //     if (down_read_trylock(&rbh->in_use) == 0)
        //         node = NULL; // contension, the node is under cleanning now, mark as does not exist 
        // }
        // up_read(&fmap->rbt_sem);
        // spin_unlock(&fmap->rbt_lock);

        if (!node) {
            if (-1 == rb_tree_alloc_and_insert(&(fmap->root), lpa, &node, &global_inactive_list, fmap))
                goto rbt_insert_failed;
            rbh = get_rbh(node);
            fmap->page_count++;
            SSD_STAT_ATOMIC_INC(page_cache_rd_miss);
            // hit_cache(rbh, PG_SIZE, 1);
            // for read, we must sync the page if it's a hole
            if (0 > sync_rbh_rd(rbh)) 
                goto read_failed;
        } else {
            SSD_STAT_ATOMIC_INC(page_cache_rd_hit);
            // hit_cache(rbh, 0, 1);
        }
        // TODO: add this?
        bytefs_assert_msg(buf + off != 0, "RD Buf: %x, Off: %x, Buf + off: %x",
                buf, off, buf + off);
        if (NULL == memcpy(buf + off, (char*)(rbh->page) + l_padding, PG_SIZE - l_padding - r_padding)) 
            goto read_failed;
        
        up_read(&rbh->in_use);

        off += PG_SIZE - l_padding - r_padding;
        ret++;
    }
    // spin_unlock_irqrestore(&fmap->lock, flags);
    return ret;

rbt_insert_failed:
    printk(KERN_ERR "%s rb_tree_alloc_and_insert failed", __func__);
    return -1;

read_failed:
    up_read(&rbh->in_use);
    printk(KERN_ERR "%s memcpy failed", __func__);
    // spin_unlock_irqrestore(&fmap->lock, flags);
    return -1;
}
EXPORT_SYMBOL(bytefs_pgcache_rd);


/**
 * @brief Not only bytefs_find_get_pages_range, it also fill the content to write into holes.
 * Whenever we change a page from clean to dirty, we alloc_dup_page that page
 * This separate function is to reduce overhead.
 * @todo : result should be stored in somewhere pointed by parameter
 * @return int64_t 
 */
int64_t bytefs_pgcache_wr(struct file* filp, uint64_t st, uint64_t ed, 
char *buf) {
    int ret = 0;
    uint64_t flags;
    uint64_t i, off;
    struct rb_node* node;
    rbh_t* rbh = NULL;

    if (!buf) return -1;

    fmapping_t* fmap = ((filp_private_t*)(filp->private_data))->fmap;

    while (rw_block) schedule();

    // spin_lock_irqsave(&fmap->lock, flags);

    // every time try to serarh in rb tree.
    // if not found, we will alloc a new rbh and insert it in the rb tree, then copy it into buf
    // if found, we will copy it into buf
    off = 0;    
    for (i = st; i < ed; i = i / PG_SIZE * PG_SIZE + PG_SIZE) {
        uint64_t lpa = i / PG_SIZE * PG_SIZE;
        uint64_t l_padding, r_padding;

        l_padding = i % PG_SIZE;
        r_padding = (lpa + PG_SIZE) > ed ? (PG_SIZE - ed % PG_SIZE) : 0;

        // search in rb tree with key lpa
        if (down_read_trylock(&fmap->rbt_sem)) {
            node = rb_search(fmap, lpa);
            if (node) {
                rbh = get_rbh(node);
                if (down_read_trylock(&rbh->in_use) == 0)
                    node = NULL; // contension, the node is under cleanning now, mark as does not exist 
            }
            up_read(&fmap->rbt_sem);
        } else {
            bytefs_warn("wr fail");
            return -1;
        }
        // down_read(&fmap->rbt_sem);
        // // spin_lock(&fmap->rbt_lock);
        // node = rb_search(fmap, lpa);
        // if (node) {
        //     rbh = get_rbh(node);
        //     if (down_read_trylock(&rbh->in_use) == 0)
        //         node = NULL; // contension, the node is under cleanning now, mark as does not exist 
        // }
        // up_read(&fmap->rbt_sem);
        // spin_unlock(&fmap->rbt_lock);

        if (!node) {
            // does not exist in page cache, promote
            if (-1 == rb_tree_alloc_and_insert(&(fmap->root), lpa, &node, &global_inactive_list, fmap)) {
                printk(KERN_ERR "%s rb_tree_alloc_and_insert failed", __func__);
                // spin_unlock_irqrestore(&fmap->lock, flags);
                return -1;
            }
            rbh = get_rbh(node);
            fmap->page_count++;
            SSD_STAT_ATOMIC_INC(page_cache_wr_miss);
            // printk(KERN_ERR "%s l_padding %lu r_padding %lu st %lu ed %lu i %lu", __func__, l_padding, r_padding, st, ed, i);
            if (l_padding || r_padding) {
                // for write, we can overwrite the hole when we write a whole page
                // if (0 > sync_rbh_rd(rbh)) {
                //     goto write_failed;
                // }
            }
            // hit_cache(rbh, PG_SIZE, 1);
        } else {
            // printk(KERN_ERR "%s rbh found", __func__);
            SSD_STAT_ATOMIC_INC(page_cache_wr_hit);
            // hit_cache(rbh, 0, 1);
        }

    #if (PG_CACHE_METHOD == PG_CACHE_METHOD_NEW)
        if (!RBH_IS_DIRTY(rbh) && get_dup_page(rbh) != 0) {
            goto write_failed;
        }
    #endif
        // write initiated, page set dirty
        rbh->flags |= RBH_FLAGS_DIRTY;
        // TODO: add this?
        bytefs_assert_msg(buf + off != 0, "WR Buf: %x, Off: %x, Buf + off: %x",
                buf, off, buf + off);
        if (NULL == memcpy((char*)(rbh->page) + l_padding, buf + off, PG_SIZE - l_padding - r_padding))
            goto write_failed;
        up_read(&rbh->in_use);
        
        off += PG_SIZE - l_padding - r_padding;
        ret++;
    }
    // spin_unlock_irqrestore(&fmap->lock, flags);
    return ret;
write_failed:
    up_read(&rbh->in_use);
    printk(KERN_ERR "%s memcpy failed", __func__);
    // spin_unlock_irqrestore(&fmap->lock, flags);
    return -1;
}
EXPORT_SYMBOL(bytefs_pgcache_wr);


/**
 * @brief We flush the pages between @param st and @param ed.
 * @return int64_t 
*/
int64_t bytefs_page_cache_flush(struct file* filp, uint64_t st, uint64_t ed) {

#if (PG_CACHE_METHOD == PG_CACHE_METHOD_NEW)
    int ret = 0;
    uint64_t flags;
    uint64_t i;
    rbh_t* pos, *q;
    fmapping_t* fmap = ((filp_private_t*)(filp->private_data))->fmap;

    // spin_lock_irqsave(&fmap->lock, flags);
    // for(i = st; i < ed; i = i / PG_SIZE * PG_SIZE + PG_SIZE) {
    //     uint64_t lpa = i / PG_SIZE * PG_SIZE;
    //     uint64_t l_padding, r_padding;

    //     l_padding = i % PG_SIZE;
    //     r_padding = (lpa + PG_SIZE) > ed ? (PG_SIZE - ed % PG_SIZE) : 0;

    //     // search in rb tree with key lpa
    //     printk(KERN_INFO "search lpa %lu", lpa);
    //     struct rb_node* node = rb_search(fmap, lpa);
    //     if(NULL == node) {
    //         continue; // clean
    //     }
    //     printk(KERN_INFO "sync node");
    //     if(RBH_IS_DIRTY(get_rbh(node)) && 0 > sync_rbh_wr_new(get_rbh(node))) {
    //         printk(KERN_ERR "bad rbh write at %s", __func__);
    //         // spin_lock_irqsave(&fmap->lock, flags);
    //         continue;
    //     }
    //     get_rbh(node)->flags &= ~RBH_FLAGS_DIRTY;
    //     // spin_lock_irqsave(&fmap->lock, flags);
    //     ret++;
    // }

    // flush entire rb tree
    if (down_read_trylock(&fmap->rbt_sem) == 0)
        return 0;
    // spin_lock(&fmap->rbt_lock);
    rbtree_postorder_for_each_entry_safe(pos, q, &(fmap->root), rb_node) {
        if (RBH_IS_DIRTY(pos)) {
            sync_rbh_wr_new(pos);
        }
    }
    up_read(&fmap->rbt_sem);
    // spin_unlock(&fmap->rbt_lock);

    // printk(KERN_INFO "end flush");
    // spin_unlock_irqrestore(&fmap->lock, flags);
    return ret;
#else
    int ret = 0;
    uint64_t flags;
    uint64_t i;

    spin_lock_irqsave(&filp_lock, flags);
    
    if(!filp->private_data) { 
        filp->private_data = kmalloc(sizeof(filp_private_t), GFP_ATOMIC);
        if(NULL == filp->private_data || -1 == bytefs_alloc_fmap(filp)) {
            printk(KERN_ERR, "%s bytefs_alloc_fmap failed", __func__);
            spin_unlock_irqrestore(&filp_lock, flags);
    return -1;
        }
    }

    spin_unlock_irqrestore(&filp_lock, flags);
    // then the lock will be per filp

    fmapping_t* fmap = ((filp_private_t*)(filp->private_data))->fmap;

    spin_lock_irqsave(&fmap->lock, flags);


    for(i = st; i < ed; i = i / PG_SIZE * PG_SIZE + PG_SIZE) {
        uint64_t lpa = i / PG_SIZE * PG_SIZE;
        uint64_t l_padding, r_padding;

        l_padding = i % PG_SIZE;
        r_padding = (lpa + PG_SIZE) > ed ? (PG_SIZE - ed % PG_SIZE) : 0;

        // search in rb tree with key lpa
        struct rb_node* node = rb_search(fmap, lpa);
        if(NULL == node) {
            continue; // clean
        }
        if(RBH_IS_DIRTY(get_rbh(node)) && 0 > sync_rbh_wr(get_rbh(node))) {
            printk(KERN_ERR "bad rbh write at %s", __func__);
            continue;
        }
        ret++;
    }
    spin_unlock_irqrestore(&fmap->lock, flags);
    return ret;

#endif 
    
    printk(KERN_ERR, "BUG: function undefined");
    return -1;
}
EXPORT_SYMBOL(bytefs_page_cache_flush);



// void evict_file_cache(struct file* filp) {
//     uint64_t flags;
//     rbh_t* pos, *q;
//     fmapping_t* fmap;
//     struct inode* inode;

//     if(!filp->private_data) { 
//         return 0;   
//     }

//     fmap = ((filp_private_t*)(filp->private_data))->fmap;

//     // for (pos = rb_first(&(fmap->root)); pos != NULL; pos = next) {
//     //     next = rb_next(pos);
//     //     kfree(get_rbh(pos)); // assuming the node is embedded in a struct with a name of my_struct
//     // }

//     inode = filp->f_mapping->host;
//     inode_lock(inode);
    
//     rbtree_postorder_for_each_entry_safe(pos, q, &(fmap->root), rb_node) {
//         if(RBH_IS_DIRTY(pos)) {
//             sync_rbh_wr_new(pos);
//         }
        
//         if(pos->page){
//             kfree(pos->page);
//             pagecount_cur--;
//         }
//         if(pos->dup_page){
//             kfree(pos->dup_page);
//             pagecount_cur--;
//         }
//         // kfree(pos);
//         // pos->fmap = NULL;
//         pos->flags &= RBH_FLAGS_INVALID;
//         //  INVALID
//     }
//     kfree(fmap);
//     inode_unlock(inode);

//     return 0;
// }
// EXPORT_SYMBOL(evict_file_cache);


// recycle page cache entirely
void evict_file_cache(struct file* filp) {
    uint64_t flags, invalid;
    rbh_t* pos, *q;
    fmapping_t* fmap;
    struct inode* inode;

    if (!filp->private_data)
        return 0;   

    fmap = ((filp_private_t*)(filp->private_data))->fmap;

    spin_lock(&fmap->fmap_evict_lock);
    // down_read(&fmap->rbt_sem);
    // spin_lock(&fmap->rbt_lock);
    long rb_count = 0;
    // bytefs_err("Filp %x evict start", filp);

    // rbtree_postorder_for_each_entry_safe(pos, q, &(fmap->root), rb_node) {
    //     rb_count++;

    //     // if dirty write the page down
    //     if (RBH_IS_DIRTY(pos)) {
    //         sync_rbh_wr_new(pos);
    //     }
        
    //     // recycle any page allocated
    //     if (pos->page) {
    //         kfree(pos->page);
    //         pos->page = NULL;
    //         atomic64_dec(&pagecount_cur);
    //     }
    //     if (pos->dup_page) {
    //         kfree(pos->dup_page);
    //         pos->dup_page = NULL;
    //         atomic64_dec(&pagecount_cur);
    //     }
    //     // mark rbh as needed to be freed
    //     SET_CAN_FREE(pos);

    //     atomic64_inc(&tofreecount_cur);
    // }
    // bytefs_assert_msg(rb_count == fmap->remaining_refs.counter, 
    //         "rb_count: %ld remaining_refs: %ld", rb_count, fmap->remaining_refs.counter);

    // bytefs_err("Filp %x evict end", filp);
    // up_read(&fmap->rbt_sem);
    // spin_unlock(&fmap->rbt_lock);
    spin_unlock(&fmap->fmap_evict_lock);

    filp->private_data = NULL;
}
EXPORT_SYMBOL(evict_file_cache);


// void cow_unit_test(void) {
//     int i;
//     static int if_test = 0;
//     if(if_test) return;
//     if_test = 1;
//     printk(KERN_ERR, "cow unit test start");
//     struct file test_file;
//     test_file.private_data = NULL;
//     // test 1
//     // wrte 5 pages to test file from 2000 to 2000 + 5 * 4096
//     // then then read those pages

//     uint8_t* buf = kmalloc(5 * PG_SIZE, GFP_ATOMIC);
//     uint8_t* buf2 = kmalloc(5 * PG_SIZE, GFP_ATOMIC);

//     for(i = 0; i < 5 * PG_SIZE; i++) {
//         buf[i] = i;
//     }

//     bytefs_pgcache_wr(&test_file, buf, 2000, 5 * PG_SIZE);
//     bytefs_pgcache_rd(&test_file, buf2, 2000, 5 * PG_SIZE);

//     for(i = 0; i < 5 * PG_SIZE; i++) {
//         if(buf[i] != buf2[i]) {
//             printk(KERN_ERR, "cow unit test 1 failed");
//             return;
//         }
//     }

//     printk(KERN_ERR, "cow unit test 1 passed");

//     // test 2
//     // flush the page cache
//     // then read those pages

//     printk(KERN_ERR, "cow unit test 2 start");

//     bytefs_page_cache_flush(&test_file, 2000, 2000 + 5 * PG_SIZE);
//     bytefs_pgcache_rd(&test_file, buf2, 2000, 5 * PG_SIZE);

//     for(i = 0; i < 5 * PG_SIZE; i++) {
//         if(buf[i] != buf2[i]) {
//             printk(KERN_ERR, "cow unit test 2 failed");
//             return;
//         }
//     }

//     printk(KERN_ERR, "cow unit test 2 passed");
// }
