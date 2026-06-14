#pragma once 

#include <mutex>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include "Common.h"
#include "TCMalloc_PageMap3.h"

class PageCache {
public:
    std::mutex _pageMtx;

    static PageCache* GetInstance() {
        return &_sInst;
    }

    // 根据cc的需求，pc从span链表数组中取k页的span
    Span* NewSpan(size_t k);

    // 根据对象的地址，返回对应的span
    Span* MapObjectToSpan(void* obj);

    // 管理cc归还的span
    void ReleaseSpanToPageCache(Span* span);

    // 根据span的地址，返回对应的页号
    PageID MapSpanToPageID(Span* span);

    // ========== 动态扩容相关 ==========
    // 获取当前已分配的页数
    size_t GetTotalPages() const { return _totalPages.load(std::memory_order_relaxed); }

    // 获取当前空闲页数
    size_t GetFreePages() const;

    // 获取分配峰值
    size_t GetPeakPages() const { return _peakPages.load(std::memory_order_relaxed); }

    // 记录分配（用于统计）
    void RecordAllocation(size_t pages);

    // 记录释放（用于统计）
    void RecordRelease(size_t pages);

    // 尝试回收空闲span（基于水位线策略）
    bool TryReclaimIdleSpans();

    // 获取内存使用统计
    void GetMemoryStats(size_t& totalAllocated, size_t& totalFree, size_t& peakUsage) const;

    // ========== 大对象分配（超过 PAGE_NUM 页） ==========
    // 直接分配大块内存（不进入缓存）
    void* AllocateLarge(size_t npage);

    // 释放大块内存
    void DeallocateLarge(void* ptr, size_t npage);

private:
    SpanList _spanLists[PAGE_NUM];

    // 页号到页面span地址的映射，方便查找
    // std::unordered_map<PageID, Span*> _idSpanMap;
    // 使用三层基数树优化，映射48位页号（256TB地址空间）
    TCMalloc_PageMap3<48 - PAGE_SHIFT> _idSpanMap;

    // 创建span的对象池
    ObjectPool<Span> _spanPool;

    // ========== 统计信息 ==========
    std::atomic<size_t> _totalPages{ 0 };          // 当前已分配的页数
    std::atomic<size_t> _peakPages{ 0 };           // 分配峰值
    std::atomic<size_t> _totalAllocatedPages{ 0 };  // 累计分配的页数（用于统计）
    std::atomic<size_t> _totalFreedPages{ 0 };     // 累计释放的页数（用于统计）

    // ========== 大对象分配记录（用于释放） ==========
    struct LargeAlloc {
        void* ptr;
        size_t npage;
    };
    std::unordered_map<void*, size_t> _largeAllocs;
    std::mutex _largeAllocMtx;

    // 只有一个页面缓存，写为单例（饿汉）
    PageCache() {
        // 确保所有 SpanList 都正确初始化
        for (int i = 0; i < PAGE_NUM; ++i) {
            _spanLists[i]; // 触发默认构造
        }
        // 初始化紧急内存池
        EmergencyPool::Instance().Init();
    }
    PageCache(const PageCache& pc) = delete;
    PageCache& operator = (const PageCache& pc) = delete;

    static PageCache _sInst; // 单例对象
};
