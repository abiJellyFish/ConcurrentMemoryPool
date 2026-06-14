#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include "ConcurrentAlloc.h"
#include "CentralCache.h"

// 是否启用工作窃取
static std::atomic<bool> g_workStealingEnabled(true);

// TLS全局对象指针，由ThreadCache.cpp定义
extern thread_local ThreadCache* pTLSThreadCache;

// 确保线程缓存已初始化（懒初始化 + 线程安全）
static ThreadCache* EnsureTLSThreadCache() {
    if (pTLSThreadCache == nullptr) {
        // 双重检查锁定
        static std::mutex initMtx;
        std::lock_guard<std::mutex> lock(initMtx);
        if (pTLSThreadCache == nullptr) {
            pTLSThreadCache = new ThreadCache();
            // 设置线程ID
            pTLSThreadCache->SetThreadId(
                std::hash<std::thread::id>()(std::this_thread::get_id()));
            // 注册到中央缓存
            CentralCache::GetInstance()->RegisterThreadCache(pTLSThreadCache);
        }
    }
    return pTLSThreadCache;
}

// 线程申请空间的接口
void* ConcurrentAlloc(size_t size) {
    // 如果申请空间超过256KB，向页面缓存申请空间
    if (size > MAX_BYTES) {
        // 按照页大小对齐
        size_t alignSize = SizeClass::_RoundUp(size, 1 << PAGE_SHIFT);
        size_t k = alignSize >> PAGE_SHIFT; // 对齐之后需要多少页

        // 大对象直接使用 mmap 分配，不进入缓存
        void* ptr = PageCache::GetInstance()->AllocateLarge(k);
        if (ptr == nullptr) {
            // 分配失败，尝试从紧急内存池获取
            if (ENABLE_EMERGENCY_POOL) {
                ptr = EmergencyPool::Instance().Alloc(size);
                if (ptr != nullptr) {
                    return ptr;
                }
            }
            return nullptr; // 内存分配失败
        }

        return ptr;
    }
    else {
        // TLS对象由每个线程独立拥有，不会有线程安全问题
        ThreadCache* tc = EnsureTLSThreadCache();
        if (tc == nullptr) {
            return nullptr;
        }

        void* ptr = tc->Allocate(size);
        if (ptr == nullptr && ENABLE_EMERGENCY_POOL) {
            // 分配失败，尝试从紧急内存池获取
            ptr = EmergencyPool::Instance().Alloc(SizeClass::RoundUp(size));
        }
        return ptr;
    }
}

// 线程回收空间的接口
void ConcurrentFree(void* obj) {
    assert(obj);

    // 通过obj找到对应的span（通过映射）
    Span* span = PageCache::GetInstance()->MapObjectToSpan(obj);
    if (span == nullptr) {
        // 找不到对应的span，可能是内存分配失败导致的
        return;
    }
    size_t size = span->_objSize; // 通过映射的span获取obj指向空间的大小

    // 如果释放空间超过256KB，向页面缓存释放空间
    if (size > MAX_BYTES || span->_n > PAGE_NUM - 1) {
        // 大对象释放
        PageCache::GetInstance()->DeallocateLarge(obj, span->_n);
    }
    else {
        // 确保pTLSThreadCache不为空
        ThreadCache* tc = pTLSThreadCache;
        if (tc == nullptr) {
            tc = EnsureTLSThreadCache();
        }
        if (tc != nullptr) {
            tc->Deallocate(obj, size);
        }
    }

    return;
}

// ========== 内存统计接口 ==========
void GetMemoryStats(MemoryStats& stats) {
    PageCache::GetInstance()->GetMemoryStats(
        stats.totalAllocated,
        stats.totalFreed,
        stats.peakUsage);
    stats.currentPages = PageCache::GetInstance()->GetTotalPages();
}

// ========== 运行时配置接口 ==========
void SetWorkStealingEnabled(bool enabled) {
    g_workStealingEnabled.store(enabled, std::memory_order_relaxed);
}

bool IsWorkStealingEnabled() {
    return g_workStealingEnabled.load(std::memory_order_relaxed);
}

void TryReclaimMemory() {
    PageCache::GetInstance()->TryReclaimIdleSpans();
}
