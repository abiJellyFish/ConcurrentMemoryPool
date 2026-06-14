#pragma once 
#include "Common.h"

// 前向声明
class ThreadCache;

class CentralCache{
  public:
    static CentralCache* GetInstance(){
      return &_sInst; // 返回单例的地址
    }

    size_t FetchRangeObj(void*& start, void*& end, size_t batchNum, size_t size);
    // 中央缓存从span链表中提供空间给线程缓存
    // 具体是从index对应的哈希桶中取batchNum * alignSize大小的空间
    
    // cc向pc申请新的非空span
    Span * GetOneSpan(SpanList& list, size_t size);

    // cc 接收线程缓存归还的空间
    void ReleaseListToSpans(void* start, size_t size);

    // ========== 任务窃取相关 ==========
    // 从其他线程的窃取队列窃取对象
    void* StealFromThread(size_t index, size_t alignSize,
                          ThreadCache* candidates[], size_t& candidateCount,
                          Span*& outSpan);

    // 注册线程的窃取队列到全局列表（用于窃取）
    void RegisterThreadCache(ThreadCache* tc);

    // 注销线程的窃取队列
    void UnregisterThreadCache(ThreadCache* tc);

    // ========== 紧急回收相关 ==========
    // 强制回收所有线程缓存的空闲对象
    void ForceReclaimAll();

    // 获取当前注册的线程数量
    size_t GetThreadCount() const;

  private:
    CentralCache() {
      // 初始化线程数组
      for (size_t i = 0; i < MAX_THREADS; ++i) {
        _threadCaches[i] = nullptr;
      }
      _threadCount.store(0, std::memory_order_relaxed);
    } // 私有构造函数，防止外部创建，确保单例
    CentralCache(const CentralCache& copy) = delete; // 删除拷贝构造 
    CentralCache& operator = (const CentralCache& copy) = delete; // 删除拷贝赋值
    
    // 单个哈希桶为Span链表
    SpanList _spanLists[FREE_LIST_NUM];

    // ========== 线程注册表（用于窃取） ==========
    static const size_t MAX_THREADS = 64;
    ThreadCache* _threadCaches[MAX_THREADS];
    std::atomic<size_t> _threadCount;
    std::mutex _threadListMtx; // 保护线程列表的锁

    // 单例模式（饿汉）确保所有线程缓存都访问一个中央缓存
    // 主函数运行前饿汉就自动创建好实例，避免并发问题
    static CentralCache _sInst;
};
