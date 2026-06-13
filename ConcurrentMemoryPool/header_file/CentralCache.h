#pragma once 
#include "Common.h"

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

  private:
    CentralCache(){} // 私有构造函数，防止外部创建，确保单例
    CentralCache(const CentralCache& copy) = delete; // 删除拷贝构造 
    CentralCache& operator = (const CentralCache& copy) = delete; // 删除拷贝赋值
    
    // 单个哈希桶为Span链表
    SpanList _spanLists[FREE_LIST_NUM];
    // 单例模式（饿汉）确保所有线程缓存都访问一个中央缓存
    // 主函数运行前饿汉就自动创建好实例，避免并发问题
    static CentralCache _sInst;
};