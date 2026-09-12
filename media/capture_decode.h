/* 防止重复包含。 */
#ifndef CAPTURE_DECODE_H
#define CAPTURE_DECODE_H

/* 解码线程需要 app_config_t 和 pipeline_state_t。 */
#include "pipeline_types.h"

/* 解码线程启动参数。 */
typedef struct {
    /* 全局只读配置。 */
    const app_config_t *cfg;
    /* 三线程共享状态。 */
    pipeline_state_t *state;
} decode_thread_arg_t;

/* 解码线程入口函数，供 pthread_create 调用。 */
void *decode_thread_main(void *arg);

/* 头文件结束。 */
#endif
