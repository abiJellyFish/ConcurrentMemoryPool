#pragma once

#include "Common.h"
#include <iostream>
#include <vector>

using std::cout;
using std::endl;
using std::vector;

class ThreadCache{
public:
	void* Allocate(size_t size); // 线程申请空间，大小为size

	void Deallocate(void* obj, size_t size); // 回收线程使用完的空间，大小size

	// 线程缓存空间不足时，向中央缓存申请
	void* FetchFromCentralCache(size_t index, size_t alignSize);

	// 线程缓存中空闲链表的内存块数量超过MaxSize时，向cc归还空间
	void ListTooLong(FreeList& list, size_t size);

private:
	FreeList _freeLists[FREE_LIST_NUM]; // 哈希表，表中每个桶表示一个空闲链表

};

// TLS全局对象指针，让每个线程都有独立的全局对象
// extern声明，由ThreadCache.cpp定义
extern thread_local ThreadCache* pTLSThreadCache;