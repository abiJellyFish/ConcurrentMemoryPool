#pragma once 

#include <mutex>
#include "Common.h"
#include "TCMalloc_PageMap3.h"

class PageCache{
  public:
    std::mutex _pageMtx;

    static PageCache* GetInstance(){
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

  private:
    SpanList _spanLists[PAGE_NUM];

    // 页号到页面span地址的映射，方便查找
    // std::unordered_map<PageID, Span*> _idSpanMap;
    // 使用三层基数树优化，映射48位页号（256TB地址空间）
    TCMalloc_PageMap3<48 - PAGE_SHIFT> _idSpanMap;

    // 创建span的对象池
    ObjectPool<Span> _spanPool;
    
    // 只有一个页面缓存，写为单例（饿汉）
    PageCache() {
      // 确保所有 SpanList 都正确初始化
      for (int i = 0; i < PAGE_NUM; ++i) {
        _spanLists[i]; // 触发默认构造
      }
    }
    PageCache(const PageCache& pc) = delete;
    PageCache& operator = (const PageCache& pc) = delete;

    static PageCache _sInst; // 单例对象
};