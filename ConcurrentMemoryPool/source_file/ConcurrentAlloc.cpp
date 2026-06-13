#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>
#include "ConcurrentAlloc.h"

// 线程申请空间的接口
void* ConcurrentAlloc(size_t size){
  //std::cout << std::this_thread::get_id() << " " << pTLSThreadCache << std::endl;

  // 如果申请空间超过256KB，向页面缓存申请空间
  if (size > MAX_BYTES){
      // 按照页大小对齐
      size_t alignSize = SizeClass::_RoundUp(size, 1 << PAGE_SHIFT); 
      size_t k = alignSize >> PAGE_SHIFT; // 对齐之后需要多少页

      // 直接调用 NewSpan，它内部会自己加锁
      // 注意：NewSpan 内部会加 _pageMtx 锁，不能在这里提前加锁，否则会导致死锁
      Span* span = PageCache::GetInstance()->NewSpan(k);
      if (span == nullptr) {
          return nullptr; // 内存分配失败
      }

      // 设置 _objSize 为 size，以便 ConcurrentFree 能正确识别大对象
      span->_objSize = size;
      
      void* ptr = (void*)((uintptr_t)span->_pageID << PAGE_SHIFT);
      return ptr;
  }
  else{
    // TLS对象由每个线程独立拥有，不会有线程安全问题
    if(pTLSThreadCache == nullptr){
      // 直接用 new 创建 ThreadCache，避免 ObjectPool 可能的问题
      pTLSThreadCache = new ThreadCache();
    }

    // this_thread::get_id() 获取当前线程编号
    // cout << std::this_thread::get_id() << " " << pTLSThreadCache << endl; // 注释掉调试输出
    return pTLSThreadCache->Allocate(size);
  }
}

// 线程回收空间的接口
void ConcurrentFree(void* obj){
  assert(obj);

  // 通过obj找到对应的span（通过映射）
  Span* span = PageCache::GetInstance()->MapObjectToSpan(obj);
  if (span == nullptr) {
    // 找不到对应的span，可能是内存分配失败导致的
    return;
  }
  size_t size = span->_objSize; // 通过映射的span获取obj指向空间的大小

  // 如果释放空间超过256KB，向页面缓存释放空间
  if (size > MAX_BYTES){
    PageCache::GetInstance()->ReleaseSpanToPageCache(span);
  }
  else{
    // 确保pTLSThreadCache不为空
    if(pTLSThreadCache == nullptr){
      pTLSThreadCache = new ThreadCache();
    }
    pTLSThreadCache->Deallocate(obj, size);
  }
  
  return;
}