#include "ssd_stat.h"
// #include "ftl.h"
// #include "ftl_mapping.h"
// #include "backend.h"
// #include "queue_sort.h"
#include <linux/string.h>
#include <linux/uaccess.h>

// internal stat counters:
struct ssd_stat ssd_stat;

int stat_flag = 0;

int check_stat_state(void){
    return stat_flag;
}
EXPORT_SYMBOL(check_stat_state);

int turn_on_stat(void){
    memset(&ssd_stat, 0, sizeof(struct ssd_stat));
    stat_flag = 1;
    return 0;
}
EXPORT_SYMBOL(turn_on_stat);

int reset_ssd_stat(void){
    memset(&ssd_stat, 0, sizeof(struct ssd_stat));
    stat_flag = 0;
    return 0;
}
EXPORT_SYMBOL(reset_ssd_stat);

int print_stat(void* ptr){
    // copy stat to user ptr
    struct ssd_stat* user_stat = (struct ssd_stat*)ptr;
    copy_to_user(user_stat, &ssd_stat, sizeof(struct ssd_stat));
    return 0;
}
EXPORT_SYMBOL(print_stat);

int ssd_stat_add(int name, unsigned long value){
    switch (name)
    {
    case 0:
        SSD_STAT_ATOMIC_ADD(byte_metadata_issue_traffic_r,value);
        break;

    case 1:
        SSD_STAT_ATOMIC_ADD(byte_metadata_issue_traffic_w,value);
        break;

    case 2:
        SSD_STAT_ATOMIC_ADD(byte_data_traffic_r,value);
        break;

    case 3:
        SSD_STAT_ATOMIC_ADD(byte_data_traffic_w,value);
        break;

    case 4:
        SSD_STAT_ATOMIC_ADD(block_metadata_issue_traffic_r,value);
        break;

    case 5:
        SSD_STAT_ATOMIC_ADD(block_metadata_issue_traffic_w,value);
        break;

    case 6:
        SSD_STAT_ATOMIC_ADD(block_data_traffic_r,value);
        break;

    case 7:
        SSD_STAT_ATOMIC_ADD(block_data_traffic_w,value);
        break;
    
    default:
        break;
    }
}
EXPORT_SYMBOL(ssd_stat_add);