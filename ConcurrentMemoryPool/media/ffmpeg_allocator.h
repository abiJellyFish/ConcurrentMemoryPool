// media/ffmpeg_allocator.h
#pragma once

extern "C" {
#include <libavutil/buffer.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

// 初始化内存池分配器
void init_ffmpeg_allocator();

// 用内存池分配一个 AVBufferRef
AVBufferRef* av_buffer_alloc_from_pool(size_t size);

// 提供给 AVCodecContext::get_buffer2 的回调
int custom_get_buffer2(struct AVCodecContext *ctx, AVFrame *frame, int flags);

// 打印内存池统计信息
void print_mempool_stats();
