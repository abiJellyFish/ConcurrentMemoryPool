# 应用于 FFmpeg 实时音视频处理的并发MemoryPool项目

- 环境
    - Ubuntu 20.04.6 LTS
    - gcc 9.4.0
    - g++ 9.4.0
    - cmake version 3.16.3

- 演示视频
    - 来源为 W3Schools 示例视频，H.264/MP4 格式
    - wget "https://www.w3schools.com/html/movie.mp4" -O movie.mp4
    - 截取 5s 时长测试
    - ffmpeg -i movie.mp4 -t 5 -c copy test.mp4

## 项目概述

将 C++ 并发内存池集成到 FFmpeg 转码管线中，替换视频帧缓冲区的动态内存分配，实现对实时音视频处理场景的内存管理优化。测试项目在实际应用中的表现。

- 使用 Valgrind + Massif 分析堆内存工具，获取视频播放时的内存管理情况

![alt text](image.png)

第一部分文件调用内存池接口，第二部分使用FFmpeg原生内存管理。

| 指标 | 内存池版本 | 原生 malloc | 说明 |
|------|-----------|-------------|------|
| 执行时间 | 0.02s | 0.34s | 17 倍 |
| 内存占用 | 30.7 MB | 35.7 MB | 减少 10% |
| 缺页中断 | 2,430 | 3,528 | 减少 30% |
| 系统调用 | 0.00s | 0.01s | 避免内核态切换 |

---

- 项目架构

```
ConcurrentMemoryPoolOptimize/
├── mempool/                     # 内存池
│   ├── ConcurrentAlloc.h/cpp    # 对外接口：ConcurrentAlloc/ConcurrentFree
│   ├── ThreadCache.h/cpp        # 线程本地缓存（L1）
│   ├── CentralCache.h/cpp       # 中央缓存（L2）
│   ├── PageCache.h/cpp          # 页缓存（L3）
│   ├── Common.h                 # 基本类
│   └── ConcurrentAllocator.h    # STL 分配器
│
├── media/                       # FFmpeg 适配层
│   └── ffmpeg_allocator.h/cpp   # 封装为 AVBufferRef 分配器
│
├── demo/                        # 演示程序
│   ├── transcode.cpp            # 内存池转码版本
│   └── transcode_native.cpp     # 原生 malloc 版本
```

| 决策 | 原因 |
|------|------|
| 替换编码器输入帧 | 解码器 `get_buffer2` 与 FFmpeg 内部状态耦合，安全第一 |
| 使用 FFmpeg 计算的 `buf->size` | 确保缓冲区大小完全匹配 SIMD 对齐要求（64 字节） |

---

- 内存池功能说明

```
原生 malloc 路径:
  malloc() → sys_brk() → 内核态 → 缺页处理 → 返回
  每次耗时: 500ns（缺页时 ~10μs）

内存池路径:
  ThreadCache::Allocate() → freelist.pop() → 返回
  每次耗时: 10ns（用户态操作）
```

- 减少内存碎片：内存池统一管理，避免 `malloc` 产生外部碎片。
- 复用机制：释放的内存立即回到 freelist，下次分配直接使用。
- 同一帧的多个平面在物理内存中更接近，使用`PageCache` 一次向 OS 预分配 1MB 以上的大页，减少缺页中断。

---

## 替换接口说明

### 编码器输入帧缓冲区

- 原生方式（`transcode_native.cpp`）：
```cpp
// FFmpeg 内部通过 av_malloc 分配，底层调用 posix_memalign
enc_frame->format = enc_ctx->pix_fmt;
enc_frame->width  = enc_ctx->width;
enc_frame->height = enc_ctx->height;
av_frame_get_buffer(enc_frame, 0);  // 默认使用 av_malloc
```

- 内存池方式（`transcode.cpp`）：
```cpp
AVBufferRef* av_buffer_alloc_from_pool(size_t size) {
    if (size == 0) return nullptr;

    void* data_ptr = ConcurrentAlloc(size);
// 其他代码
}

// 1. 先用 FFmpeg 获取正确的 linesize 和所需大小
av_frame_get_buffer(enc_frame, 0);
for (int i = 0; i < 4; i++) {
    linesizes[i] = enc_frame->linesize[i];
    actual_sizes[i] = enc_frame->buf[i]->size;  // FFmpeg 计算的对齐大小
}
av_frame_unref(enc_frame);

// 2. 用内存池重新分配
for (int i = 0; i < 4 && actual_sizes[i] > 0; i++) {
    AVBufferRef* buf = av_buffer_alloc_from_pool(actual_sizes[i]);
    enc_frame->buf[i] = buf;
    enc_frame->data[i] = buf->data;
    enc_frame->linesize[i] = linesizes[i];
}
```

### FFmpeg Buffer 释放回调

`media/ffmpeg_allocator.cpp`
```cpp
// 自定义释放回调 → 归还内存给内存池
static void pool_buffer_free(void* opaque, uint8_t* data) {
    auto* op = static_cast<PoolBufferOpaque*>(opaque);
    if (op && data) {
        ConcurrentFree(data);  // 归还到线程缓存
    }
    delete op;
}

// 创建 AVBufferRef 时注册回调
AVBufferRef* buf = av_buffer_create(
    data_ptr, size, pool_buffer_free, op, 0);
```

---

## 音视频技术原理

### 核心 API

| API | 功能 | 所在流程 |
|-----|------|---------|
| `avformat_open_input()` | 打开输入文件/流 | 解封装 |
| `avformat_find_stream_info()` | 探测流信息 | 解封装 |
| `avcodec_find_decoder()` | 查找解码器 | 解码 |
| `avcodec_open2()` | 打开编解码器 | 编解码 |
| `avcodec_send_packet()` | 发送压缩包到解码器 | 解码 |
| `avcodec_receive_frame()` | 接收解码后的帧 | 解码 |
| `av_frame_get_buffer()` | 为 AVFrame 分配缓冲区 | 帧管理 |
| `av_buffer_create()` | 创建引用计数缓冲区 | 替换为内存池实现 |
| `sws_getContext()` / `sws_scale()` | 像素格式/尺寸转换 | 色彩空间 |
| `avcodec_send_frame()` | 发送原始帧到编码器 | 编码 |
| `avcodec_receive_packet()` | 接收编码后的包 | 编码 |
| `av_interleaved_write_frame()` | 写入输出文件 | 封装 |
| `av_packet_rescale_ts()` | 时间戳转换 | 时间基管理 |

```
AVFormatContext  ← 容器格式上下文（MP4）
    └── AVStream[]  ← 流（视频/音频）
         └── AVCodecParameters  ← 编解码参数

AVCodecContext  ← 编解码器上下文
    ├── get_buffer2()  ← 帧缓冲区分配回调
    └── pix_fmt, width, height

AVFrame  ← 原始帧（YUV/PCM）
    ├── data[4]      ← 各平面数据指针
    ├── linesize[4]  ← 各平面行步长
    └── buf[4]       ← AVBufferRef 引用

AVPacket  ← 压缩数据包（H.264/AAC）
    ├── data, size
    ├── pts, dts     ← 时间戳
    └── stream_index
```

---

### 视频编解码管线

```
┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐
│  Input   │ → │  Demux   │ → │  Decode  │ → │  Scale   │
│  File    │    │  解封装  │    │  解码    │    │ 色彩转换 │
└──────────┘    └──────────┘    └──────────┘    └──────────┘
                                                     ↓
┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐
│  Output  │ ← │   Mux    │ ← │  Encode  │ ← │  Frame   │
│  File    │    │  封装    │    │  编码    │    │  Buffer  │
└──────────┘    └──────────┘    └──────────┘    └──────────┘
                                                     ↑
                                                 内存池替换
```

### 视频像素格式 ：YUV420P

```
Y 平面 (亮度):  width × height        = 320×240 = 76,800 字节
U 平面 (色度):  width/2 × height/2    = 160×120 = 19,200 字节
V 平面 (色度):  width/2 × height/2    = 160×120 = 19,200 字节

5 秒 × 25fps = 125 帧
总数据量: 125 × 115,200 ≈ 14.4 MB
```

- 每帧 115KB，125 帧需要反复分配/释放 ~14MB
- 传统 `malloc` 每次都触发系统调用（`brk`/`mmap`）
- 内存池预分配大块内存，后续分配/释放是 O(1) 的指针操作

### H.264 编码参数

```
GOP (Group of Pictures):    12 帧
B-frames:                   1
码率:                       400 kbps
Preset:                     fast（编码速度优先）
Tune:                       zerolatency（低延迟）
```

---


## 八、后续扩展方向

1. **音频缓冲区替换**：将 `AVFrame` 的音频缓冲区也用内存池管理
2. **RTP 包池**：为网络传输的 RTP 包预分配对象池
3. **完全替换 `av_malloc`**：通过 `av_malloc` 的 hook 机制全局接管
4. **大页内存 + DMA**：使用 `HugePage` 支持摄像头 DMA 零拷贝
