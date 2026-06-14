// media/ffmpeg_allocator.cpp
#include "ffmpeg_allocator.h"
#include <new>
#include <stdexcept>
#include <cstring>
#include <cstdio>
#include <atomic>

extern "C" {
#include <libavutil/pixdesc.h>
}

// 直接包含你内存池的底层头文件
#include "../mempool/ConcurrentAlloc.h"

// 统计计数器
static std::atomic<size_t> g_pool_alloc_count{0};
static std::atomic<size_t> g_pool_free_count{0};
static std::atomic<size_t> g_pool_alloc_bytes{0};

// 释放回调携带的信息
struct PoolBufferOpaque {
    size_t size;
};

// 自定义释放回调：将内存归还给内存池
// ★ 修改释放回调，使用 raw_ptr
static void pool_buffer_free(void* opaque, uint8_t* data) {
    auto* op = static_cast<PoolBufferOpaque*>(opaque);
    if (op && data) {
        // 从对齐指针恢复原始指针
        void** anchor = reinterpret_cast<void**>(data) - 1;
        void* raw_ptr = *anchor;
        ConcurrentFree(raw_ptr);
        g_pool_free_count++;
    }
    delete op;
}

void init_ffmpeg_allocator() {
    printf("[MemPool] FFmpeg allocator initialized\n");
}

void print_mempool_stats() {
    printf("\n========== Memory Pool Statistics ==========\n");
    printf("Total allocations: %zu\n", g_pool_alloc_count.load());
    printf("Total frees:       %zu\n", g_pool_free_count.load());
    printf("Total bytes alloc: %zu (%.2f MB)\n", 
           g_pool_alloc_bytes.load(), 
           g_pool_alloc_bytes.load() / (1024.0 * 1024.0));
    printf("=============================================\n\n");
}

AVBufferRef* av_buffer_alloc_from_pool(size_t size) {
    if (size == 0) return nullptr;

    // ★ FFmpeg 要求 64 字节对齐，额外分配空间来保证对齐
    const size_t alignment = 64;
    size_t alloc_size = size + alignment;  // 多分配一些用于对齐
    
    void* raw_ptr = ConcurrentAlloc(alloc_size);
    if (!raw_ptr) {
        fprintf(stderr, "[MemPool] Failed to allocate %zu bytes\n", alloc_size);
        return nullptr;
    }

    // ★ 手动对齐指针
    uint8_t* aligned_ptr = reinterpret_cast<uint8_t*>(
        (reinterpret_cast<uintptr_t>(raw_ptr) + alignment - 1) & ~(alignment - 1)
    );
    
    // 在 aligned_ptr 前面保存 raw_ptr，用于释放
    void** anchor = reinterpret_cast<void**>(aligned_ptr) - 1;
    *anchor = raw_ptr;

    g_pool_alloc_count++;
    g_pool_alloc_bytes += alloc_size;

    // 清零对齐后的内存
    std::memset(aligned_ptr, 0, size);

    auto* op = new (std::nothrow) PoolBufferOpaque{alloc_size};
    if (!op) {
        ConcurrentFree(raw_ptr);
        g_pool_free_count++;
        return nullptr;
    }

    AVBufferRef* buf = av_buffer_create(
        aligned_ptr,  // ★ 使用对齐后的指针
        size, 
        pool_buffer_free, 
        op, 
        0
    );
    
    if (!buf) {
        ConcurrentFree(raw_ptr);
        g_pool_free_count++;
        delete op;
        return nullptr;
    }
    
    return buf;
}
int custom_get_buffer2(AVCodecContext *ctx, AVFrame *frame, int flags) {
    // 先用默认方式获取帧格式信息和 linesize
    int ret = avcodec_default_get_buffer2(ctx, frame, flags);
    if (ret < 0) return ret;

    // 保存默认分配的信息
    const AVPixelFormat pix_fmt = static_cast<AVPixelFormat>(frame->format);
    int linesizes[4] = {0};
    int actual_sizes[4] = {0};  // 记录实际需要的大小
    
    for (int i = 0; i < 4; i++) {
        linesizes[i] = frame->linesize[i];
        if (frame->buf[i]) {
            actual_sizes[i] = frame->buf[i]->size;  // FFmpeg 自己算的大小
        }
    }
    
    if (linesizes[0] <= 0) {
        av_frame_unref(frame);
        return AVERROR(EINVAL);
    }

    // 释放默认分配的缓冲区引用
    av_frame_unref(frame);

    // ★ 直接用 FFmpeg 算好的大小，不多加
    for (int i = 0; i < 4 && actual_sizes[i] > 0; i++) {
        AVBufferRef *buf = av_buffer_alloc_from_pool(actual_sizes[i]);
        if (!buf) {
            av_frame_unref(frame);
            return AVERROR(ENOMEM);
        }
        
        frame->buf[i] = buf;
        frame->data[i] = buf->data;
        frame->linesize[i] = linesizes[i];
    }

    if (frame->data[0]) {
        frame->extended_data = frame->data;
    }

    frame->format = pix_fmt;
    frame->width = ctx->width;
    frame->height = ctx->height;

    return 0;
}
