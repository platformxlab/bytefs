#include "ssd_stat.h"
// #include "ftl.h"
// #include "ftl_mapping.h"
// #include "backend.h"
// #include "queue_sort.h"
#include <linux/string.h>
#include <linux/uaccess.h>

// internal stat counters:
struct ssd_stat stat;
int stat_flag = 0;

inline int check_stat_state(void) {
    return stat_flag;
}
int turn_on_stat(void){
    memset(&stat, 0, sizeof(struct ssd_stat));
    stat_flag = 1;
    return 0;
}
int reset_ssd_stat(void){
    memset(&stat, 0, sizeof(struct ssd_stat));
    stat_flag = 0;
    return 0;
}
int print_stat(void* ptr){
    // copy stat to user ptr
    struct ssd_stat* user_stat = (struct ssd_stat*)ptr;
    copy_to_user(user_stat, &stat, sizeof(struct ssd_stat));
    return 0;
}