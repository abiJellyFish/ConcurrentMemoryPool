#include "PageCache.h"

PageCache PageCache::_sInst;

// 根据cc的需求，pc从span链表数组中取k页的span
Span* PageCache::NewSpan(size_t k){
  // assert(k > 0 && k < PAGE_NUM);
  assert(k > 0);

  if (k > PAGE_NUM - 1){
    void* ptr = SystemAlloc(k); 
    if (ptr == nullptr) {
      return nullptr;
    }
    
    //Span* span = new Span; 
    Span* span = _spanPool.New();
       
    span->_pageID = ((PageID)ptr >> PAGE_SHIFT);
    span->_n = k; 
    span->_objSize = 0;
    span->_freeList = nullptr;
    span->use_count = 0;
    span->_prev = nullptr;
    span->_next = nullptr;
    span->_isUse = false;

    // 加锁保护基数树操作
    GetInstance()->_pageMtx.lock();
    
    // 确保内存已分配
    if (!_idSpanMap.Ensure(span->_pageID, k)) {
      // Ensure 失败，释放 span 并返回 nullptr
      _spanPool.Delete(span);
      SystemFree(ptr);
      GetInstance()->_pageMtx.unlock();
      return nullptr;
    }
    // 映射所有页面
    for(PageID i = 0; i < k; ++i){
      _idSpanMap.set(span->_pageID + i, span);
    }
    
    GetInstance()->_pageMtx.unlock();

    return span;
  }
  
  while(true){
    GetInstance()->_pageMtx.lock(); // 对PageCache整体加互斥锁

    // 1. 如果k对应的哈希桶有span
    if(!_spanLists[k].Empty()){
      Span* span = _spanLists[k].PopFront();

      // 记录分配的span页号到span地址的映射
      if (!_idSpanMap.Ensure(span->_pageID, span->_n)) {
        // Ensure 失败，将 span 放回链表并返回 nullptr
        _spanLists[k].PushFront(span);
        GetInstance()->_pageMtx.unlock();
        return nullptr;
      }
      for(PageID i = 0; i < span->_n; ++i){
        _idSpanMap.set(span->_pageID + i, span);
      }

      GetInstance()->_pageMtx.unlock();
      return span;
    }

    // 2. 如果k对应的哈希桶没有span，但之后的哈希桶有
    for(int i = k + 1; i < PAGE_NUM; ++i){
      if(!_spanLists[i].Empty()){
        Span* nSpan = _spanLists[i].PopFront();
        
        // 先记录切分前的页面范围，用于后续移除映射
        PageID oldPageID = nSpan->_pageID;
        PageID newPageID = nSpan->_pageID + k;
        size_t newN = nSpan->_n - k;
        
        // 移除 k 页范围内的旧映射，避免其他线程访问到错误的 span
        for(PageID j = 0; j < k; ++j){
          _idSpanMap.erase(oldPageID + j);
        }

        // 更新 nSpan 的状态
        nSpan->_pageID = newPageID;
        nSpan->_n = newN;
        
        _spanLists[nSpan->_n].PushFront(nSpan);

        // 映射 nSpan 的所有页面
        if (!_idSpanMap.Ensure(nSpan->_pageID, nSpan->_n)) {
          // Ensure 失败，返回 nullptr
          GetInstance()->_pageMtx.unlock();
          return nullptr;
        }
        for(PageID j = 0; j < nSpan->_n; ++j){
          _idSpanMap.set(nSpan->_pageID + j, nSpan);
        }

        // 先解锁，避免与_spanPool的锁嵌套导致死锁
        GetInstance()->_pageMtx.unlock();
        
        // 在锁外创建新的 kSpan
        Span* kSpan = _spanPool.New();

        kSpan->_pageID = oldPageID;
        kSpan->_n = k;
        kSpan->_objSize = 0;
        kSpan->_freeList = nullptr;
        kSpan->use_count = 0;
        kSpan->_prev = nullptr;
        kSpan->_next = nullptr;
        kSpan->_isUse = false;

        // 重新加锁设置 kSpan 的映射
        GetInstance()->_pageMtx.lock();

        // 记录KSpan页号到span地址的映射
        if (!_idSpanMap.Ensure(kSpan->_pageID, kSpan->_n)) {
          // Ensure 失败，释放 kSpan 并返回 nullptr
          _spanPool.Delete(kSpan);
          GetInstance()->_pageMtx.unlock();
          return nullptr;
        }
        for(PageID j = 0; j < kSpan->_n; ++j){
          _idSpanMap.set(kSpan->_pageID + j, kSpan);
        }

        GetInstance()->_pageMtx.unlock();
        return kSpan;
      }
    }

    // 3. 如果都找不到，先解锁再向系统申请128页的span
    GetInstance()->_pageMtx.unlock();
    
    void* ptr = SystemAlloc(PAGE_NUM - 1);
    if (ptr == nullptr) {
      // 系统内存分配失败，继续循环重试
      continue;
    }
    
    // Span* bigSpan = new Span;
    Span* bigSpan = _spanPool.New();

    // 保证申请的内存对齐
    bigSpan->_pageID = ((PageID)ptr) >> PAGE_SHIFT;
    bigSpan->_n = PAGE_NUM - 1;
    bigSpan->_objSize = 0;
    bigSpan->_freeList = nullptr;
    bigSpan->use_count = 0;
    bigSpan->_prev = nullptr;
    bigSpan->_next = nullptr;
    bigSpan->_isUse = false;
    
    GetInstance()->_pageMtx.lock();
    
    // 设置基数树映射
    if (!_idSpanMap.Ensure(bigSpan->_pageID, bigSpan->_n)) {
      // Ensure 失败，释放资源
      _spanPool.Delete(bigSpan);
      SystemFree(ptr);
      GetInstance()->_pageMtx.unlock();
      continue;
    }
    for(PageID j = 0; j < bigSpan->_n; ++j){
      _idSpanMap.set(bigSpan->_pageID + j, bigSpan);
    }
    
    _spanLists[PAGE_NUM - 1].PushFront(bigSpan);
    GetInstance()->_pageMtx.unlock();
    
    // 循环重新尝试分配k页
  }
}

// 根据对象的地址，返回对应的span
Span* PageCache::MapObjectToSpan(void* obj){
  PageID id = ((PageID)obj) >> PAGE_SHIFT;

  // map不是线程安全（扩容），查询需要加锁
  GetInstance()->_pageMtx.lock();
  Span* span = (Span*)_idSpanMap.get(id);
  GetInstance()->_pageMtx.unlock();

  if(span != nullptr){
    return span;
  }else{
    assert(false);
    return nullptr;
  }
}

// 管理cc归还的span
void PageCache::ReleaseSpanToPageCache(Span* span){
  _pageMtx.lock();

  // 通过span判断释放的空间页数是否大于128页，如果大于128页就归还给os
  if (span->_n > PAGE_NUM - 1){
      void* ptr = (void*)(span->_pageID << PAGE_SHIFT); // 获取到要释放的地址
      SystemFree(ptr); // 直接调用系统接口释放空间
      
      // 移除所有页面的映射
      for(PageID i = 0; i < span->_n; ++i){
        _idSpanMap.erase(span->_pageID + i);
      }
      
      _pageMtx.unlock(); // 先解锁，避免与_spanPool的锁嵌套
      
      // delete span;
      _spanPool.Delete(span); // 定长内存池释放span

      return;
  }
  // 后面是小于128页的情况

  // 向左合并
  Span* leftSpanToDelete = nullptr;
  while(1){
    // 获取左边相邻页面，并映射对应span
    PageID leftID = span->_pageID - 1;
    Span* leftSpan = (Span*)_idSpanMap.get(leftID);

    // 如果左边没有相邻页面，则退出
    if(leftSpan == nullptr){
      break;
    }

    // 如果左边相邻页面在cc中，则退出
    if(leftSpan->_isUse == true){
      break;
    }

    // 如果合并后超过128页，则退出
    if(leftSpan->_n + span->_n > PAGE_NUM - 1){
      break;
    }

    // 合并当前span和相邻span
    span->_pageID = leftSpan->_pageID;
    span->_n += leftSpan->_n;
    
    _spanLists[leftSpan->_n].Erase(leftSpan);
    // 从_idSpanMap中移除被合并的span的所有页面映射
    for(PageID i = 0; i < leftSpan->_n; ++i){
      _idSpanMap.erase(leftSpan->_pageID + i);
    }
    
    // 先保存待删除的span，稍后在锁外删除
    if(leftSpanToDelete == nullptr){
      leftSpanToDelete = leftSpan;
    }
  }

  // 向右合并
  Span* rightSpanToDelete = nullptr;
  while(1){
    // 获取右边相邻页面，并映射对应span
    PageID rightID = span->_pageID + span->_n;
    Span* rightSpan = (Span*)_idSpanMap.get(rightID);

    // 如果右边没有相邻页面，则退出
    if(rightSpan == nullptr){
      break;
    }

    // 如果右边相邻页面在cc中，则退出
    if(rightSpan->_isUse == true){
      break;
    }

    // 如果合并后超过128页，则退出
    if(rightSpan->_n + span->_n > PAGE_NUM - 1){
      break;
    }

    // 合并当前span和相邻span，右边相邻的span会直接拼接在当前span后面
    span->_n += rightSpan->_n;
    
    _spanLists[rightSpan->_n].Erase(rightSpan);
    // 从_idSpanMap中移除被合并的span的所有页面映射
    for(PageID i = 0; i < rightSpan->_n; ++i){
      _idSpanMap.erase(rightSpan->_pageID + i);
    }
    
    // 先保存待删除的span，稍后在锁外删除
    if(rightSpanToDelete == nullptr){
      rightSpanToDelete = rightSpan;
    }
  }

  // 合并完成后，将当前span放回对应的哈希桶
  _spanLists[span->_n].PushFront(span);
  span->_isUse = false;
  
  // 映射当前span的所有页面
  if (!_idSpanMap.Ensure(span->_pageID, span->_n)) {
    // Ensure 失败，解锁并返回
    _pageMtx.unlock();
    return;
  }
  for(PageID i = 0; i < span->_n; ++i){
    _idSpanMap.set(span->_pageID + i, span);
  }
  
  _pageMtx.unlock(); // 解锁
  
  // 在锁外删除span，避免死锁
  if(leftSpanToDelete != nullptr){
    _spanPool.Delete(leftSpanToDelete); // 定长内存池释放span
  }
  if(rightSpanToDelete != nullptr){
    _spanPool.Delete(rightSpanToDelete);
  }
}