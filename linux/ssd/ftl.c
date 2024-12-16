#include "ftl.h"
#include "bytefs.h"

#include "backend.h"
#include "bytefs_heap.h"
#include "bytefs_utils.h"
#include "../block/ssd/bytefs_debug_bio.h"
#include "ssd_stat.h"
#include <linux/mutex.h>

struct ssd gdev;
int inited_flag = 0;

// static struct hrtimer ftl_timer;
uint64_t start, cur;

// lock for accessing
DEFINE_MUTEX(mapping_table_mutex);
DEFINE_MUTEX(log_tail_mutex);
DEFINE_MUTEX(log_size_mutex);
DEFINE_MUTEX(log_page_buffer_mutex);
/**
 * Modifying the log tail would require a lock bc there could be concurrent writes to the log 
 * region at the same time. However, modifying the log head does not require a lock as the
 * log head is only advanced when there is log coalescing and flushing event triggered. Now log
 * coalescing and flushing procedure is moved to ftl_thread, means there would be no race 
 * condition happing, thus no lock associated.
*/

/* testing macro to reduce printk */
#define TEST_FTL_NODEBUG    0
#define TEST_FTL_FILL_LOG   1
#define TEST_FTL_SHOW_SIZE  2
#define TEST_FTL_NEW_BASE   4
#define TEST_FTL_DEBUG      TEST_FTL_NODEBUG

// this is a naive initialization of the nvme cmd, we just put addr at prp1
void bytefs_init_nvme(NvmeCmd* req, int op, uint64_t lba, uint32_t nlb, void* addr) {
    req->opcode = op;
    req->fuse = 0;
    req->psdt = 0;
    req->cid = 0;
    req->nsid = 1;
    req->mptr = 0;
    req->dptr.prp1 = (uint64_t)addr;
    req->dptr.prp2 = 0;
    req->cdw10 = lba;
    req->cdw11 = 0;
    req->cdw12 = nlb;
    req->cdw13 = 0;
    req->cdw14 = 0;
    req->cdw15 = 0;
}


static void uint2timespec(uint64_t time, struct timespec64 *t)
{
    t->tv_nsec = time % 1000000000UL;
    t->tv_sec = (time - t->tv_nsec) / 1000000000UL;
}



static void sleepns(uint64_t ns) {
    struct timespec64 abs_wait_barrier;
    // ktime_get_ts64(&abs_wait_barrier);
    uint2timespec(ns, &abs_wait_barrier);
    // clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &abs_wait_barrier, NULL);
    hrtimer_nanosleep(&abs_wait_barrier, HRTIMER_MODE_REL, CLOCK_MONOTONIC);
}

/**
 * get the physical address of the page
 * @param lpn: logical page number
 * @param ssd: ssd structure
 * @return: physical address of the page by struct ppa
 */
inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn)
{
    // @TODO make this number less if add gc
    bytefs_assert_msg(lpn < ssd->sp.tt_pgs,
            "LPN: %lld exceeds #tt_pgs: %d", lpn, ssd->sp.tt_pgs);
    return ssd->maptbl[lpn];
}

/**
 * set the physical address of the page
 */
inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    bytefs_assert_msg(lpn < ssd->sp.tt_pgs,
            "LPN: %lld exceeds #tt_pgs: %d", lpn, ssd->sp.tt_pgs);

    ssd->maptbl[lpn] = *ppa;
}

/**
 * get the realppa (physical page index) of the page
 *
 */
void ppa2pgidx(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;

    ppa->realppa = ppa->g.ch  * spp->pgs_per_ch  + \
                   ppa->g.lun * spp->pgs_per_lun + \
                   ppa->g.blk * spp->pgs_per_blk + \
                   ppa->g.pg;
    bytefs_assert_msg(ppa->realppa < spp->tt_pgs,
            "PPA: %lld exceeds #tt_pgs: %d", ppa->realppa, ssd->sp.tt_pgs);
}

void pgidx2ppa(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t pgidx = ppa->realppa;
    bytefs_assert_msg(ppa->realppa < spp->tt_pgs,
            "PPA: %lld exceeds #tt_pgs: %d", ppa->realppa, ssd->sp.tt_pgs);

    ppa->g.ch = pgidx / spp->pgs_per_ch;
    pgidx %= spp->pgs_per_ch;
    ppa->g.lun = pgidx / spp->pgs_per_lun;
    pgidx %= spp->pgs_per_lun;
    ppa->g.blk = pgidx / spp->pgs_per_blk;
    pgidx %= spp->pgs_per_blk;
    ppa->g.pg = pgidx;
}

inline uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa)
{
    ppa2pgidx(ssd, ppa);
    return ssd->rmap[ppa->realppa];
}

/* set rmap[page_no(ppa)] -> lpn */
inline void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    ppa2pgidx(ssd, ppa);
    ssd->rmap[ppa->realppa] = lpn;
}


static void ssd_init_write_pointer(struct ssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp;
    wpp->next_ch = 0;
    wpp->blk_ptr = bytefs_get_next_free_blk(ssd, &wpp->next_ch);
    wpp->ch = wpp->blk_ptr->ch_idx;
    wpp->lun = wpp->blk_ptr->way_idx;
    wpp->blk = wpp->blk_ptr->blk_idx;
    wpp->pg = 0;
    wpp->blk_ptr->wp = 0;

}

static inline void check_addr(int a, int max)
{
    bytefs_assert(a >= 0 && a < max);
}

// rewrite advance write pointer to block granularity
void ssd_advance_write_pointer(struct ssd *ssd)
{
    uint64_t gc_start_time, gc_end_time;
    // static int req_cnt = 0;
    // req_cnt++;
    // printk(KERN_ERR "REQ CNT %d\n", req_cnt);
    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *wpp = &ssd->wp;
    // struct line_mgmt *lm = &ssd->lm;


    check_addr(wpp->ch, spp->nchs);
    wpp->pg++;
    // check if we reached last page in block
    if (wpp->pg == spp->pgs_per_blk) {
        // move wp to the next block
        wpp->blk_ptr = bytefs_get_next_free_blk(ssd, &wpp->next_ch);
        bytefs_assert(wpp->blk_ptr);
        wpp->ch = wpp->blk_ptr->ch_idx;
        wpp->lun = wpp->blk_ptr->way_idx;
        wpp->blk = wpp->blk_ptr->blk_idx;
        wpp->pg = 0;
        wpp->blk_ptr->wp = 0;
        // gc if needed
        if (bytefs_should_start_gc(ssd)) {
            gc_start_time = get_time_ns();
            bytefs_log("GC starts @ %20lld w/capacity = %d/%d",
                gc_start_time,
                ssd->total_free_blks, ssd->sp.blks_per_ch * ssd->sp.nchs);
            bytefs_gc(ssd);
            gc_end_time = get_time_ns();
            bytefs_log("GC ends   @ %20lld w/capacity = %d/%d (duration: %lld ns)",
                gc_end_time,
                ssd->total_free_blks, ssd->sp.blks_per_ch * ssd->sp.nchs,
                gc_end_time - gc_start_time);
        }
    } else {
        // move to next page
        wpp->blk_ptr->wp++;
    }
    // printf("wpp->blk_ptr->wp: %d, wpp->ch:%d, wpp->lun:%d, wpp->blk:%d, wpp->pg:%d", wpp->blk_ptr->wp, wpp->ch, wpp->lun, wpp->blk, wpp->pg);

    bytefs_assert(wpp->pg < spp->pgs_per_blk);
    bytefs_assert(wpp->lun < spp->luns_per_ch);
    bytefs_assert(wpp->ch < spp->nchs);
    bytefs_assert(wpp->blk < spp->blks_per_lun);

    bytefs_assert(wpp->blk_ptr->wp == wpp->pg);
    // bytefs_assert(wpp->blk_ptr == &(ssd->ch[wpp->ch].lun[wpp->lun].blk[wpp->blk]));
    // printk(KERN_ERR "REQ CNT %d\n", req_cnt);
}


struct ppa get_new_page(struct ssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp;
    struct ppa ppa;
    ppa.realppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.blk = wpp->blk;
    ppa.g.pg = wpp->pg;
    ppa2pgidx(ssd, &ppa);
    return ppa;
}

static void check_params(struct ssdparams *spp)
{

}

static void ssd_init_params(struct ssdparams *spp)
{

    spp->pgsz = PG_SIZE;

    spp->pgs_per_blk = PG_COUNT;
    spp->blks_per_lun = BLOCK_COUNT;
    spp->luns_per_ch = WAY_COUNT;
    spp->nchs = CH_COUNT;

    spp->pg_rd_lat = NAND_READ_LATENCY;
    spp->pg_wr_lat = NAND_PROG_LATENCY;
    spp->blk_er_lat = NAND_BLOCK_ERASE_LATENCY;
    spp->ch_xfer_lat = CHNL_TRANSFER_LATENCY_NS;

    spp->pgs_per_lun = spp->pgs_per_blk * spp->blks_per_lun;
    spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
    spp->tt_pgs = spp-> pgs_per_ch * spp->nchs;


    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;
    spp->tt_luns = spp->luns_per_ch * spp->nchs;

    spp->num_poller = NUM_POLLERS;

    // GC related
    // spp->gc_thres_pcent = 0.75;
    // spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
    // spp->gc_thres_pcent_high = 0.95;
    // spp->gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->tt_lines);
    // spp->enable_gc_delay = true;

    check_params(spp);
}


static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp)
{
    int i = 0;
    blk->npgs = spp->pgs_per_blk;
    blk->pg = kzalloc(sizeof(struct nand_page) * blk->npgs, GFP_KERNEL);
    for (i = 0; i < blk->npgs; i++) {
        blk->pg[i].pg_num = i;
        blk->pg[i].status = PG_FREE;
    }
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt = 0;
    blk->wp = 0;
}


static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
    int i;
    lun->nblks = spp->blks_per_lun;
    lun->blk = kzalloc(sizeof(struct nand_block) * lun->nblks, GFP_KERNEL);
    for (i = 0; i < lun->nblks; i++) {
        ssd_init_nand_blk(&lun->blk[i], spp);
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;
}

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
{
    int i;
    ch->nluns = spp->luns_per_ch;
    ch->lun = kzalloc(sizeof(struct nand_lun) * ch->nluns, GFP_KERNEL);
    //printk(KERN_ERR "ch->nluns = %x",ch->lun);
    for (i = 0; i < ch->nluns; i++) {
        ssd_init_nand_lun(&ch->lun[i], spp);
    }
    ch->next_ch_avail_time = 0;
    ch->busy = 0;
}

static void ssd_init_maptbl(struct ssd *ssd)
{
    int i;
    struct ssdparams *spp = &ssd->sp;
    //printk(KERN_ERR "%llu",sizeof(struct ppa) * spp->tt_pgs);
    unsigned int size = sizeof(struct ppa) * spp->tt_pgs;
    ssd->maptbl = vmalloc(size);
    //printk(KERN_ERR "ssd->maptbl = %x",ssd->maptbl);
    for (i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].realppa = UNMAPPED_PPA;
    }
}

static void ssd_init_rmap(struct ssd *ssd)
{
    int i;
    struct ssdparams *spp = &ssd->sp;
    //printk(KERN_ERR "%llu",sizeof(struct ppa) * spp->tt_pgs);
    unsigned int size = sizeof(struct ppa) * spp->tt_pgs;
    ssd->rmap = vmalloc(size);
    // ssd->rmap = kzalloc(sizeof(uint64_t) * spp->tt_pgs,GFP_KERNEL);
    for (i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
}



static void ssd_init_queues(struct ssd *ssd)
{
    int i;
    struct ssdparams *spp = &ssd->sp;

    ssd->to_poller = kcalloc(spp->num_poller + 1, sizeof(void*), GFP_KERNEL);
    ssd->to_ftl = kcalloc(spp->num_poller + 1, sizeof(void*), GFP_KERNEL);

    //@TODO check i = 1 here !!!
    for (i = 1; i <= spp->num_poller; i++) {
        ssd->to_poller[i] = ring_alloc(MAX_REQ, 1);
        ssd->to_ftl[i] = ring_alloc(MAX_REQ, 1);
    }

}



void ssd_end(void) {
    // struct ssd *ssd = &gdev;
    // pthread_join(ssd->thread_id, NULL);
    return;

}

static inline bool valid_ppa(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    // int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    // int sec = ppa->g.sec;

    if ( ch >= 0 && ch < spp->nchs
            && lun >= 0 && lun < spp->luns_per_ch
            // && pl >=   0 && pl < spp->pls_per_lun
            && blk >= 0 && blk < spp->blks_per_lun
            && pg >= 0 && pg < spp->pgs_per_blk
            // && sec >= 0 && sec < spp->secs_per_pg
       )
        return true;

    return false;
}

// maybe changed according to GC?
static inline bool valid_lpn(struct ssd *ssd, uint64_t lpn)
{
    return (lpn < ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->realppa == UNMAPPED_PPA);
}

static inline struct ssd_channel *get_ch(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->ch[ppa->g.ch]);
}

static inline struct nand_lun *get_lun(struct ssd *ssd, struct ppa *ppa)
{
    struct ssd_channel *ch = get_ch(ssd, ppa);
    return &(ch->lun[ppa->g.lun]);
}



static inline struct nand_block *get_blk(struct ssd *ssd, struct ppa *ppa)
{
    // struct nand_plane *pl = get_pl(ssd, ppa);
    struct nand_lun *lun = get_lun(ssd, ppa);
    return &(lun->blk[ppa->g.blk]);
}



static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = get_blk(ssd, ppa);
    return &(blk->pg[ppa->g.pg]);
}



/** key function that calculates latency */
uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct nand_cmd *ncmd)
{
    int c = ncmd->cmd;
    uint64_t cmd_stime, nand_stime;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lun = get_lun(ssd, ppa);
    uint64_t lat = 0;

    cur = get_time_ns();
    cmd_stime = (ncmd->stime == 0) ? \
                         cur :  ncmd->stime;

    switch (c) {
    case NAND_READ:
        /* read: perform NAND cmd first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;
        lat = lun->next_lun_avail_time - cmd_stime;

        break;

    case NAND_WRITE:
        /* write: transfer data through channel first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        if (ncmd->type == USER_IO) {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        } else {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        }
        lat = lun->next_lun_avail_time - cmd_stime;


        break;

    case NAND_ERASE:
        /* erase: only need to advance NAND status */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->blk_er_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
        break;

    default:
        bytefs_err("Unsupported NAND command: 0x%x\n", c);
    }

    return lat;
}

/* update SSD status about one page from PG_VALID -> PG_INVALID */
void mark_page_invalid(struct ssd *ssd, struct ppa *ppa)
{
    // printf("PPA MARK INVALID %lX\n", ppa->realppa);
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;


    /* update corresponding page status */
    pg = get_pg(ssd, ppa);
    bytefs_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    bytefs_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    bytefs_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;


}


/* mark current page as valid */
void mark_page_valid(struct ssd *ssd, struct ppa *ppa)
{
    // printf("PPA MARK VALID %lX\n", ppa->realppa);
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    // struct line *line;

    /* update page status */
    pg = get_pg(ssd, ppa);
    bytefs_assert_msg(pg->status == PG_FREE, "Real status: %s",
            pg->status == PG_VALID ? "VALID" : "INVALID");
    if (pg->status != PG_FREE) dump_stack();
    pg->status = PG_VALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    bytefs_assert(blk->vpc >= 0 || blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc++;

}


void mark_block_free(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = get_blk(ssd, ppa);
    struct nand_page *pg = NULL;
    int i;
    for (i = 0; i < spp->pgs_per_blk; i++) {
        /* reset page status */
        pg = &blk->pg[i];
        // bytefs_assert(pg->nsecs == spp->secs_per_pg);
        pg->status = PG_FREE;
    }

    /* reset block status */
    bytefs_assert(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt++;

    blk->is_candidate = 0;
}

// static void gc_read_page(struct ssd *ssd, struct ppa *ppa)
// {
//     /* advance ssd status, we don't care about how long it takes */
//     if (ssd->sp.enable_gc_delay) {
//         struct nand_cmd gcr;
//         gcr.type = GC_IO;
//         gcr.cmd = NAND_READ;
//         gcr.stime = 0;
//         ssd_advance_status(ssd, ppa, &gcr);
//     }
// }

// /* move valid page data (already in DRAM) from victim line to a new page */
// static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa)
// {
//     struct ppa new_ppa;
//     struct nand_lun *new_lun;
//     uint64_t lpn = get_rmap_ent(ssd, old_ppa);

//     bytefs_assert(valid_lpn(ssd, lpn));
//     new_ppa = get_new_page(ssd);
//     /* update maptbl */
//     set_maptbl_ent(ssd, lpn, &new_ppa);
//     /* update rmap */
//     set_rmap_ent(ssd, lpn, &new_ppa);

//     mark_page_valid(ssd, &new_ppa);

//     /* need to advance the write pointer here */
//     ssd_advance_write_pointer(ssd);

//     if (ssd->sp.enable_gc_delay) {
//         struct nand_cmd gcw;
//         gcw.type = GC_IO;
//         gcw.cmd = NAND_WRITE;
//         gcw.stime = 0;
//         ssd_advance_status(ssd, &new_ppa, &gcw);
//     }

//     /* advance per-ch gc_endtime as well */
// #if 0
//     new_ch = get_ch(ssd, &new_ppa);
//     new_ch->gc_endtime = new_ch->next_ch_avail_time;
// #endif

//     new_lun = get_lun(ssd, &new_ppa);
//     new_lun->gc_endtime = new_lun->next_lun_avail_time;

//     return 0;
// }



/* here ppa identifies the block we want to clean */
// static void clean_one_block(struct ssd *ssd, struct ppa *ppa)
// {
//     struct ssdparams *spp = &ssd->sp;
//     struct nand_page *pg_iter = NULL;
//     int cnt = 0;

//     for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
//         ppa->g.pg = pg;
//         pg_iter = get_pg(ssd, ppa);
//         /* there shouldn't be any free page in victim blocks */
//         bytefs_assert(pg_iter->status != PG_FREE);
//         if (pg_iter->status == PG_VALID) {
//             gc_read_page(ssd, ppa);
//             /* delay the maptbl update until "write" happens */
//             gc_write_page(ssd, ppa);
//             cnt++;
//         }
//     }

//     bytefs_assert(get_blk(ssd, ppa)->vpc == cnt);
// }



// static int do_gc(struct ssd *ssd, bool force)
// {
//     struct line *victim_line = NULL;
//     struct ssdparams *spp = &ssd->sp;
//     struct nand_lun *lunp;
//     struct ppa ppa;
//     int ch, lun;

//     victim_line = select_victim_line(ssd, force);
//     if (!victim_line) {
//         return -1;
//     }

//     ppa.g.blk = victim_line->id;
//     ftl_debug("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
//               victim_line->ipc, ssd->lm.victim_line_cnt, ssd->lm.full_line_cnt,
//               ssd->lm.free_line_cnt);

//     /* copy back valid data */
//     for (ch = 0; ch < spp->nchs; ch++) {
//         for (lun = 0; lun < spp->luns_per_ch; lun++) {
//             ppa.g.ch = ch;
//             ppa.g.lun = lun;
//             ppa.g.pl = 0;
//             lunp = get_lun(ssd, &ppa);
//             clean_one_block(ssd, &ppa);
//             mark_block_free(ssd, &ppa);

//             if (spp->enable_gc_delay) {
//                 struct nand_cmd gce;
//                 gce.type = GC_IO;
//                 gce.cmd = NAND_ERASE;
//                 gce.stime = 0;
//                 ssd_advance_status(ssd, &ppa, &gce);
//             }

//             lunp->gc_endtime = lunp->next_lun_avail_time;
//         }
//     }

//     /* update line status */
//     mark_line_free(ssd, &ppa);

//     return 0;
// }

// 2bssd buffer implementation  //
static void ssd_init_bytefs_buffer(struct ssd *ssd) {
    int i;
    // struct ssdparams *spp = &ssd->sp;

    // memset the buffer region and map them
    ssd->bytefs_log_region_start = cache_mapped(ssd->bd, BYTEFS_LOG_REGION_START);
    ssd->bytefs_log_region_end = cache_mapped(ssd->bd, BYTEFS_LOG_REGION_END);
    ssd->indirection_mt = (struct indirection_mte *) cache_mapped(ssd->bd, BYTEFS_INDIRECTION_MT_START);
    ssd->coalescing_mt = (struct coalescing_mte *) cache_mapped(ssd->bd, BYTEFS_COALESCING_MT_START);
    ssd->log_page_buffer = kzalloc(PG_SIZE, GFP_KERNEL);
    ssd->flush_page_buffer = kzalloc(PG_SIZE, GFP_KERNEL);

    memset(ssd->bytefs_log_region_start, 0, BYTEFS_LOG_REGION_SIZE);

    ssd->imt_size = 0;
    ssd->cmt_size = 0;
    for (i = 0; i < BYTEFS_MT_SIZE; i++) {
        ssd->indirection_mt[i].lpa = INVALID_LPN;
        ssd->indirection_mt[i].log_offset = INVALID_LOG_OFFSET;
        ssd->indirection_mt[i].psl = 0;
        ssd->coalescing_mt[i].lpn = INVALID_LPN;
        ssd->coalescing_mt[i].bitmap = 0;
        ssd->coalescing_mt[i].psl = 0;
    }

    ssd->log_rp = (struct bytefs_log_metadata *) ssd->bytefs_log_region_start;
    ssd->log_wp = (struct bytefs_log_metadata *) ssd->bytefs_log_region_start;
    ssd->log_size = 0;

    mutex_init(&mapping_table_mutex);
    mutex_init(&log_tail_mutex);
    mutex_init(&log_size_mutex);
    mutex_init(&log_page_buffer_mutex);
}

static void validate_log_head(struct ssd *ssd) {
    // only log rp is involved in the function, no lock is required
    uint64_t log_delta_size;
    // reset read pointer to the head if the following structure is invalid
    if ((uint64_t) ssd->log_rp + sizeof(struct bytefs_log_metadata) >= (uint64_t) ssd->bytefs_log_region_end) {
        ssd->log_rp = (struct bytefs_log_metadata *) ssd->bytefs_log_region_start;
    } else if (ssd->log_rp->lpa == INVALID_LPN) {
        bytefs_assert(ssd->log_rp->data_len == INVALID_UINT16_T);
        bytefs_assert(ssd->log_rp->flag == INVALID_UINT8_T);
        ssd->log_rp = (struct bytefs_log_metadata *) ssd->bytefs_log_region_start;
    }
    // make sure the log entry is valid, does not span across the log end boarder
    log_delta_size = sizeof(struct bytefs_log_metadata) + ssd->log_rp->data_len;
    bytefs_assert((uint64_t) ssd->log_rp + log_delta_size <= (uint64_t) ssd->bytefs_log_region_end);
}

static void advance_log_head(struct ssd *ssd) {
    // NOTE: make sure validate_log_head is called before this function to allow correct
    //       removal of log entries.
    // already at next log read pointer
    uint64_t log_delta_size = sizeof(struct bytefs_log_metadata) + ssd->log_rp->data_len;
    void *next_log_rp = (void *) ssd->log_rp + log_delta_size;
    // bytefs_log("Log rp lpa %lld size %lld flag %lld", 
    //         ssd->log_rp->lpa, ssd->log_rp->data_len, ssd->log_rp->flag);
    bytefs_assert(ssd->log_rp->data_len >= 0);

    // editing the log size would require a lock
    mutex_lock(&log_size_mutex);
    bytefs_assert(ssd->log_size >= log_delta_size);
    ssd->log_size -= log_delta_size;
    mutex_unlock(&log_size_mutex);

    // advnace log read pointer
    ssd->log_rp = (struct bytefs_log_metadata *) next_log_rp;
}

static void advance_log_tail(struct ssd *ssd, uint64_t lpa, uint64_t data_len, void *data) {
    // get next log write pointer
    uint64_t data_offset;
    struct bytefs_log_metadata *this_log_wp;
    struct coalescing_mte *cmte;
    void *next_log_wp;
    uint64_t fill_size = 0;
    uint64_t log_delta_size = sizeof(struct bytefs_log_metadata) + data_len;
    uint64_t start_blocking_time = 0, end_blocking_time = 0;
    bytefs_assert(lpa % BYTEFS_LOG_REGION_GRANDULARITY == 0);
    bytefs_assert(data_len % BYTEFS_LOG_REGION_GRANDULARITY == 0);
    bytefs_assert(data_len > 0);

    // get current log tail and advance for the next write
    mutex_lock(&log_tail_mutex);
    this_log_wp = ssd->log_wp;
    next_log_wp = (void *) this_log_wp + log_delta_size;
    if (next_log_wp > ssd->bytefs_log_region_end) {
        fill_size = (uint64_t) ssd->bytefs_log_region_end - (uint64_t) this_log_wp;
        fill_size = fill_size > sizeof(struct bytefs_log_metadata) ? 
                    sizeof(struct bytefs_log_metadata) : fill_size;
        memset((void *) this_log_wp, INVALID_UINT8_T, fill_size);
        this_log_wp = (struct bytefs_log_metadata *) ssd->bytefs_log_region_start;
        next_log_wp = (void *) this_log_wp + log_delta_size;
    }
    // clear out the flag before size increase and wp advance to prevent flush procedure from
    // incorrectly regard the entry as flushable
    this_log_wp->flag = 0;
    // advnace log write pointer
    ssd->log_wp = (struct bytefs_log_metadata *) next_log_wp;
    while (1) {
        if (ssd->log_size + log_delta_size <= BYTEFS_LOG_REGION_SIZE) {
            mutex_lock(&log_size_mutex);
            if (ssd->log_size + log_delta_size <= BYTEFS_LOG_REGION_SIZE)
                break;
            mutex_unlock(&log_size_mutex);
        }
        if (start_blocking_time == 0) {
            start_blocking_time = get_time_ns();
            bytefs_log("Write blocking start at %lld", start_blocking_time);
        }
        schedule();
    }
    ssd->log_size += log_delta_size;
    bytefs_assert(ssd->log_size <= BYTEFS_LOG_REGION_SIZE);
    SSD_STAT_ADD(log_append, 1);
    mutex_unlock(&log_size_mutex);
    mutex_unlock(&log_tail_mutex);
    if (start_blocking_time != 0) {
        end_blocking_time = get_time_ns();
        bytefs_log("Write blocking [%lld - %lld] (%lld)", 
            start_blocking_time, end_blocking_time,
            end_blocking_time - start_blocking_time);
    }

    // fill in metadata and data
    this_log_wp->lpa = lpa;
    this_log_wp->data_len = data_len;
    this_log_wp->flag = 0;
    // bytefs_log("Log wp lpa %lld size %lld flag %lld", 
    //         ssd->log_wp->lpa, ssd->log_wp->data_len, ssd->log_wp->flag);
    memcpy((void *) this_log_wp + sizeof(struct bytefs_log_metadata), data, data_len);

    // insert into imt, operation must be batched as the whole operation must be done atomically
    mutex_lock(&mapping_table_mutex);
    for (data_offset = 0; data_offset < data_len; data_offset += BYTEFS_LOG_REGION_GRANDULARITY) {
        imt_insert(ssd, lpa + data_offset, 
                (uint64_t) this_log_wp + sizeof(struct bytefs_log_metadata) + data_offset);
        cmt_insert(ssd, lpa + data_offset);
    }
    mutex_unlock(&mapping_table_mutex);
    // mark completion
    this_log_wp->flag = BYTEFS_LOG_VALID;
    // bytefs_log("Write complete lpa: %lld size %lld", lpa, data_len);
}

static void try_order_log_flush(struct ssd *ssd) {
    if (ssd->log_size > BYTEFS_LOG_FLUSH_HI_THRESHOLD)
        ssd->log_flush_required = 1;
}

static uint64_t flush_log_region(struct ssd *ssd) {
    // only one thread can enter this at the same time
    struct coalescing_mte *cmte;
    struct indirection_mte *imte;
    uint64_t current_lpn, current_offset, current_size;
    uint64_t bitmask, bit_idx, cmte_bitmap, coalescing_offset, imte_log_offset;
    struct ppa ppa;
    struct nand_cmd cmd;
    int log_size;
    uint64_t current_time, lat = 0;
    uint64_t wr_modified = 0;
    SSD_STAT_ATOMIC_INC(log_flushes);
    while (1) {
        mutex_lock(&log_size_mutex);
        if (ssd->log_size == 0) {
            mutex_unlock(&log_size_mutex);
            break;
        }
        mutex_unlock(&log_size_mutex);
        
        current_time = get_time_ns();
        validate_log_head(ssd);
        // current examing log entry is not filled completely yet
        if ((ssd->log_rp->flag & BYTEFS_LOG_VALID) == 0) {
            bytefs_log("Flush early finish w/imt size: %d", ssd->imt_size);
            break;
        }
        bytefs_assert(ssd->log_rp->lpa % BYTEFS_LOG_REGION_GRANDULARITY == 0);
        bytefs_assert(ssd->log_rp->data_len % BYTEFS_LOG_REGION_GRANDULARITY == 0);
        bytefs_assert(ssd->log_rp->data_len != 0);
        current_lpn = ssd->log_rp->lpa / ssd->sp.pgsz;
        current_offset = ssd->log_rp->lpa - current_lpn * ssd->sp.pgsz;
        current_size = ssd->log_rp->data_len;
        // bytefs_log("On retrieving cmt, current lpa %lld lpn %lld size %lld offset %lld", 
        //         ssd->log_rp->lpa, current_lpn, current_size, current_offset);
        bytefs_assert(current_offset % BYTEFS_LOG_REGION_GRANDULARITY == 0);
        // whole section critical as cmte should not be modified while doing read
        mutex_lock(&mapping_table_mutex);
        do {
            cmte = cmt_get(ssd, current_lpn * ssd->sp.pgsz);
            // if the entry is not found in the imt/cmt, it means that it is flushed before,
            // we will skip the entry as it is not valid any more
            if (cmte) {
                bytefs_assert(cmte->bitmap != 0);
                cmte_bitmap = cmte->bitmap;
                // find the page in the ftl mapping
                ppa = get_maptbl_ent(ssd, current_lpn);
                if (mapped_ppa(&ppa)) {
                    // found, read out first
                    cmd.type = USER_IO;
                    cmd.cmd = NAND_READ;
                    cmd.stime = current_time;
                    lat = max(lat, ssd_advance_status(ssd, &ppa, &cmd));
                    backend_rw(ssd->bd, ppa.realppa, ssd->flush_page_buffer, 0);
                    // update old page information
                    mark_page_invalid(ssd, &ppa);
                    bytefs_try_add_gc_candidate_ppa(ssd, &ppa);
                    set_rmap_ent(ssd, INVALID_LPN, &ppa);
                    SSD_STAT_ADD(log_coalescing_rd_page, 1);
                } else {
                    memset(ssd->flush_page_buffer, 0, ssd->sp.pgsz);
                }
                // bring page from nand flash to coalescing buffer
                // try to coalesce all the entries that can be found in current cmt entry
                wr_modified = 0;
                for (bit_idx = 0; bit_idx < PG_SIZE / BYTEFS_LOG_REGION_GRANDULARITY; bit_idx++) {
                    coalescing_offset = bit_idx * BYTEFS_LOG_REGION_GRANDULARITY;
                    imte = imt_get(ssd, current_lpn * ssd->sp.pgsz + coalescing_offset);
                    if (imte) {
                        imte_log_offset = imte->log_offset;
                        imt_remove(ssd, current_lpn * ssd->sp.pgsz + coalescing_offset);
                        memcpy(ssd->flush_page_buffer + coalescing_offset, 
                            (void *) imte_log_offset, 
                            BYTEFS_LOG_REGION_GRANDULARITY);
                        wr_modified++;
                    }
                }
                cmt_remove(ssd, current_lpn * ssd->sp.pgsz);
                cmte = cmt_get(ssd, current_lpn * ssd->sp.pgsz);
                bytefs_assert(!cmte);
                SSD_STAT_ADD(byte_issue_nand_wr_modified_distribution[wr_modified - 1], 1);
                // write the page back
                ppa = get_new_page(ssd);
                set_maptbl_ent(ssd, current_lpn, &ppa);
                set_rmap_ent(ssd, current_lpn, &ppa);
                mark_page_valid(ssd, &ppa);
                ssd_advance_write_pointer(ssd);
                // bytefs_log("Setting ftl mapping lpn %lld -> ppn %lld", current_lpn, ppa.realppa);
                // perform data write
                cmd.type = USER_IO;
                cmd.cmd = NAND_WRITE;
                cmd.stime = current_time;
                lat = max(lat, ssd_advance_status(ssd, &ppa, &cmd));
                backend_rw(ssd->bd, ppa.realppa, ssd->flush_page_buffer, 1);
                // operate on remaining part of the log if any
                SSD_STAT_ADD(log_wr_page, 1);
            }
            // advance to next part of the current log
            if (current_size < ssd->sp.pgsz) {
                // bytefs_log("Log flush advanced by %lld size %lld -> %lld iter finish", 
                //         current_size, current_size, 0);
                current_size = 0;
            } else {
                // bytefs_log("Log flush advanced by %lld size %lld -> %lld", 
                //         ssd->sp.pgsz - current_offset, current_size, 
                //         current_size - (ssd->sp.pgsz - current_offset));
                current_size -= ssd->sp.pgsz - current_offset;
                current_lpn++;
                current_offset = 0;
            }
            // bytefs_log("On iter finishing, current lpn %lld size %lld offset %lld", 
            //         current_lpn, current_size, current_offset);
        } while (current_size);
        mutex_unlock(&mapping_table_mutex);
        // proceed to next log
        // bytefs_log("Log size before advance head %lld", ssd->log_size);
        advance_log_head(ssd);
        // bytefs_log("Log size after advance head %lld", ssd->log_size);
    }
    // bytefs_log("coalescing finish w/imt size: %d", ssd->imt_size);
    return lat;
}

void force_flush_log(void) {
    struct ssd *ssd = &gdev;
    // signal ftl thread to flush region
    bytefs_log("Force flush ordered");
    ssd->log_flush_required = 1;
}

static uint64_t read_data(struct ssd *ssd, uint64_t lpa, uint64_t size, void *data, uint64_t stime) {
    struct ppa ppa;
    struct coalescing_mte *cmte;
    struct indirection_mte *imte;
    struct nand_cmd cmd;
    uint64_t cmte_bitmap;
    uint64_t current_iter_size, current_data_offset, current_log_offset;
    uint64_t bitmask;
    uint64_t lpn = lpa / ssd->sp.pgsz;
    uint64_t offset = lpa - lpn * ssd->sp.pgsz;
    void *data_frontier = data;
    uint64_t lat = 0;
    uint64_t rd_modified = 0;
    SSD_STAT_ATOMIC_INC(log_rd_op);
    // hard limit, otherwise more complicated design would be required on bytefs_log_metadata
    bytefs_assert_msg(lpa % BYTEFS_LOG_REGION_GRANDULARITY == 0, "lpa: %lld", lpa);
    bytefs_assert_msg(size % BYTEFS_LOG_REGION_GRANDULARITY == 0, "size: %lld", size);
    // for each iteration of the loop, process a page
    while (size > 0) {
        // max size is page size minus offset, actural size is min of max size and remaining size
        current_iter_size = ssd->sp.pgsz - offset;
        current_iter_size = current_iter_size > size ? size : current_iter_size;
        // find the entry in the coalescing mapping table
        mutex_lock(&mapping_table_mutex);
        cmte = cmt_get(ssd, lpn * ssd->sp.pgsz);
        ppa = get_maptbl_ent(ssd, lpn);
        // bytefs_log("Read data LPN %lld offset %lld cursize %lld size %lld", 
        //         lpn, offset, current_iter_size, size);

        if (cmte) {
            SSD_STAT_ADD(log_rd_log_page_hit, 1);
            // there is data in the log buffer to be read
            bytefs_assert(cmte->bitmap != 0);
            cmte_bitmap = cmte->bitmap;
            // mutex_unlock(&mapping_table_mutex);
            // determine if all the data requested in this page are in the log region
            for (current_data_offset = 0;
                 current_data_offset < current_iter_size;
                 current_data_offset += BYTEFS_LOG_REGION_GRANDULARITY) {
                current_log_offset = current_data_offset + offset;
                bitmask = 1UL << (current_log_offset / BYTEFS_LOG_REGION_GRANDULARITY);
                if ((bitmask & cmte_bitmap) != 0) {
                    // imt entry not found, load from nand flash
                    // bytefs_log("Read load from nand LPN %lld (%lld)", lpn, ppa.realppa);
                    if (mapped_ppa(&ppa)) {
                        cmd.type = USER_IO;
                        cmd.cmd = NAND_READ;
                        cmd.stime = stime;
                        lat = max(lat, ssd_advance_status(ssd, &ppa, &cmd));
                        // aquire the ownership of the log page buffer
                        mutex_lock(&log_page_buffer_mutex);
                        backend_rw(ssd->bd, ppa.realppa, ssd->log_page_buffer, 0);
                        // read the required part to data
                        memcpy(data_frontier, ssd->log_page_buffer + offset, current_iter_size);
                        mutex_unlock(&log_page_buffer_mutex);
                    } else {
                        // bytefs_log("Data read w/cmt found LPN %lld offset %lld",
                        //         lpn, current_log_offset);
                        memset(data_frontier, 0, current_iter_size);
                    }
                    SSD_STAT_ADD(log_rd_log_page_partial_hit, 1);
                    SSD_STAT_SUB(log_rd_log_page_hit, 1);
                    break;
                }
            }
            // redo the loop and copy over, whole lock is posed as a consistent version need to be
            // achieved
            // mutex_lock(&mapping_table_mutex);
            rd_modified = 0;
            for (current_data_offset = 0;
                 current_data_offset < current_iter_size;
                 current_data_offset += BYTEFS_LOG_REGION_GRANDULARITY) {
                current_log_offset = current_data_offset + offset;
                imte = imt_get(ssd, lpn * ssd->sp.pgsz + current_log_offset);
                if (imte) {
                    memcpy(data_frontier + current_data_offset,
                           (void *) imte->log_offset,
                           BYTEFS_LOG_REGION_GRANDULARITY);
                    rd_modified++;
                }
            }
            SSD_STAT_ADD(byte_issue_nand_rd_modified_distribution[rd_modified - 1], 1);
            mutex_unlock(&mapping_table_mutex);
        } else {
            SSD_STAT_ADD(log_direct_rd_page, 1);
            mutex_unlock(&mapping_table_mutex);
            // cmt entry not found, load from nand flash
            // bytefs_log("Read cmte not found load from nand LPN %lld (%lld)", lpn, ppa.realppa);
            if (mapped_ppa(&ppa)) {
                cmd.type = USER_IO;
                cmd.cmd = NAND_READ;
                cmd.stime = stime;
                lat = max(lat, ssd_advance_status(ssd, &ppa, &cmd));
                // aquire the ownership of the log page buffer
                mutex_lock(&log_page_buffer_mutex);
                backend_rw(ssd->bd, ppa.realppa, ssd->log_page_buffer, 0);
                // read the required part to data
                memcpy(data_frontier, ssd->log_page_buffer + offset, current_iter_size);
                mutex_unlock(&log_page_buffer_mutex);
            } else {
                // bytefs_warn("Data read w/cmt not found " 
                //         "uninitialized region LPN %lld offset %lld",
                //         lpn, offset);
                memset(data_frontier, 0, current_iter_size);
            }
        }
        // bytefs_log("Data frontier advanced by %lld size %lld -> %lld", 
        //         current_iter_size, size, size - current_iter_size);
        // advance read data frontier
        data_frontier += current_iter_size;
        size -= current_iter_size;
        // advance to next page
        lpn++;
        offset = 0;
    }
    // bytefs_log("Read data finish with log size %lld/%ld/%ld latency %lld ========", 
    //         ssd->log_size, BYTEFS_LOG_FLUSH_HI_THRESHOLD, BYTEFS_LOG_REGION_SIZE, lat);
    return lat;
}

static uint64_t write_data(struct ssd *ssd, uint64_t lpa, uint64_t size, void *data, uint64_t stime) {
    int mutex_status;
    uint64_t flush_start_time, flush_end_time;
    uint64_t lpn = lpa / ssd->sp.pgsz;
    uint64_t lat = 0;
    uint64_t current_size = 0, current_offset = 0;
    SSD_STAT_ATOMIC_INC(log_wr_op);
    // hard limit, otherwise more complicated design would be required on bytefs_log_metadata
    bytefs_assert_msg(lpa % BYTEFS_LOG_REGION_GRANDULARITY == 0, "lpa: %lld", lpa);
    bytefs_assert_msg(size % BYTEFS_LOG_REGION_GRANDULARITY == 0, "size: %lld", size);
    bytefs_assert(size > 0);
    // put data to log region
    while (size > 0) {
        current_size = size > BYTEFS_LOG_MAX_ENTRY_DATA_SIZE ? BYTEFS_LOG_MAX_ENTRY_DATA_SIZE : size;
        advance_log_tail(ssd, lpa + current_offset, current_size, data + current_offset);
        try_order_log_flush(ssd);
        size -= current_size;
        current_offset += current_size;
    }
    // bytefs_log("Write data finish with log size %lld/%ld/%ld latency %lld ========", 
    //         ssd->log_size, BYTEFS_LOG_FLUSH_HI_THRESHOLD, BYTEFS_LOG_REGION_SIZE, lat);
    return lat;
}

/** Block read
 *
 */
static uint64_t block_read(struct ssd *ssd, event *ctl)
{
    NvmeRwCmd* req = (NvmeRwCmd*)(&(ctl->cmd));
    uint64_t lpa = req->slba * ssd->sp.pgsz;
    uint64_t size = req->nlb * ssd->sp.pgsz;

    uint64_t stime = get_time_ns();
    uint64_t maxlat = read_data(ssd, lpa, size, (void *) req->prp1, stime);
    SSD_STAT_ATOMIC_INC(block_rissue_count);
    SSD_STAT_ATOMIC_ADD(block_rissue_traffic, size);

    return maxlat;
}



static uint64_t block_write(struct ssd *ssd, event *ctl)
{
    NvmeRwCmd* req = (NvmeRwCmd*)(&(ctl->cmd));
    uint64_t lba = req->slba;
    struct ssdparams *spp = &ssd->sp;
    int nlb = req->nlb;
    uint64_t start_lpn = lba;
    uint64_t end_lpn = lba + nlb;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    struct nand_cmd swr;

    for (lpn = start_lpn; lpn < end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);
        if (mapped_ppa(&ppa)) {
            /* update old page information first */
            mark_page_invalid(ssd, &ppa);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
        }

        /* new write */
        ppa = get_new_page(ssd);

        // bytefs_log("write request: lpn=%lu, ppa=%lu ", lpn, ppa.realppa);
        /* update maptbl */
        set_maptbl_ent(ssd, lpn, &ppa);
        /* update rmap */
        set_rmap_ent(ssd, lpn, &ppa);

        // don't need to mark valid here?
        ppa2pgidx(ssd, &ppa);
        mark_page_valid(ssd, &ppa);

        /* need to advance the write pointer here */
        ssd_advance_write_pointer(ssd);

        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = ctl->s_time;
        /* get latency statistics */
        curlat = ssd_advance_status(ssd, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
        //actually write page into addr @TODO check it this is safe
        backend_rw(ssd->bd, ppa.realppa, (void *) req->prp1, 1);
    }
    SSD_STAT_ATOMIC_INC(block_wissue_count);
    SSD_STAT_ATOMIC_ADD(block_wissue_traffic, nlb * ssd->sp.pgsz);

    return maxlat;
}

struct ftl_thread_info {
    int num_poller;
    struct Ring **to_ftl;
    struct Ring **to_poller;
};




int ftl_thread(void* arg)
{
    struct ftl_thread_info *info = (struct ftl_thread_info*) arg;
    struct ssd *ssd = &gdev;
    event *ctl_event = NULL;
    NvmeRwCmd *req = NULL;
    uint64_t lat = 0;
    uint64_t flush_start_time, flush_end_time;
    int rc;
    int i;
    // int debug_rpush = 0;

    // while (!*(ssd->dataplane_started_ptr)) {
    //     usleep(100000);
    // }

    //init time here
    /* measure monotonic time for nanosecond precision */
    // clock_gettime(CLOCK_MONOTONIC, &start);
    bytefs_log("ftl thread: start time %llu", get_time_ns());

    ssd->to_ftl = info->to_ftl;
    ssd->to_poller = info->to_poller;

    //main loop
    while (!kthread_should_stop()) {
        // keep the multiple poller here for furture expansion of multiple queue
        for (i = 1; i <= info->num_poller; i++) {
            if (!ssd->to_ftl[i] || ring_is_empty(ssd->to_ftl[i]))
                continue;
            // printk(KERN_NOTICE "ftl_thread : get ring %d to_ftl[1]:%lx\n",debug_rpush++,ssd->to_ftl[i]);
            ctl_event = (event *)ring_get(ssd->to_ftl[i]);
            if (!ctl_event) {
                bytefs_err("FTL to_ftl dequeue failed\n");
                continue;
            };


            req = (NvmeRwCmd *)(&(ctl_event->cmd));
            // clock_gettime(CLOCK_MONOTONIC, &cur);
            cur = get_time_ns();
            // bytefs_log("ftl thread: cur_start time %llu", cur);

            ctl_event->s_time = cur;
            switch (req->opcode) {
            case NVME_CMD_WRITE:
                // bytefs_log("ssd write");
                lat = block_write(ssd, ctl_event);
                break;
            case NVME_CMD_READ:
                // bytefs_log("ssd read");
                lat = block_read(ssd, ctl_event);
                break;

            // add future api here
            // case NVME_CMD_DSM:
            //     lat = 0;
            //     break;

            default:
                bytefs_err("FTL received unkown request type, ERROR\n");
                break;
            }

            // cur = get_time_ns();
            // bytefs_log("ftl thread: cur_finish time %llu", cur);

            // bytefs_log("lat=%lld", lat);
            ctl_event->reqlat = lat;
            ctl_event->expire_time = ctl_event->s_time + lat;


            do {
                rc = ring_put(ssd->to_poller[i], (void *)ctl_event);

                if (rc == -1) {
                    // if (ring_is_full(ssd->to_poller[i]))
                    //     bytefs_err("FTL to_poller queue full\n");

                    // bytefs_err("FTL to_poller enqueue failed, %ld\n", ctl_event->identification);
                    schedule();
                }
            } while (rc != 0);

            // /* clean if needed (in the background) */
            // if (should_gc(ssd)) {
            //     do_gc(ssd, false);
            // }
        }
        // if flush is ordered, do the flush immediately
        if (ssd->log_flush_required) {
            flush_start_time = get_time_ns();
            bytefs_log("Log flush starts @ %20lld w/capacity = %lld/%ld",
                flush_start_time,
                ssd->log_size, BYTEFS_LOG_REGION_SIZE);
            flush_log_region(ssd);
            flush_end_time = get_time_ns();
            bytefs_log("Log flush ends   @ %20lld w/capacity = %lld/%ld (duration: %lld ns)",
                flush_end_time,
                ssd->log_size, BYTEFS_LOG_REGION_SIZE,
                flush_end_time - flush_start_time);
            ssd->log_flush_required = 0;
        }
        schedule();
        // printk_ratelimited(KERN_ERR "ftl thread scheduled");
    }

    return 0;
}

/**
 * Poll completed events for all threads.
*/
int request_poller_thread(void* arg) {
    struct ftl_thread_info *info = (struct ftl_thread_info*) arg;
    struct ssd *ssd = &gdev;
    event* evt = NULL; // haor2 : no idea why this is allocated. To fix.
    struct bytefs_heap event_queue;
    uint64_t cur_time;
    int i;

    bytefs_log("request poller thread: start time %llu", get_time_ns());

    heap_create(&event_queue, MAX_EVENT_HEAP);
    while (!kthread_should_stop()) {
        for (i = 1; i <= info->num_poller; i++) {
            if (!ssd->to_poller[i] || ring_is_empty(ssd->to_poller[i]))
                continue;
            evt = (event*) ring_get(ssd->to_poller[i]);
            if (!evt) {
                bytefs_expect("FTL to_poller dequeue failed");
                continue;
            }
            if (!evt->if_block) {
                /* debugging snippet */
                // if (evt->bio != NULL) {
                //     if (*evt->if_end_bio > 1) {
                //         // printk(KERN_ERR"end : %lld %llx\n",*evt->if_end_bio,evt->if_end_bio);
                //         (*evt->if_end_bio)--;
                //         // printk(KERN_ERR"end : %lld %llx\n",*evt->if_end_bio,evt->if_end_bio);
                //     }
                //     else{
                //         // byte_fs_print_submit_bio_info(evt->bio);
                //         kfree(evt->if_end_bio);
                //         bio_endio(evt->bio);
                //         // evt->bio->bi_end_io(evt->bio);
                //         kfree(evt);
                //     }
                // }
                /* end debugging */
                heap_insert(&event_queue, evt->expire_time, evt);
                // printk(KERN_NOTICE "hahah event enqueue expire: %llu \n", evt->expire_time);
            } else {
                evt->completed = 1;
            }
        }

        do {
            evt = heap_get_min(&event_queue);
            if (evt != NULL) {
                cur_time = get_time_ns();
                // printk_ratelimited(KERN_NOTICE "hahah event dequeueeueuueueu cur: %llu expire: %llu \n",cur_time,evt->expire_time);
                if (cur_time >= evt->expire_time) {
                    // printk(KERN_NOTICE "hahah event expired expire: %llu \n", evt->expire_time);
                    if (evt->bio != NULL) {
                        if ((*evt->if_end_bio) > 1) {
                            (*evt->if_end_bio)--;
                        } else {
                            kfree(evt->if_end_bio);
                            bio_endio(evt->bio);
                        }
                    }
                    heap_pop_min(&event_queue);
                    evt->completed = 1;
                    // kfree(evt);
                }

            }
            schedule();
            // cpu_relax();
        } while (heap_is_full(&event_queue, info->num_poller));
    }
    return 0;
}


// /**
//  * print out the memory layout for emulated 2B-SSD
//  * outputsize source is ftl_mapping.h
// */
// static void ftl_debug_DRAM_MEM_LAYOUT(void) {
// #if (TEST_FTL_DEBUG==TEST_FTL_SHOW_SIZE)
//     /**
//      * CACHE
//      * ------------------------------------------------------------------------------------------------------
//      * | LOG           |  INDIRECTION_MT    | C..._MT |FARR  | PGW | PGR |NANDBUF|MIG_BUF|MIGDESTBUF|C...BUF|
//      * |(2000KB : ~2MB)| (3187200B : ~3MB)  | (800KB) |(~6KB)|(4KB)|(4KB)| (4KB) | (4KB) |  (4KB)   | (4KB) |
//      * ------------------------------------------------------------------------------------------------------
//     */
//     printk("LOGSIZE : %lu\n",          (long unsigned int)LOG_SIZE);
//     printk("IMTSIZE : %lu\n",          (long unsigned int)INDIRECTION_MT_SIZE);
//     printk("CMTSIZE : %lu\n",          (long unsigned int)COALESCING_MT_SIZE);
//     printk("FARRSIZE : %lu\n",         (long unsigned int)FLUSHED_ARRAY_SIZE);
//     printk("PGWRBUFSIZE : %lu\n",      (long unsigned int)PG_WR_FLUSH_BUF_SIZE);
//     printk("PGRDBUFSIZE : %lu\n",      (long unsigned int)PG_RD_FLUSH_BUF_SIZE);
//     printk("PGRDNANDBUFSIZE : %lu\n",  (long unsigned int)PG_RD_NAND_BUF_SIZE);
//     printk("MIGLOGBUF : %lu\n",        (long unsigned int)MIGRATION_LOG_BUF_SIZE);
//     printk("MIGDESTBUFSIZE : %lu\n",   (long unsigned int)MIGRATION_DEST_BUF_SIZE);
//     printk("CBUF_SIZE : %lu\n",        (long unsigned int)COALESCING_BUF_SIZE);
//     printk("TOTAL_SIZE : %lu\n",       (long unsigned int)DRAM_END);
// #endif
// }

/**
* Initialize ssd parameters for emulated 2B-SSD, initialize related functionality of the emulated hardware.
* SIDE-EFFECT : one kthread created.
* >6GB host's memory will be used, check backend.c to see details.
*/
int ssd_init(void)
{
    if(inited_flag == 1)
        return 0;
    else
        inited_flag = 1;
    struct ssd *ssd = &gdev;
    struct ssdparams *spp = &ssd->sp;
    int i, ret;

    bytefs_assert(ssd);

    ssd_init_params(spp);
    bytefs_log("Init para");
    /* initialize ssd internal layout architecture */
    ssd->ch = kzalloc(sizeof(struct ssd_channel) * spp->nchs, GFP_KERNEL);
    /* 512 KB to emulate SSD storage hierarchy will be allocated besides the 6GB emulation */
    for (i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&ssd->ch[i], spp);
    }
    bytefs_log("ByteFS init maptbl");
    /* initialize maptbl */
    ssd_init_maptbl(ssd);
    bytefs_log("ByteFS init remap");
    /* initialize rmap */
    ssd_init_rmap(ssd);

    /* initialize GC related structures */
    bytefs_log("ByteFS init GC facility");
    bytefs_gc_init(ssd);
    /* initialize write pointer, this is how we allocate new pages for writes */
    bytefs_log("ByteFS init wp");
    ssd_init_write_pointer(ssd);

    /* initialize all queues */
    bytefs_log("ByteFS init all queues");
    ssd_init_queues(ssd);
    /* initialize bgackend */
    bytefs_log("ByteFS init backend");
    ret = init_dram_backend(&(ssd->bd), TOTAL_SIZE, 0);

    /* initialize buffer */
    bytefs_log("ByteFS init in device log buffer");
    ssd_init_bytefs_buffer(ssd);

    // bytefs_log("start thread");
    // init new kernel thread for ftl emulation
    bytefs_log("ByteFS start threads");
    // thread info is initialized for one time only
    ssd->thread_args = kzalloc(sizeof(struct ftl_thread_info), GFP_KERNEL);
    ssd->thread_args->num_poller = 1;
    ssd->thread_args->to_ftl = ssd->to_ftl;
    ssd->thread_args->to_poller = ssd->to_poller;

    // ftl_thread(arg);
    ssd->thread_id = kzalloc(sizeof(struct task_struct), GFP_KERNEL);
    ssd->thread_id = kthread_create(ftl_thread, ssd->thread_args, "ftl_thread");
    kthread_bind(ssd->thread_id, SSD_THREAD_CPU);
    if (ssd->thread_id == 0)
        bytefs_err("Failed to create FTL thread\n");
    else
        wake_up_process(ssd->thread_id);

    ssd->polling_thread_id = kzalloc(sizeof(struct task_struct), GFP_KERNEL);
    ssd->polling_thread_id = kthread_create(request_poller_thread, ssd->thread_args, "ftl_poller_thread");
    kthread_bind(ssd->polling_thread_id, SSD_POLLING_CPU);
    if (ssd->polling_thread_id == 0)
        bytefs_err("Failed to create request poller thread\n");
    else
        wake_up_process(ssd->polling_thread_id);

    bytefs_log("ByteFS init done");
    return 0;
}
EXPORT_SYMBOL(ssd_init);

int ssd_reset(void) {
    int i, j, k, l;
    struct ssd *ssd = &gdev;
    struct ssdparams *spp = &ssd->sp;

    // kill the thread first
    if (ssd->thread_id && !kthread_stop(ssd->thread_id)) {
        ssd->thread_id = 0;
        bytefs_log("ftl thread stopped");
    } else {
        bytefs_err("ftl thread stop failed");
    }

    if (ssd->polling_thread_id && !kthread_stop(ssd->polling_thread_id)) {
        ssd->polling_thread_id = 0;
        bytefs_log("polling thread stopped");
    } else {
        bytefs_err("polling thread stop failed");
    }

    // clear all information for pages and blocks
    bytefs_log("ByteFS reset clear buffer info");
    for (i = 0; i < spp->nchs; i++) {
        for (j = 0; j < spp->luns_per_ch; j++) {
                for (k = 0; k < spp->blks_per_lun; k++) {
                    for (l = 0; l < spp->pgs_per_blk; l++) {
                        ssd->ch[i].lun[j].blk[k].pg[l].pg_num = i;
                        ssd->ch[i].lun[j].blk[k].pg[l].status = PG_FREE;
                    }
                    ssd->ch[i].lun[j].blk[k].ipc = 0;
                    // vpc here should be inited to 0
                    // ssd->ch[i].lun[j].blk[k].vpc = spp->pgs_per_blk;
                    ssd->ch[i].lun[j].blk[k].vpc = 0;
                    ssd->ch[i].lun[j].blk[k].erase_cnt = 0;
                    ssd->ch[i].lun[j].blk[k].wp = 0;
                }
                ssd->ch[i].lun[j].next_lun_avail_time = 0;
                ssd->ch[i].lun[j].busy = false;
        }
        ssd->ch[i].next_ch_avail_time = 0;
        ssd->ch[i].busy = 0;
    }

    // mapping
    for (i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].realppa = UNMAPPED_PPA;
    }
    for (i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }

    // reset GC facilities
    bytefs_log("ByteFS reset GC facilities");
    bytefs_gc_reset(ssd);
    // reset write pointer
    bytefs_log("ByteFS reset write pointer");
    ssd_init_write_pointer(ssd);
    // reset bytefs log related structure
    bytefs_log("ByteFS reset associated log structures");
    ssd_init_bytefs_buffer(ssd);

    // drain ring buffer
    bytefs_log("ByteFS reset drain ring buffer");
    for (i = 0; i < ssd->thread_args->num_poller; i++) {
        while (ssd->to_ftl[i] && !ring_is_empty(ssd->to_ftl[i]))
            ring_get(ssd->to_ftl[i]);
        while (ssd->to_ftl[i] && !ring_is_empty(ssd->to_poller[i]))
            ring_get(ssd->to_poller[i]);
    }

    //TODO reset buffer here

    ssd->thread_id = kzalloc(sizeof(struct task_struct), GFP_KERNEL);
    ssd->thread_id = kthread_create(ftl_thread, ssd->thread_args, "ftl_thread");
    kthread_bind(ssd->thread_id, SSD_THREAD_CPU);
    if (ssd->thread_id == 0)
        bytefs_err("Failed to create FTL thread\n");
    else {
        wake_up_process(ssd->thread_id);
        bytefs_log("FTL thread started");
    }

    ssd->polling_thread_id = kzalloc(sizeof(struct task_struct), GFP_KERNEL);
    ssd->polling_thread_id = kthread_create(request_poller_thread, ssd->thread_args, "ftl_poller_thread");
    kthread_bind(ssd->polling_thread_id, SSD_POLLING_CPU);
    if (ssd->polling_thread_id == 0)
        bytefs_err("Failed to create request poller thread\n");
    else {
        wake_up_process(ssd->polling_thread_id);
        bytefs_log("request poller thread started");
    }

    return 0;
}


/** user block interface to init an nvme cmd to the ssd device */

/** Legacy calling interface
* Testing block write to NVMe SSD time. Without use of BIO.
* INPUT :
*   is_write - write or not
*   lba      - logical block address : used for block I/O reference
*   len      - r/w length
*   buf      - in read case : source of input; in write case : destination of output.
* RETURN :
*   issued length :
*   -1 on error
* SIDE-EFFECT :
* currently blocking
*/
int nvme_issue(int is_write, uint64_t lba, uint64_t len, void *buf) {
    // if((lba + len)* PG_SIZE >= (32ull << 30)) {
    //     printk(KERN_ERR "bad nvme addr at %llx size = %llx", lba * PG_SIZE, len * PG_SIZE);
    //     return -1;
    // }
    // return 0;


    struct ssd *ssd = &gdev;
    int ret;
    event* e = kzalloc(sizeof(struct cntrl_event), GFP_ATOMIC);
    uint64_t sec, actrual_t;

    if (e == NULL) return -1;
    if (is_write) {
        bytefs_init_nvme(&(e->cmd), NVME_CMD_WRITE, lba, len, buf);
    } else {
        bytefs_init_nvme(&(e->cmd), NVME_CMD_READ, lba, len, buf);
    }

    e->s_time = 0;
    e->expire_time = 0;
    e->reqlat = 0;
    e->completed = 0;
    if (is_write)
        e->if_block = 1;
    else
        e->if_block = 0;
    e->bio = NULL;
    e->e_time = 0;

    ret = ring_put(ssd->to_ftl[1], (void *)e);
    if (ret == -1) {
        bytefs_log("Ring buffer full");
        // while (!ring_put(ssd->to_ftl[1], (void *)&e)) {}
        return -1;
    }
    //printk(KERN_NOTICE "issue: put success \n");


        // read should be blocking

    while (!e->completed) {
        
    }

    kfree(e);
    return len;




    cur = get_time_ns();
    // bytefs_log("issue: get success expire time = %llu stime = %llu cur = %llu ",e->expire_time,e->s_time,cur);

    if (e->expire_time >= 65000 + e->s_time) {
        // bytefs_log("sleep %llu ",e->expire_time - 65000 - e->s_time);
        sec = e->expire_time - 65000 - e->s_time;
        // sleepns(sec);
    } else {
        // bytefs_log("no sleep -%llu ",65000 + e->s_time - e->expire_time);
    }

    // uint2time(e->expire_time - 65000 - e->s_time, &abs_wait_barrier);
    // printf("wait for barrier %llu\n",e->expire_time);
    // clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &abs_wait_barrier, NULL);

    // clock_gettime(CLOCK_MONOTONIC, &actural_barrier);

    // actrual_t = get_time_ns();nvme_issue

    // uint64_t actrual_t = time2uint(&actural_barrier);
    //printk(KERN_NOTICE "FIN: <%ld> @ %ld(%ld) | %ld(%ld) => %ld\n", e->identification,
    // e->expire_time, actrual_t,
    // e->expire_time - e->s_time, actrual_t - e->s_time,
    // actrual_t - e->expire_time);

    kfree(e);
    return len;
}
EXPORT_SYMBOL(nvme_issue);







/**
* Block-interface write to NVMe SSD time. This function does NOT block.
* This function doesn't block, instead, it submit the I/O, and use BIO as a receipt for checking whether it's completed or not.
* INPUT :
*   is_write - write or not
*   lba      - logical block address : used for block I/O reference
*   page_num      - number of page
*   buf      - in read case : source of input; in write case : destination of output.
*   bio      - related bio request. After finishing the issue (poller handled the request), bio->end_io() should be invoked.
* RETURN :
*   -1 on failure. 0 on success.
* SIDE-EFFECT :
*   on testing, bio should be aligned to bio in BIO layer.
*/
int nvme_issue_nowait(int is_write, uint64_t lba, uint64_t page_num, void *buf, struct bio* bio, unsigned long *if_end_bio) {
    struct ssd *ssd = &gdev;
    int ret;
    event* e;
    // uint64_t abs_wait_barrier, actural_barrier;

    // sanity check on NVMe issue
    if (!lba_is_legal(lba) || !lba_is_legal(lba + page_num - 1)) {
        bytefs_log("LBA out of bound %llu number of pages = %llu", lba, page_num);
        return -1;
    }
    // printk(KERN_ERR "lba = %llu, page_num = %llu\n");

    e = kzalloc(sizeof(struct cntrl_event), GFP_ATOMIC);
    if (e == NULL) {
        bytefs_err("No space left");
        return -1;
    }
    if (is_write) {
        bytefs_init_nvme(&(e->cmd), NVME_CMD_WRITE, lba, page_num, buf);
    } else {
        bytefs_init_nvme(&(e->cmd), NVME_CMD_READ, lba, page_num, buf);
    }

    e->s_time = 0;
    e->expire_time = 0;
    e->reqlat = 0;
    e->if_block = 0; // set event no block
    e->if_end_bio = if_end_bio;
    e->completed = 0;
    e->e_time = 0;
    e->bio = bio;

    do {
        if (ring_is_full(ssd->to_ftl[1])) {
            if (unlikely(bio->bi_opf & (1UL << 21))) {     // hard code here for REQ_NOWAIT
                return -EBUSY;
            }
        } else {
            ret = ring_put(ssd->to_ftl[1], (void *)e);
            break;
        }
        printk_ratelimited(KERN_NOTICE "Hey! to_ftl queue is full\n");
    } while (1);

    return 0;
}

/**
 * see nvme_issue. This function BLOCKS.
 *              *IMPORTANT*
 * You need to mannually call bio->bi_end_io after this function,
 * because this function is usually called to do temporary read / write without needing bio.
*/
int nvme_issue_wait(int is_write, uint64_t lba, uint64_t page_num, void *buf, struct bio* bio) {
    struct ssd *ssd = &gdev;
    int ret;
    event* e;
    uint64_t sec;
    // uint64_t abs_wait_barrier, actural_barrier;

    // sanity check on NVMe issue
    if (!lba_is_legal(lba) || !lba_is_legal(lba + page_num - 1)) {
        bytefs_err("LBA out of bound %llu number of pages = %llu\n", lba, page_num);
        return -1;
    }
    // printk(KERN_ERR "lba = %llu, page_num = %llu\n");

    e = kzalloc(sizeof(struct cntrl_event), GFP_ATOMIC);
    if (e == NULL) return -1;
    if (is_write) {
        bytefs_init_nvme(&(e->cmd), NVME_CMD_WRITE, lba, page_num, buf);
    } else {
        bytefs_init_nvme(&(e->cmd), NVME_CMD_READ, lba, page_num, buf);
    }

    e->s_time = 0;
    e->expire_time = 0;
    e->reqlat = 0;
    e->if_block = 1; // set event block
    e->completed = 0;
    e->e_time = 0;
    e->bio = bio;

    do {
        if (ring_is_full(ssd->to_ftl[1])) {
            if (unlikely(bio->bi_opf & (1UL << 21))) {     // hard code here for REQ_NOWAIT
                return -EBUSY;
            }
        } else {
            ret = ring_put(ssd->to_ftl[1], (void *)e);
            break;
        }

    } while (1);

    if (ret == -1) {
        bytefs_log("Ring buffer full");
        // while (!ring_put(ssd->to_ftl[1], (void *)&e)) {}
        return -1;
    }
    /* blocking */

    while (!e->completed) {
        cpu_relax();
        schedule();
    }

    // below is blocking

    if (e->expire_time >= 65000 + e->s_time) {
        bytefs_log("sleep %llu", e->expire_time - 65000 - e->s_time);
        sec = e->expire_time - 65000 - e->s_time + get_time_ns();
        while (get_time_ns() <= sec) {
            // schedule(); // schedule if multiple threads are allocated
        }
        // sleepns(e->expire_time - 65000 - e->s_time);
    } else {
        printk_ratelimited(KERN_ERR "WARNING : no sleep lat = expire - start = %llu \n", e->expire_time - e->s_time );
    }
    // if (e->expire_time >= 65000 + e->s_time) {
    //     bytefs_log("sleep %llu ", e->expire_time - 65000 - e->s_time);
    //     uint64_t sec = e->expire_time - 65000 - e->s_time;
    //     sleepns(e->expire_time - 65000 - e->s_time);
    // } else {
    //     bytefs_log("no sleep -%llu ", 65000 + e->s_time - e->expire_time);
    // }


    kfree(e);

    return 0;
}

/**
* Testing function for 2B-SSD r/w
* INPUT :
*   is_write - write or not
*   lpa      - byte addressable reference
*   size      - r/w length
*   buf      - in read case : source of input; in write case : destination of output.
* RETURN :
*   0 - on succes
*   always 0
*/
int byte_issue(int is_write, uint64_t lpa, uint64_t size, void *buf) {
    struct ssd *ssd = &gdev;
    // struct ssdparams *spp = &ssd->sp;
    uint64_t stime, endtime;
    int latency = 0;

    // bytefs_log("BI operates on %lld = %lld PG + %d OFF, size = %lld", lpa, lpn, offset, size);

    if (size == 0)
        return 0;

    stime = get_time_ns();
    if (is_write) {
        latency = write_data(ssd, lpa, size, buf, stime);
        SSD_STAT_ATOMIC_INC(byte_wissue_count);
        SSD_STAT_ATOMIC_ADD(byte_wissue_traffic, size);
        endtime = get_time_ns();
        SSD_STAT_ATOMIC_ADD(total_w_lat, endtime - stime);
    } else {
        latency = read_data(ssd, lpa, size, buf, stime);
        SSD_STAT_ATOMIC_INC(byte_rissue_count);
        SSD_STAT_ATOMIC_ADD(byte_rissue_traffic, size);
        endtime = get_time_ns();   
        SSD_STAT_ATOMIC_ADD(total_r_lat, endtime - stime);
    }

    
    if (endtime - stime > latency)
        return 0;
    
    latency -= (endtime - stime);

    if (!is_write)
        sleepns(latency);
    
    return 0;
}
EXPORT_SYMBOL(byte_issue);

/* this function should now be pat of the read/write functionality  */
/**
 * This function DOES block. no wait means sectors are not cached into pages.
 * The rest of pages will be NVMe_issued normally without blocking.
 * INPUT:
 * is_write - read/write
 * bi_sec   - starting sector
 * num_sec  - number of sectors to write
 * buf      - buf to read from / write to
 * bio      - related bio request. After finishing the issue (poller handled the request), bio->end_io() should be invoked.
 * SIDE-EFFECT :
 * It uses temporary buffer to pad the request which occupies a chunk of memory.
*/
int nvme_issue_sector_wait(int is_write, int64_t bi_sec, int64_t num_sec, void* buf, struct bio* bio) {
    uint64_t sec_start = bi_sec;
    uint64_t sec_end = bi_sec + num_sec;
    // event* e;
    int ret = -1;
    char* tmp_buf;
#if (TEST_FTL_NEW_BASE & TEST_FTL_DEBUG)
    bytefs_log("bi_sec = %lld, num_sec = %lld",bi_sec, num_sec);

    bytefs_log("page_start : %lld, page_num : %lld, buf_size : %lld KB", (sec_start - sec_start % NUM_SEC_PER_PAGE) / NUM_SEC_PER_PAGE,
            (num_sec + sec_start % NUM_SEC_PER_PAGE  + (NUM_SEC_PER_PAGE - sec_end % NUM_SEC_PER_PAGE)) / NUM_SEC_PER_PAGE,
            (num_sec + sec_start % NUM_SEC_PER_PAGE + (NUM_SEC_PER_PAGE - sec_end % NUM_SEC_PER_PAGE)) * SEC_SIZE / 1024 );
#endif
    tmp_buf = kmalloc((num_sec + sec_start % NUM_SEC_PER_PAGE + (NUM_SEC_PER_PAGE - sec_end % NUM_SEC_PER_PAGE)) * SEC_SIZE, GFP_ATOMIC);
    if (NULL == tmp_buf) {
        bytefs_log("temporary buffer allocation failed");
        return -1;
    }
    if (!is_write) {
        ret = nvme_issue_wait(0, (sec_start - sec_start % NUM_SEC_PER_PAGE) / NUM_SEC_PER_PAGE ,
                              (num_sec + sec_start % NUM_SEC_PER_PAGE  + (NUM_SEC_PER_PAGE - sec_end % NUM_SEC_PER_PAGE)) / NUM_SEC_PER_PAGE,
                              tmp_buf, bio);
        memcpy(buf, tmp_buf + (sec_start % NUM_SEC_PER_PAGE) * SEC_SIZE, num_sec * SEC_SIZE);
    } else {
        //debug : to delete


        ret = nvme_issue_wait(0, (sec_start - sec_start % NUM_SEC_PER_PAGE) / NUM_SEC_PER_PAGE ,
                              (num_sec + sec_start % NUM_SEC_PER_PAGE  + (NUM_SEC_PER_PAGE - sec_end % NUM_SEC_PER_PAGE)) / NUM_SEC_PER_PAGE,
                              tmp_buf, bio);
        memcpy(tmp_buf + (sec_start % NUM_SEC_PER_PAGE) * SEC_SIZE, buf, num_sec * SEC_SIZE);
        ret = nvme_issue_wait(1, (sec_start - sec_start % NUM_SEC_PER_PAGE) / NUM_SEC_PER_PAGE ,
                              (num_sec + sec_start % NUM_SEC_PER_PAGE  + (NUM_SEC_PER_PAGE - sec_end % NUM_SEC_PER_PAGE)) / NUM_SEC_PER_PAGE,
                              tmp_buf, bio);
    }
    kfree(tmp_buf);
    return ret;
}

void ssd_parameter_dump(void) {
    struct ssd *ssd = &gdev;
    printk(KERN_INFO "Log region\n");
    printk(KERN_INFO "  Region start : %llX\n", (uint64_t) ssd->bytefs_log_region_start);
    printk(KERN_INFO "  Region end   : %llX\n", (uint64_t) ssd->bytefs_log_region_end);
    printk(KERN_INFO "  Read ptr     : %llX\n", (uint64_t) ssd->log_rp);
    printk(KERN_INFO "  Write ptr    : %llX\n", (uint64_t) ssd->log_wp);
    printk(KERN_INFO "  Log size     : %llX\n", (uint64_t) ssd->log_size);
    printk(KERN_INFO "  Pneding Flush: %s\n", ssd->log_flush_required ? "o" : "x");
    printk(KERN_INFO "Mapping table\n");
    printk(KERN_INFO "  IMT size     : %ld\n", (uint64_t) ssd->imt_size);
    printk(KERN_INFO "  CMT size     : %ld\n", (uint64_t) ssd->cmt_size);
}

void cmt_validation(void) {
    struct ssd *ssd = &gdev;
}

/*
* Unit test for 2B-SSD emulation
*/

// int main() {
//     printf("Total DRAM Cache Buffer Size: %ldB %fMB\n",
//             DRAM_END, (double) DRAM_END / 1024 / 1024);
//     printf("LOG ENTRY SIZE: %ld, LOG TABLE SIZE %d\n", sizeof(struct bytefs_log_entry), LOG_SIZE);

//     ssd_init();
//     // char add[PG_SIZE * 200] = "444";
//     // char add2[PG_SIZE * 200] = "";


//     // struct ssd *ssd = &gdev;
//     // struct ppa ppa = get_new_page(ssd);
//     // mark_page_valid(ssd, &ppa);
//     // mark_page_invalid(ssd, &ppa);

//     // exit(0);

//     unsigned long workset_size = PG_SIZE * 2000;
//     char *garbage = (char *) kzalloc(workset_size);
//     for (int i = 0; i < PG_SIZE; i++) {
//         memset(garbage, i % 256, workset_size / PG_SIZE);
//     }

//     byte_issue(1, 0, workset_size, garbage);
//     for (int i = 0; i < 1000000; i++) {
//         unsigned long byte_size = (rand() * LOG_DSIZE_GRANULARITY) % PG_SIZE;
//         unsigned long lpa = rand() % (workset_size - byte_size);
//         // byte_issue(1, )
//         // nvme_issue(1, rand(), rand() % 10, garbage[rand()]);
//     }

//     return 0;
// }