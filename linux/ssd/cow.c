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

#include "cow.h"
#include "ftl.h"
#include "ssd_stat.h"

// spinlock to protect filp allocation
static DEFINE_SPINLOCK(filp_lock);

static inline rbh_t* get_rbh(struct rb_node* node) {
    return container_of(node, rbh_t, rb_node);
}


static int64_t XOR_page(void) {

    
    printk(KERN_ERR, "BUG: function undefined");
    return -1;
}

static void _bytefs_init_pgcache(fmapping_t* fmap) {
    fmap->root = RB_ROOT;
    fmap->page_count = 0;
    // initialize spinlock for fmap
    spin_lock_init(&fmap->lock);
}

// allocate and initialize
static int64_t bytefs_alloc_fmap(struct file* filp) {
    // don't have page cache for this file
    fmapping_t* fmap = kmalloc(sizeof(fmapping_t), GFP_KERNEL);
    if (!fmap) {
        printk(KERN_ERR, "%s kmalloc failed", __func__);
        return -1;
    }
    _bytefs_init_pgcache(fmap);
    bytefs_wr_private(filp->private_data, fmap);
    return 0;
}

static rbh_t* bytefs_alloc_init_rbh(uint64_t lpa) {
    rbh_t* rbh = kmalloc(sizeof(rbh_t), GFP_KERNEL);
    if (!rbh) {
        printk(KERN_ERR, "%s kmalloc failed", __func__);
        return NULL;
    }
    rbh->lpa = lpa;
    rbh->page = bytefs_alloc_page(GFP_KERNEL);
    if (!rbh->page) {
        printk(KERN_ERR, "%s alloc_page failed", __func__);
        return NULL;
    }
    rbh->dup_page = NULL;
    rbh->flags = RBH_FLAGS_INIT;
    return rbh;
}

// sync rbh from disk 
static int64_t sync_rbh_rd(rbh_t* rbh) {
    if(NULL == rbh || NULL == rbh->page) return -1;
    // currently only use block issue to sync page
    rbh->flags &= (~RBH_FLAGS_DIRTY);
    return nvme_issue(0, rbh->lpa / PG_SIZE, 1, rbh->page);
}
// sync rbh to disk 
static int64_t sync_rbh_wr(rbh_t* rbh) {
    if(NULL == rbh || NULL == rbh->page) return -1;
    // currently only use block issue to sync page
    rbh->flags &= (~RBH_FLAGS_DIRTY);
    return nvme_issue(1, rbh->lpa / PG_SIZE, 1, rbh->page);
}

static int if_dirty_rbh(rbh_t* rbh) {
    if(NULL == rbh) {
        printk(KERN_ERR, "%s BUG: rbh is NULL", __func__);
        return -1;
    }
    return rbh->flags & RBH_FLAGS_DIRTY;
}

/**
 * @brief this function should only be called when the page with @param lpa is not in the cache.
 * @todo we should use a lock to protect the rb tree.
 * @return int64_t  0 if success, -1 if failed
 */
static int64_t rb_tree_alloc_and_insert(struct rb_root* root, uint64_t lpa, struct rb_node** node) {
    struct rb_node** new;
    struct rb_node* parent = NULL;
    int64_t ret = -1;

    if(NULL == root) return -1;

    new = &(root->rb_node);

    while(*new) {
        rbh_t* rbh = get_rbh(*new);
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
        rbh_t* rbh = bytefs_alloc_init_rbh(lpa);
        // add it in the rb tree
        rb_link_node(&(rbh->rb_node), parent, new);
        rb_insert_color(&(rbh->rb_node), root);
        *node = &(rbh->rb_node);
        
        ret = 0;
    } else {
        printk(KERN_ERR, "%s BUG: rbh already exists", __func__);
    }
    return ret;
}

static struct rb_node* rb_search(struct rb_root* root, uint64_t lpa) {
    struct rb_node* node = root->rb_node;
    while(node) {
        rbh_t* rbh = get_rbh(node);
        if(lpa < rbh->lpa) {
            node = node->rb_left;
        } else if(lpa > rbh->lpa) {
            node = node->rb_right;
        } else {
            return node;
        }
    }
    return NULL;
}
// free a rb tree node
static void rb_erase_and_free(struct rb_root* root, struct rb_node* node) {
    if(NULL == root || NULL == node){
        printk(KERN_ERR, "%s BUG: root or node is NULL", __func__);
        return;
    }
    rb_erase(node, root);
    rbh_t* rbh = get_rbh(node);
    if(rbh->page) {
        kfree(rbh->page);
    }
    if(rbh->dup_page) {
        kfree(rbh->dup_page);
    }
    kfree(rbh);
}




#if (PG_CACHE_METHOD == PG_CACHE_METHOD_OLD)
/**
 * @brief bytefs version of getting a contiguous (with respect to file mapping) mapped pages,
 * starting from lpa @param st, ending at @param ed. Currently implementation is simply 
 * @return int64_t number of pages found. -1 on failure.
 */
int64_t bytefs_pgcache_rd(struct file* filp, uint64_t st, uint64_t ed, 
char __user *buf) {
    // note that in file_table.c, struct file is zeroed out when it is allocated

    int ret = 0;
    uint64_t flags;
    uint64_t i, off;

    spin_lock_irqsave(&filp_lock, flags);

    if(!filp->private_data) { 
        filp->private_data = kmalloc(sizeof(filp_private_t), GFP_KERNEL);
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

    // every time try to serarh in rb tree.
    // if not found, we will alloc a new rbh and insert it in the rb tree, then copy it into buf
    // if found, we will copy it into buf

    /**
     * @todo we should have a lock
     */
    off = 0;
    for(i = st; i < ed; i = i / PG_SIZE * PG_SIZE + PG_SIZE) {
        uint64_t lpa = i / PG_SIZE * PG_SIZE;
        uint64_t l_padding, r_padding;

        l_padding = i % PG_SIZE;
        r_padding = (lpa + PG_SIZE) > ed ? (PG_SIZE - ed % PG_SIZE) : 0;

        // search in rb tree with key lpa
        struct rb_node* node = rb_search(&(fmap->root), lpa);
        if(NULL == node) {
            if(-1 == rb_tree_alloc_and_insert(&(fmap->root), lpa, &node)) {
                printk(KERN_ERR, "%s rb_tree_alloc_and_insert failed", __func__);
                spin_unlock_irqrestore(&fmap->lock, flags);
                return -1;
            }
            fmap->page_count++;
            SSD_STAT_ATOMIC_INC(page_cache_rd_miss);
            rbh_t* rbh = get_rbh(node);
            // for read, we must sync the page if it's a hole
            if(0 > sync_rbh_rd(rbh)) {
                printk(KERN_ERR, "%s sync_rbh_rd failed", __func__);
                return -1;
            }
        } else{
            SSD_STAT_ATOMIC_INC(page_cache_rd_hit);
        }
        rbh_t* rbh = get_rbh(node);
        // if(0 > memcpy(buf + off, 
        // (char*)(rbh->page) + l_padding, PG_SIZE - l_padding - r_padding)) {
        //     printk(KERN_ERR, "%s copy_to_user failed", __func__);
        //     spin_unlock_irqrestore(&fmap->lock, flags);
        //     return -1;
        // }
        off += PG_SIZE - l_padding - r_padding;
        ret++;
    }
    // printk(KERN_ERR "bytefs_pgcache_rd: %d pages found", fmap->page_count);
    spin_unlock_irqrestore(&fmap->lock, flags);
    return ret;
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
char __user *buf) {
    int ret = 0;
    uint64_t flags;
    uint64_t i, off;

    spin_lock_irqsave(&filp_lock, flags);

    if(!filp->private_data) { 
        filp->private_data = kmalloc(sizeof(filp_private_t), GFP_KERNEL);
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

    // every time try to serarh in rb tree.
    // if not found, we will alloc a new rbh and insert it in the rb tree, then copy it into buf
    // if found, we will copy it into buf
    off = 0;
    for(i = st; i < ed; i = i / PG_SIZE * PG_SIZE + PG_SIZE) {
        uint64_t lpa = i / PG_SIZE * PG_SIZE;
        uint64_t l_padding, r_padding;

        l_padding = i % PG_SIZE;
        r_padding = (lpa + PG_SIZE) > ed ? (PG_SIZE - ed % PG_SIZE) : 0;

        // search in rb tree with key lpa
        struct rb_node* node = rb_search(&(fmap->root), lpa);
        if(NULL == node) {
            if(-1 == rb_tree_alloc_and_insert(&(fmap->root), lpa, &node)) {
                printk(KERN_ERR, "%s rb_tree_alloc_and_insert failed", __func__);
                spin_unlock_irqrestore(&fmap->lock, flags);
                return -1;
            }
            fmap->page_count++;
            SSD_STAT_ATOMIC_INC(page_cache_wr_miss);
            if(l_padding || r_padding) {
                rbh_t* rbh = get_rbh(node);
                // for write, we can overwrite the hole when we write a whole page
                if(0 > sync_rbh_rd(rbh)) {
                    printk(KERN_ERR, "%s sync_rbh_rd failed", __func__);
                    return -1;
                }
            }
        }else {
            SSD_STAT_ATOMIC_INC(page_cache_wr_hit);
        }
        rbh_t* rbh = get_rbh(node);
        rbh->flags |= RBH_FLAGS_DIRTY;
        // if(0 > memcpy((char*)(rbh->page) + l_padding, buf + off, 
        // PG_SIZE - l_padding - r_padding)) {
        //     printk(KERN_ERR, "%s copy_to_user failed", __func__);
        //     spin_unlock_irqrestore(&fmap->lock, flags);
        //     return -1;
        // }
        off += PG_SIZE - l_padding - r_padding;
        ret++;
    }
    spin_unlock_irqrestore(&fmap->lock, flags);
    return ret;
}
EXPORT_SYMBOL(bytefs_pgcache_wr);


/**
 * @brief We flush the pages between @param st and @param ed.
 * @return int64_t 
 */
int64_t bytefs_page_cache_flush(struct file* filp, uint64_t st, uint64_t ed) {

#if (BYTE_BLOCK_MIX == 1)

#else
    int ret = 0;
    uint64_t flags;
    uint64_t i;

    spin_lock_irqsave(&filp_lock, flags);

    if(!filp->private_data) { 
        filp->private_data = kmalloc(sizeof(filp_private_t), GFP_KERNEL);
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
        struct rb_node* node = rb_search(&(fmap->root), lpa);
        if(NULL == node) {
            continue; // clean
        }
        if(0 > sync_rbh_wr(get_rbh(node))) {
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

#endif /**/
