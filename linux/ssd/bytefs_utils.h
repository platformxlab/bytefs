#ifndef __BYTEFS_UTILS_H_
#define __BYTEFS_UTILS_H_

#include "ftl.h"
#include "bytefs.h"
#include "backend.h"

#define bytefs_err(fmt, ...) \
    do { printk(KERN_ERR "[BYTEFS Err]: " fmt "\n", ## __VA_ARGS__); } while (0)

#define bytefs_warn(fmt, ...) \
    do { printk(KERN_WARNING "[BYTEFS Warn]: " fmt "\n", ## __VA_ARGS__); } while (0)

#define bytefs_log(fmt, ...) \
    do { printk(KERN_INFO "[BYTEFS Log]: " fmt "\n", ## __VA_ARGS__); } while (0)

#define bytefs_expect(cond)                                                     \
    do {                                                                        \
        if (!(cond))                                                            \
            printk(KERN_ERR "[BYTEFS Exception]: %s:%d (%s) %s\n",              \
                __FILE__, __LINE__, __func__, # cond);                          \
    } while (0)


#define bytefs_expect_msg(cond, fmt, ...)                                       \
    do {                                                                        \
        if (!(cond))                                                            \
            printk(KERN_ERR "[BYTEFS Exception]: %s:%d (%s) " fmt "\n",         \
                __FILE__, __LINE__, __func__, ## __VA_ARGS__);                  \
    } while (0)

#define bytefs_assert(cond)                                                     \
    do {                                                                        \
        if (!(cond)) {                                                          \
            printk(KERN_ERR "[!!! BYTEFS Assertion]: %s:%d (%s) %s\n",          \
                __FILE__, __LINE__, __func__, # cond);                          \
        }                                                                       \
    } while (0)
  
            // while (1) { schedule(); };                                          

#define bytefs_assert_msg(cond, fmt, ...)                                       \
    do {                                                                        \
        if (!(cond)) {                                                          \
            printk(KERN_ERR "[!!! BYTEFS Assertion] %s:%d (%s) " fmt "\n",      \
                __FILE__, __LINE__, __func__, ## __VA_ARGS__);                  \
        }                                                                       \
    } while (0)

int ssd_test(void);
int bytefs_start_threads(void);
int bytefs_stop_threads(void);

#endif
