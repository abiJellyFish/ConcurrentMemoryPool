#include "ThreadCache.h"
#include "CentralCache.h"
#include "PageCache.h"

// 定义 thread_local 变量
thread_local ThreadCache* pTLSThreadCache = nullptr;

// 线程申请size大小的空间
// 由于类在外部头文件里定义，加ThreadCache::（作用域解析符号）表示是成员函数
void* ThreadCache::Allocate(size_t size){
  assert(size <= MAX_BYTES);

  size_t alignSize = SizeClass::RoundUp(size);
  size_t index = SizeClass::Index(size);

  if(!_freeLists[index].Empty()){
    return _freeLists[index].Pop();
  }
  else{
    // 如果空闲链表为空，向中央缓存申请空间
    return FetchFromCentralCache(index, alignSize);
  }
}

// 从线程中回收空间，内存后续会对齐
void ThreadCache::Deallocate(void* obj, size_t size){
  assert(obj);
  assert(size <= MAX_BYTES);

  // 在哈希表中找到size对应的空闲链表，并回收空间
  size_t index = SizeClass::Index(size);
  _freeLists[index].Push(obj);

  // 当线程中某个桶内存块数量超过MaxSize时，归还MaxSize个内存块给cc
  if(_freeLists[index].Size() >= _freeLists[index].MaxSize()){
    ListTooLong(_freeLists[index], size);
  }
}

// 线程缓存空间不足时，向中央缓存申请空间
void* ThreadCache::FetchFromCentralCache(size_t index, size_t alignSize){
  // 慢开始反馈调节算法，也就是申请次数越多，分配的空间越大

  // windows平台头文件内置min，会冲突，使用平台自带的
  /*#ifdef WIN32
    size_t batchNum = std::min(_freeLists[index].MaxSize(), SizeClass::NumMoveSize(alignSize));
  #else
    size_t batchNum = std::min(_freeLists[index].MaxSize(), SizeClass::NumMoveSize(alignSize));
    // MaxSize()是空闲链表能申请的最大空间，NumMoveSize（）是tc向cc申请的最大空间 
    // 也就是如果线程申请的空间超过 NumMoveSize ,就提供 NumMoveSize
  #endif
  */

  size_t maxSizeTc = _freeLists[index].MaxSize();
  size_t maxSizeCc = SizeClass::NumMoveSize(alignSize);
  size_t batchNum = (maxSizeTc < maxSizeCc) ? maxSizeTc : maxSizeCc;

  if(batchNum == _freeLists[index].MaxSize()){
    _freeLists[index].MaxSize()++;
    // 下次申请多提供一块空间
  }

  void* start = nullptr; void* end = nullptr;
  size_t actulNum = CentralCache::GetInstance()->FetchRangeObj(start, end, batchNum, alignSize);
  // 返回值为获取到的内存块数量，之后存放到空闲链表中
  
  assert(actulNum >= 1);
  // 如果只需要一块空间，直接将空间提供给线程，无需接入 _freeLists
  if(actulNum == 1){
    assert(start == end);
    return start;
  }
  else{
    _freeLists[index].PushRange(ObjNext(start), end, actulNum - 1);
    // actulNum - 1因为第一块内存直接分配给线程
    return start;
  }
}

// 空闲链表大小超过MaxSize时，归还MaxSize个内存块给cc
void ThreadCache::ListTooLong(FreeList& list, size_t size){
  void* start = nullptr; void* end = nullptr;
  list.PopRange(start, end, list.MaxSize());

  // end 指向空，所以next为空的节点就是end
  CentralCache::GetInstance()->ReleaseListToSpans(start, size);
}