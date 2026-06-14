#include "CentralCache.h"
#include "PageCache.h"
#include "ThreadCache.h"
#include <random>
#include <algorithm>
#include <vector>

CentralCache CentralCache::_sInst; 
// 单例对象。静态成员对象在类外声明才会分配内存
// 头文件会被多个程序调用，要在外部程序单独创建对象

// cc从span链表中提供空间给tc，size是每块空间的大小，batchNum是内存块数量
size_t CentralCache::FetchRangeObj(void*& start, void*& end, size_t batchNum, size_t size){
  size_t index = SizeClass::Index(size);
  // 获取size对应的span链表位置
  // 如果index位置的span存在且不为空，则直接获取；
  // 如果span存在且为空，或者不存在span，则cc向pc申请新span
  
  // 只有一个cc，可能有多个线程向cc的同一个span链表申请空间，需要加锁
  _spanLists[index]._mtx.lock();

  Span* span = GetOneSpan(_spanLists[index], size);
  if (span == nullptr || span->_freeList == nullptr) {
    _spanLists[index]._mtx.unlock();
    return 0;
  }
  assert(span); assert(span->_freeList);

  // end向后移动目标数量，将start到end的范围分配给tc
  end = span->_freeList; start = end;
  size_t actualNum = 1;

  size_t i = 0;
  while (i < batchNum - 1 && ObjNext(end) != nullptr) {
    end = ObjNext(end);
    ++actualNum;
    ++i;
  }

  // 内存范围从span链表中拆出
  span->_freeList = ObjNext(end);
  ObjNext(end) = nullptr;

  // 记录分配的内存块数量，用于判断何时可以将span归还给pc
  span->use_count += actualNum;

  _spanLists[index]._mtx.unlock();

  return actualNum;
}

// cc向pc申请新的非空span
Span * CentralCache::GetOneSpan(SpanList& list, size_t size){
  
  // 查找spanlist中的非空span，如果没有，则申请新span
  Span* it = list.Begin();
  while(it != list.End()){
    if(it->_freeList != nullptr){ 
      // 如果找到非空span，直接返回（锁由调用者在FetchRangeObj中释放）
      return it;
    }
    else{ 
      it = it->_next;
    }
  }

  // 如果没有找到非空span，先释放桶锁
  list._mtx.unlock();
  
  // 解除桶锁，让其他向这个cc桶操作的线程拿到锁
  // 如果没有找到非空span，则从pc获取一个新span
  // 向pc获取一个新span
  size_t k = SizeClass::NumMovePage(size);

  // PageCache::GetInstance()->_pageMtx.lock();
  Span* span = PageCache::GetInstance()->NewSpan(k);
  if (span == nullptr) {
    // 内存分配失败，重新加锁并返回nullptr
    list._mtx.lock();
    return nullptr;
  }
  
  span->_isUse = true;
  span->_objSize = size; // 记录span切分的内存块大小
  //PageCache::GetInstance()->_pageMtx.unlock();

  char* start = (char*)(span->_pageID << PAGE_SHIFT);
  // 计算实际可用的结束地址（确保不越界）
  char* end = (char*)((uintptr_t)start + (uintptr_t)span->_n * (1UL << PAGE_SHIFT));

  // 切分span，用tail连接切分的内存块
  span->_freeList = start;

  void* tail = start;
  // 确保 size 对齐
  size_t alignSize = SizeClass::RoundUp(size);
  start += alignSize;

  // 防止指针越界，只切分在有效范围内的内存
  while(start < end - alignSize){
    ObjNext(tail) = start;
    start += alignSize;
    tail = ObjNext(tail);
  }
  ObjNext(tail) = nullptr;

  list._mtx.lock(); // span挂载到对应的桶之前加锁
  // 把span连接到cc中哈希桶对应的位置
  list.PushFront(span);
  return span;
}

// cc 接收线程缓存归还的空间
void CentralCache::ReleaseListToSpans(void* start, size_t size){
  size_t index = SizeClass::Index(size);
  
  _spanLists[index]._mtx.lock();

  // 遍历start到end的范围，将每个内存块挂载到对应的span链表中
  while(start != nullptr){
    void* next = ObjNext(start);
    Span* span = PageCache::GetInstance()->MapObjectToSpan(start);
    if (span == nullptr) {
      // 找不到对应的span，跳过这个内存块
      start = next;
      continue;
    }

    ObjNext(start) = span->_freeList;
    span->_freeList = start;

    span->use_count--;
    // 如果span归还了所有空间，则让cc将归还的span交给pc管理，合并更大的span
    if(span->use_count == 0){
      _spanLists[index].Erase(span);
      span->_freeList = nullptr;
      span->_next = nullptr;
      span->_prev = nullptr;

      _spanLists[index]._mtx.unlock();

      PageCache::GetInstance()->ReleaseSpanToPageCache(span);

      _spanLists[index]._mtx.lock();
    }

    start = next;
  }

  _spanLists[index]._mtx.unlock();
}

// ========== 任务窃取相关实现 ==========

// 注册线程的窃取队列到全局列表
void CentralCache::RegisterThreadCache(ThreadCache* tc) {
  if (tc == nullptr) return;

  std::lock_guard<std::mutex> lock(_threadListMtx);
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    if (_threadCaches[i] == nullptr) {
      _threadCaches[i] = tc;
      _threadCount.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  // 如果注册表满了，忽略（实际应该扩展）
}

// 注销线程的窃取队列
void CentralCache::UnregisterThreadCache(ThreadCache* tc) {
  if (tc == nullptr) return;

  std::lock_guard<std::mutex> lock(_threadListMtx);
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    if (_threadCaches[i] == tc) {
      _threadCaches[i] = nullptr;
      _threadCount.fetch_sub(1, std::memory_order_relaxed);
      return;
    }
  }
}

// 从其他线程的窃取队列窃取对象
void* CentralCache::StealFromThread(size_t index, size_t alignSize,
                                   ThreadCache* candidates[], size_t& candidateCount,
                                   Span*& outSpan) {
  // 获取当前线程的指针
  ThreadCache* currentTc = pTLSThreadCache;
  size_t currentThreadId = 0;
  if (currentTc != nullptr) {
    currentThreadId = currentTc->GetThreadId();
  }

  // 收集候选线程
  candidateCount = 0;
  std::vector<ThreadCache*> targets;

  {
    std::lock_guard<std::mutex> lock(_threadListMtx);
    for (size_t i = 0; i < MAX_THREADS; ++i) {
      ThreadCache* tc = _threadCaches[i];
      if (tc != nullptr && tc != currentTc && tc->GetThreadId() != currentThreadId) {
        targets.push_back(tc);
      }
    }
  }

  if (targets.empty()) {
    return nullptr;
  }

  // 随机选择并尝试窃取
  std::random_device rd;
  std::mt19937 gen(rd());
  std::shuffle(targets.begin(), targets.end(), gen);

  size_t maxAttempts = std::min(targets.size(), MAX_STEAL_ATTEMPTS);
  for (size_t i = 0; i < maxAttempts; ++i) {
    ThreadCache* victim = targets[i];
    if (victim == nullptr) continue;

    StealQueue& stealQueue = victim->GetStealQueue();
    if (stealQueue.Empty()) continue;

    // 尝试窃取一批对象
    void* tail = nullptr;
    size_t stolenCount = 0;
    void* batch = stealQueue.StealHalf(tail, stolenCount);

    if (batch != nullptr && candidateCount < 32) {
      candidates[candidateCount++] = victim;
      outSpan = nullptr; // 窃取队列的对象不属于任何特定 span
      return batch;
    }
  }

  return nullptr;
}

// ========== 紧急回收相关实现 ==========

// 强制回收所有线程缓存的空闲对象
void CentralCache::ForceReclaimAll() {
  std::lock_guard<std::mutex> lock(_threadListMtx);

  for (size_t i = 0; i < MAX_THREADS; ++i) {
    ThreadCache* tc = _threadCaches[i];
    if (tc != nullptr) {
      // 触发该线程的紧急回收
      // 注意：这需要线程配合，实际实现可能需要更好的机制
      // 这里简化处理，只重置失败计数
      tc->ResetAllocFailures();
    }
  }
}

// 获取当前注册的线程数量
size_t CentralCache::GetThreadCount() const {
  return _threadCount.load(std::memory_order_relaxed);
}
