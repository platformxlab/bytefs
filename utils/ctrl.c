#include <time.h>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/time.h>

#define BYTEFS_SYSCALL_NUM 	(428)

// #define TOTAL_SIZE					(4UL * 1024 * 1024 * 1024)
#define TOTAL_SIZE					(524288UL * 4096 * 80 / 100)
#define ACCESS_GRANDULARITY (64)
#define SIZE_MASK_INV 			(~(ACCESS_GRANDULARITY - 1))

#define PG_SIZE   											(4096UL)
#define BYTEFS_LOG_REGION_GRANDULARITY 	(64UL)

#define TEMP_ARRAY_LEN  (25)

static int ceil_div(int x, int y) {
  return (x + y - 1) / y;
}


struct ssd_stat {
    // total issue counter
    uint64_t block_rissue_count;
    uint64_t block_wissue_count;
    uint64_t byte_rissue_count;
    uint64_t byte_wissue_count;
    // total traffic
    uint64_t block_rissue_traffic;
    uint64_t block_wissue_traffic;
    uint64_t byte_rissue_traffic;
    uint64_t byte_wissue_traffic;
    // meta data
    uint64_t inode_traffic;
    uint64_t superblock_traffic;
    uint64_t bitmap_traffic;
    uint64_t journal_traffic;
    uint64_t dp_traffic;
    // traffic
    uint64_t block_metadata_issue_traffic_r;
    uint64_t block_metadata_issue_traffic_w;
    uint64_t block_data_traffic_r;
    uint64_t block_data_traffic_w;
    uint64_t byte_metadata_issue_traffic_r;
    uint64_t byte_metadata_issue_traffic_w;
    uint64_t byte_data_traffic_r;
    uint64_t byte_data_traffic_w;
    // log based stats
    uint64_t log_wr_op;
    uint64_t log_rd_op;
    uint64_t log_rd_log_page_partial_hit;
    uint64_t log_rd_log_page_hit;
    uint64_t log_direct_rd_page;
    uint64_t log_coalescing_rd_page;
    uint64_t log_wr_page;
    uint64_t log_append;
    uint64_t log_flushes;
    uint64_t byte_issue_nand_wr_modified_distribution[PG_SIZE / BYTEFS_LOG_REGION_GRANDULARITY];
    uint64_t byte_issue_nand_rd_modified_distribution[PG_SIZE / BYTEFS_LOG_REGION_GRANDULARITY];
    // latency
    uint64_t total_r_lat;
    uint64_t total_w_lat;
    uint64_t prog_lat;
    // page cache related 
    uint64_t page_cache_rd_hit;
    uint64_t page_cache_rd_miss;
    uint64_t page_cache_wr_hit;
    uint64_t page_cache_wr_miss;
    uint64_t page_cache_flush_traffic;
    uint64_t page_cache_actuall_w_traffic;
    // locality related
    uint64_t locality_info[PG_SIZE / BYTEFS_LOG_REGION_GRANDULARITY][128][2]; 
    // temp / test
    uint64_t temp[TEMP_ARRAY_LEN];
};

int main(int argc, char *argv[]) {
  assert(argc >= 2);
  int opt = atoi(argv[1]);
  switch (opt) {
    case 4: {
      // read
      assert(argc >= 4);
      uint64_t access_start = atoll(argv[2]) & SIZE_MASK_INV;
      uint64_t access_size = atoll(argv[3]) & SIZE_MASK_INV;
      void *buf = malloc((ceil_div(access_size, sizeof(uint64_t)) + 2) * sizeof(uint64_t));
      ((uint64_t *) buf)[0] = access_start;
      ((uint64_t *) buf)[1] = access_size;
      int retval = syscall(BYTEFS_SYSCALL_NUM, 4, buf);
      printf("Start: %010lX Size: %010lX (%ld)\n", access_start, access_size, access_size);
      for (uint64_t i = 0; i < access_size; i += sizeof(uint64_t)) {
        uint64_t val = ((uint64_t *) buf)[i / sizeof(uint64_t) + 2];
        if (val != 0)
          printf("%010lX + %010lX: %016lX\n", access_start, i * sizeof(uint64_t), val);
      }
      free(buf);
      return retval;
    }
    case 5: {
      // write
      assert(argc >= 4);
      uint64_t access_start = atoll(argv[2]) & SIZE_MASK_INV;
      uint64_t access_size = atoll(argv[3]) & SIZE_MASK_INV;
      void *buf = malloc((ceil_div(access_size, sizeof(uint64_t)) + 2) * sizeof(uint64_t));
      memset(buf, 0, access_size + 2 * sizeof(uint64_t));
      ((uint64_t *) buf)[0] = access_start;
      ((uint64_t *) buf)[1] = access_size;
      if (argc >= 5) strcpy(buf + 2 * sizeof(uint64_t), argv[4]);
      printf("Start: %010lX Size: %010lX (%ld)\n", access_start, access_size, access_size);
      for (uint64_t i = 0; i < access_size; i += sizeof(uint64_t)) {
        uint64_t val = ((uint64_t *) buf)[i / sizeof(uint64_t) + 2];
        if (val != 0)
          printf("%010lX + %010lX: %016lX\n", access_start, i * sizeof(uint64_t), val);
      }
      int retval = syscall(BYTEFS_SYSCALL_NUM, 5, buf);
      free(buf);
      return retval;
    }
    case 6: {
      assert(argc >= 3);
      struct timeval cur_time;
      gettimeofday(&cur_time, NULL);
      srand(cur_time.tv_usec);
      uint64_t num_pgs = atoll(argv[2]);
      uint64_t access_size = (64 * num_pgs) & SIZE_MASK_INV;
      uint64_t access_start = (rand() % (TOTAL_SIZE - access_size)) & SIZE_MASK_INV;
      void *test_buf = malloc(ceil_div(access_size, sizeof(uint64_t)) * sizeof(uint64_t));
      void *buf = malloc((ceil_div(access_size, sizeof(uint64_t)) + 2) * sizeof(uint64_t));
      void *data_buf = buf + 2 * sizeof(uint64_t);
      for (uint64_t i = 0; i < access_size; i++) {
        ((uint8_t *) test_buf)[i] = (uint8_t) rand();
      }
      memcpy(data_buf, test_buf, access_size);

      ((uint64_t *) buf)[0] = access_start;
      ((uint64_t *) buf)[1] = access_size;
      printf("Start: %lX, Size: %ld\n", ((uint64_t *) buf)[0], ((uint64_t *) buf)[1]);
      syscall(BYTEFS_SYSCALL_NUM, 5, buf);

      ((uint64_t *) buf)[0] = access_start;
      ((uint64_t *) buf)[1] = access_size;
      memset(data_buf, 0, access_size);
      syscall(BYTEFS_SYSCALL_NUM, 4, buf);
      int retval = 0;
      for (int i = 0; i < access_size; i += sizeof(uint64_t)) {
        if (((uint64_t *) (data_buf))[i / sizeof(uint64_t)] != ((uint64_t *) test_buf)[i / sizeof(uint64_t)]) {
          printf("%5d %016lX %016lX\n", i, 
              ((uint64_t *) data_buf)[i / sizeof(uint64_t)], 
              ((uint64_t *) test_buf)[i / sizeof(uint64_t)]);
          retval = 1;
        }
      }
      free(test_buf);
      free(buf);
      return retval;
    }
    case 42: {
      struct ssd_stat stat;
      int retval = syscall(BYTEFS_SYSCALL_NUM, 42, &stat);      
      printf("============= ByteFS report =============\n");
      // log based stats
      int short_field_len = 10;
      int long_field_len = 20;
      printf("  Issue count\n");
      printf("    Block issue count:      %-*lu = R: %-*lu + W: %-*lu \n", 
          short_field_len, stat.block_rissue_count + stat.block_wissue_count,
          short_field_len, stat.block_rissue_count,
          short_field_len, stat.block_wissue_count);
      printf("    Byte issue count:       %-*lu = R: %-*lu + W: %-*lu \n", 
          short_field_len, stat.byte_rissue_count + stat.byte_wissue_count,
          short_field_len, stat.byte_rissue_count,
          short_field_len, stat.byte_wissue_count);

      printf("  Traffic in bytes\n");
      printf("    Block issue traffic:    %-*lu = R: %-*lu + W: %-*lu \n", 
          long_field_len, stat.block_rissue_traffic + stat.block_wissue_traffic,
          long_field_len, stat.block_rissue_traffic,
          long_field_len, stat.block_wissue_traffic);
      printf("    Byte issue traffic:     %-*lu = R: %-*lu + W: %-*lu \n", 
          long_field_len, stat.byte_rissue_traffic + stat.byte_wissue_traffic,
          long_field_len, stat.byte_rissue_traffic,
          long_field_len, stat.byte_wissue_traffic);
      
      printf("  Traffic breakdown \n");
      printf("    Block data traffic:     %-*lu = R: %-*lu + W: %-*lu \n",
          long_field_len, stat.block_data_traffic_r+stat.block_data_traffic_w, 
          long_field_len, stat.block_data_traffic_r,
          long_field_len, stat.block_data_traffic_w);
      printf("    Block meta traffic:     %-*lu = R: %-*lu + W: %-*lu \n",
          long_field_len, stat.block_metadata_issue_traffic_r+stat.block_metadata_issue_traffic_w,
          long_field_len, stat.block_metadata_issue_traffic_r,
          long_field_len, stat.block_metadata_issue_traffic_w);
      printf("    Byte data traffic:      %-*lu = R: %-*lu + W: %-*lu \n",
          long_field_len, stat.byte_data_traffic_r+stat.byte_data_traffic_w,
          long_field_len, stat.byte_data_traffic_r,
          long_field_len, stat.byte_data_traffic_w);
      printf("    Byte meta traffic:      %-*lu = R: %-*lu + W: %-*lu \n",
          long_field_len, stat.byte_metadata_issue_traffic_r+stat.byte_metadata_issue_traffic_w,
          long_field_len, stat.byte_metadata_issue_traffic_r,
          long_field_len, stat.byte_metadata_issue_traffic_w);

      printf("  Metadata\n");
      printf("    Inode traffic:          %-*lu\n", long_field_len, stat.inode_traffic);
      printf("    Superblock traffic:     %-*lu\n", long_field_len, stat.superblock_traffic);
      printf("    Bitmap traffic:         %-*lu\n", long_field_len, stat.bitmap_traffic);
      printf("    Journal traffic:        %-*lu\n", long_field_len, stat.journal_traffic);
      printf("    Journal traffic:        %-*lu\n", long_field_len, stat.dp_traffic);

      printf("  Log\n");
      printf("    Write operation:        %-*lu\n", long_field_len, stat.log_wr_op);
      printf("    Read operation:         %-*lu\n", long_field_len, stat.log_rd_op);
      printf("    Read partial hit:       %-*lu\n", long_field_len, stat.log_rd_log_page_partial_hit);
      printf("    Read log hit:           %-*lu\n", long_field_len, stat.log_rd_log_page_hit);
      printf("    Direct NAND read:       %-*lu\n", long_field_len, stat.log_direct_rd_page);
      printf("    Coalescing NAND read:   %-*lu\n", long_field_len, stat.log_coalescing_rd_page);
      printf("    NAND write:             %-*lu\n", long_field_len, stat.log_wr_page);
      printf("    Log append:             %-*lu\n", long_field_len, stat.log_append);
      printf("    Log flush:              %-*lu\n", long_field_len, stat.log_flushes);

      printf("  Page Cache\n");
      printf("    Page cache read:        %-*lu = Hit: %-*lu + Miss: %-*lu \n", 
          long_field_len, (stat.page_cache_rd_hit + stat.page_cache_rd_miss),
          long_field_len, stat.page_cache_rd_hit,
          long_field_len, stat.page_cache_rd_miss);
      printf("    Page cache write:       %-*lu = Hit: %-*lu + Miss: %-*lu \n", 
          long_field_len, (stat.page_cache_wr_hit + stat.page_cache_wr_miss),
          long_field_len, stat.page_cache_wr_hit,
          long_field_len, stat.page_cache_wr_miss);
      printf("    Page cache flush traffic:            %-*lu\n", long_field_len, stat.page_cache_flush_traffic);
      printf("    Page cache actual write traffic:     %-*lu\n", long_field_len, stat.page_cache_actuall_w_traffic);

      printf("  Latency\n");
      printf("    Total read latency:     %-*lu\n", long_field_len, stat.total_r_lat);
      printf("    Total write latency:    %-*lu\n", long_field_len, stat.total_w_lat);
      printf("    Total program latency:  %-*lu\n", long_field_len, stat.prog_lat);

      printf("========== ByteFS rw modification distribution ==========\n");
      double total_byte_issue_nand_wr_modified = 0;
      double total_byte_issue_nand_rd_modified = 0;
      for (int i = 0; i < PG_SIZE / BYTEFS_LOG_REGION_GRANDULARITY; i++) {
        total_byte_issue_nand_wr_modified += 
            stat.byte_issue_nand_wr_modified_distribution[i];
        total_byte_issue_nand_rd_modified += 
            stat.byte_issue_nand_rd_modified_distribution[i];
      }
      for (int i = 0; i < PG_SIZE / BYTEFS_LOG_REGION_GRANDULARITY; i++) {
        printf("%3.0f%%: r %7.3f%%, w %7.3f%%   ",
        // printf("%3.0f%%, %7.3f%%, %7.3f%%\n",
            100.0 * (i + 1) / (PG_SIZE / BYTEFS_LOG_REGION_GRANDULARITY),
            100.0 * stat.byte_issue_nand_wr_modified_distribution[i] / 
                total_byte_issue_nand_wr_modified,
            100.0 * stat.byte_issue_nand_rd_modified_distribution[i] / 
                total_byte_issue_nand_rd_modified);
        if (i % 4 == 3) printf("\n");
      }
      printf("  Temp / Test\n");
      for (int i = 0; i < TEMP_ARRAY_LEN; i++) {
        printf("    Val %3d: %-*lu\n", i, long_field_len, stat.temp[i]);
      }
      return retval;
    }
    case 7: {
      assert(argc >= 4);
      struct timeval cur_time;
      gettimeofday(&cur_time, NULL);
      srand(cur_time.tv_usec);
      uint64_t num_pgs = atoll(argv[2]);
      uint64_t num_iter = atoll(argv[3]);
      uint64_t access_size = (4096 * num_pgs) & SIZE_MASK_INV;
      void *test_buf = malloc(ceil_div(access_size, sizeof(uint64_t)) * sizeof(uint64_t));
      void *buf = malloc((ceil_div(access_size, sizeof(uint64_t)) + 2) * sizeof(uint64_t));

      int retval = 0;
      for (uint64_t iter = 0; iter < num_iter; iter++) {
        uint64_t access_start = (rand() % (TOTAL_SIZE - access_size)) & SIZE_MASK_INV;
        void *data_buf = buf + 2 * sizeof(uint64_t);
        for (uint64_t i = 0; i < access_size; i++) {
          ((uint8_t *) test_buf)[i] = (uint8_t) rand();
        }
        memcpy(data_buf, test_buf, access_size);

        ((uint64_t *) buf)[0] = access_start;
        ((uint64_t *) buf)[1] = access_size;
        printf("Start: %lX, Size: %ld\n", ((uint64_t *) buf)[0], ((uint64_t *) buf)[1]);
        syscall(BYTEFS_SYSCALL_NUM, 5, buf);

        ((uint64_t *) buf)[0] = access_start;
        ((uint64_t *) buf)[1] = access_size;
        memset(data_buf, 0, access_size);
        syscall(BYTEFS_SYSCALL_NUM, 4, buf);
        for (int i = 0; i < access_size; i += sizeof(uint64_t)) {
          if (((uint64_t *) (data_buf))[i / sizeof(uint64_t)] != ((uint64_t *) test_buf)[i / sizeof(uint64_t)]) {
            printf("%5d %016lX %016lX\n", i, 
                ((uint64_t *) data_buf)[i / sizeof(uint64_t)], 
                ((uint64_t *) test_buf)[i / sizeof(uint64_t)]);
            retval = 1;
          }
        }
      }

      free(test_buf);
      free(buf);
      return retval;
    }
    case 8: {
      assert(argc >= 4);
      struct timeval cur_time;
      gettimeofday(&cur_time, NULL);
      srand(cur_time.tv_usec);
      uint64_t num_log_blk = atoll(argv[2]);
      uint64_t num_iter = atoll(argv[3]);
      uint64_t access_size = ACCESS_GRANDULARITY * num_log_blk;
      void *test_buf = malloc(ceil_div(access_size, sizeof(uint64_t)) * sizeof(uint64_t));
      void *buf = malloc((ceil_div(access_size, sizeof(uint64_t)) + 2) * sizeof(uint64_t));

      int retval = 0;
      for (uint64_t iter = 0; iter < num_iter; iter++) {
        uint64_t access_start = (rand() % (TOTAL_SIZE - access_size)) & SIZE_MASK_INV;
        void *data_buf = buf + 2 * sizeof(uint64_t);
        for (uint64_t i = 0; i < access_size; i++) {
          ((uint8_t *) test_buf)[i] = (uint8_t) rand();
        }
        memcpy(data_buf, test_buf, access_size);

        ((uint64_t *) buf)[0] = access_start;
        ((uint64_t *) buf)[1] = access_size;
        printf("Start: %lX, Size: %ld\n", ((uint64_t *) buf)[0], ((uint64_t *) buf)[1]);
        syscall(BYTEFS_SYSCALL_NUM, 5, buf);

        ((uint64_t *) buf)[0] = access_start;
        ((uint64_t *) buf)[1] = access_size;
        memset(data_buf, 0, access_size);
        syscall(BYTEFS_SYSCALL_NUM, 4, buf);
        for (int i = 0; i < access_size; i += sizeof(uint64_t)) {
          if (((uint64_t *) (data_buf))[i / sizeof(uint64_t)] != ((uint64_t *) test_buf)[i / sizeof(uint64_t)]) {
            printf("%5d %016lX %016lX\n", i, 
                ((uint64_t *) data_buf)[i / sizeof(uint64_t)], 
                ((uint64_t *) test_buf)[i / sizeof(uint64_t)]);
            retval = 1;
          }
        }
      }

      free(test_buf);
      free(buf);
      return retval;
    }
    case 906: {
      assert(argc >= 3);
      void *buf = malloc(sizeof(uint64_t));
      uint64_t size = atoll(argv[2]);
      if (size == 0) return -1;
      printf("Try pinning %ld MB memory\n", size);
      ((uint64_t *) buf)[0] = size;
      int retval = syscall(BYTEFS_SYSCALL_NUM, opt, buf);
      free(buf);
      return retval;
    }
    default:
      return syscall(BYTEFS_SYSCALL_NUM, opt, 0);
  }
}

