/**
 * Emulate SSD cache(DRAM in SSD) and SSD flash storage.
*/
#include <linux/time.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/printk.h>
// #include <stdbool.h>
#include <linux/gfp.h>


#include "ftl.h"
#include "bytefs_utils.h"

/**
 * Memory Backend (mbe) for emulated SSD.
 * INPUT:
 * mbe - memory backend, a chunk of DRAM to emulate SSD cache and SSD disk storage.
 * nbytes - the number of bytes of such chunk
 * phy_loc - the location related to mbe where you want to put your emulated disk. (In current case, zero)
 * RETURN:
 * 0 on success
 * other on failure.
 * SIDE-EFFECT:
 * At least 4G memory is strictly allocated. 
 * Meaning to emulate this version of 2B-SSD, you need at least 6G simply for emulation usage. 
 * WARNING:
 * In GRUB, mmap=2G!(4G+kernel_memory) is needed. Any smaller DRAM configuration will lead to failure.
*/
int init_dram_backend(struct SsdDramBackend **mbe, size_t nbytes, phys_addr_t phy_loc)
{
    void* virt_loc;
    struct SsdDramBackend *b = *mbe = (struct SsdDramBackend*) kzalloc(sizeof(struct SsdDramBackend), GFP_KERNEL);
    if (!b) {
        bytefs_err("MTE mapping failure");
        goto failed;
    }

    b->size = nbytes;
    b->phy_loc = (void *) phy_loc;

    // if (mlock(b->logical_space, nbytes) == -1) {
        // femu_err("Failed to pin the memory backend to the host DRAM\n");
        // free(b->logical_space);
        // abort();
    // }
    //TODO init or check here?
    // void* virt_loc = request_mem_region_exclusive((phys_addr_t)phy_loc,nbytes,"bytefs_be");

    // use malloc for non kernel version
    // void* virt_loc = kmalloc(nbytes+CACHE_SIZE);
    // if(!virt_loc){
    //     printk(KERN_ERR "phy_loc_exclusive_failure\n");
    //     // goto failed;
    // }
    // printk(KERN_ERR "phy_loc %llu\n",phy_loc);
    // virt_loc = ioremap_cache(phy_loc,nbytes); //@TODO check this
    // init_dram_space(phy_loc,virt_loc,nbytes);

    virt_loc = vmalloc(ALL_TOTAL_SIZE);
    if (!virt_loc) {
        bytefs_err("MTE mapping failure");
        goto failed;
    }

    b->virt_loc = virt_loc;
    bytefs_log("vir_loc %llu", (uint64_t) virt_loc);
    return 0;
failed:
    return -1;
}


void free_dram_backend(struct SsdDramBackend *b)
{
    // TODO no need for free?

    // if (b->logical_space) {
    //     munlock(b->logical_space, b->size);
    //     g_free(b->logical_space);
    // }

    // no need to use this in non-kernel version
    // iounmap(b->virt_loc);

    kfree(b->virt_loc);
    kfree(b);
    return;
}

/*
* backend page read/write one page
* INPUT:
*     b        - SSD Ram Backend structure : recording the emulated DRAM physical starting address and corresponding virtual address
*                in host's DRAM (for emulation purpose).
*     ppa      - Starting address, unit is ppa. ppa is Physical Page Address : unit is 4KB DRAM chunk.
*     data     - for read : source; for write : destination
*     is_write - r/w
* RETURN:
*     always 0
* SIDE-EFFECT  - overwrite one page starting from 
*                data/ppa
*/
int backend_rw(struct SsdDramBackend *b, unsigned long ppa, void* data, bool is_write)
{
    void* mb = b->virt_loc;
    void* page_addr = mb + (ppa * PG_SIZE);
    if ((uint64_t) ppa > TOTAL_SIZE - PG_SIZE) {
        bytefs_err("BE RW Invalid PPN: %lX\n", ppa);
        return -1;
    }

    if (is_write) {
        memcpy(page_addr, data, PG_SIZE);
    } else {
        memcpy(data, page_addr, PG_SIZE);
    }
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    
    return 0;
}

/**
 * return dram translated location. Cache is in 2GB~4GB in current simulated version, and always in TOTAL_SIZE ~ TOTAL_SIZE + CACHE_SIZE.
 * INPUT : 
 * b - backend SSD storage. b->virt_loc is the locaion in memory, and it's the starting address for backend.
 * off - offset to which the reqeusted position relative to cache.
 * RETURN:
 * address in host's DRAM.
 */
void *cache_mapped(struct SsdDramBackend *b, unsigned long off) {
    bytefs_assert(off < CACHE_SIZE);
    return b->virt_loc + TOTAL_SIZE + off;
}


/*
* cache read/write size bytes
* INPUT:
*     b        - SSD Ram Backend structure : recording the emulated DRAM physical starting address and corresponding virtual address
*                in host's DRAM (for emulation purpose).
*     off      - offset to the starting address
*     data     - for read : source; for write : destination
*     is_write - r/w
* RETURN:
*     always 0
* SIDE-EFFECT  - overwrite one page starting from 
*                data/ppa 
*/
int cache_rw(struct SsdDramBackend *b, unsigned long off, void* data, bool is_write,unsigned long size)
{
    void* page_addr = cache_mapped(b, off);
    if (off + size > CACHE_SIZE) return -1;

    if (is_write) {
        memcpy(page_addr, data, size);
    } else {
        memcpy(data, page_addr, size);
    }
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    
    return 0;
}



// for test only
// int main( ){
//     // init dram backend
//     struct SsdDramBackend *b;
//     size_t bytes = TOTAL_SIZE;
//     init_dram_backend(&b, bytes, (void*)0x0000000000);
//     printf("init dram backend done\n");

//     // write page to dram
//     char page[1024*16] = "hello world";
//     backend_rw(b,0,page,1);
//     printf("write to dram done\n");

//     // read from dram
//     char* data2 = (char*)malloc(PG_SIZE);
//     backend_rw(b,0,data2,0);
//     printf("read from dram done\n");
//     printf("data2: %s\n",data2);

//     // free dram backend
//     free_dram_backend(b);
//     printf("free dram backend done\n");

//     return 0;
// }