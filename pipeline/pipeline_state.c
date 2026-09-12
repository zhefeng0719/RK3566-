/* 公共管线类型和函数声明。 */
#include "pipeline_types.h"

/* memset 用于清空结构体。 */
#include <string.h>

/* 初始化三线程共享状态。 */
int pipeline_state_init(pipeline_state_t *state)
{
    /* 先把所有字段清零，确保 stop/计数/指针都是确定值。 */
    memset(state, 0, sizeof(*state));
    /* 初始化保护最新帧的互斥锁。 */
    pthread_mutex_init(&state->frame_mutex, NULL);
    /* 初始化新帧条件变量。 */
    pthread_cond_init(&state->frame_cond, NULL);
    /* 初始化保护最新检测结果的互斥锁。 */
    pthread_mutex_init(&state->result_mutex, NULL);
    /* 当前函数总是成功，返回 0。 */
    return 0;
}

/* 销毁三线程共享状态。 */
void pipeline_state_deinit(pipeline_state_t *state)
{
    /* 先锁住 frame_mutex，避免释放 latest_frame 时其他线程还在访问。 */
    pthread_mutex_lock(&state->frame_mutex);
    /* 如果还持有最后一帧 AVFrame，就释放它。 */
    if (state->latest_frame.frame) {
        av_frame_free(&state->latest_frame.frame);
    }
    /* 释放完后解锁。 */
    pthread_mutex_unlock(&state->frame_mutex);

    /* 销毁保护最新帧的互斥锁。 */
    pthread_mutex_destroy(&state->frame_mutex);
    /* 销毁新帧条件变量。 */
    pthread_cond_destroy(&state->frame_cond);
    /* 销毁保护检测结果的互斥锁。 */
    pthread_mutex_destroy(&state->result_mutex);
}

/* 请求整条管线停止。 */
void pipeline_request_stop(pipeline_state_t *state)
{
    /* 设置全局停止标志。 */
    state->stop = 1;
    /* 锁住 frame_mutex，准备广播条件变量。 */
    pthread_mutex_lock(&state->frame_mutex);
    /* 唤醒所有可能正在等待 frame_cond 的线程。 */
    pthread_cond_broadcast(&state->frame_cond);
    /* 解锁。 */
    pthread_mutex_unlock(&state->frame_mutex);
}
