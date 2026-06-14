#pragma once

#include "Common.h"
#include <iostream>
#include <vector>
#include <atomic>

using std::cout;
using std::endl;
using std::vector;

class ThreadCache {
public:
	void* Allocate(size_t size); // 线程申请空间，大小为size

	void Deallocate(void* obj, size_t size); // 回收线程使用完的空间，大小size

	// 线程缓存空间不足时，向中央缓存申请
	void* FetchFromCentralCache(size_t index, size_t alignSize);

	// 线程缓存中空闲链表的内存块数量超过MaxSize时，向cc归还空间
	void ListTooLong(FreeList& list, size_t size);

	// ========== 任务窃取相关 ==========
	// 获取窃取队列（供其他线程访问）
	StealQueue& GetStealQueue() { return _stealQueue; }

	// 尝试从本地窃取队列获取对象
	void* FetchFromStealQueue(size_t index, size_t alignSize);

	// 尝试从其他线程窃取
	void* StealFromOtherThread(size_t index, size_t alignSize);

	// 获取线程ID
	size_t GetThreadId() const { return _threadId; }

	// 设置线程ID
	void SetThreadId(size_t id) { _threadId = id; }

	// 记录分配失败次数
	void RecordAllocFailure() { _allocFailures.fetch_add(1, std::memory_order_relaxed); }

	// 获取分配失败次数
	size_t GetAllocFailures() const { return _allocFailures.load(std::memory_order_relaxed); }

	// 重置分配失败计数
	void ResetAllocFailures() { _allocFailures.store(0, std::memory_order_relaxed); }

	// 检查是否需要紧急回收
	bool NeedEmergencyReclaim() const {
		return _allocFailures.load(std::memory_order_relaxed) >= MEM_PRESSURE_THRESHOLD;
	}

	// 定期回收检查（被调用时触发）
	void TryReclaim();

	// 获取当前空闲对象总数
	size_t GetTotalFreeCount() const;

private:
	FreeList _freeLists[FREE_LIST_NUM]; // 哈希表，表中每个桶表示一个空闲链表

	// ========== 窃取队列 ==========
	StealQueue _stealQueue; // 本线程的窃取队列，存放其他线程归还的对象

	// ========== 线程标识 ==========
	size_t _threadId = 0; // 当前线程的唯一标识

	// ========== 统计信息 ==========
	std::atomic<size_t> _allocFailures{0}; // 分配失败次数，用于触发紧急回收
};

// TLS全局对象指针，让每个线程都有独立的全局对象
// extern声明，由ThreadCache.cpp定义
extern thread_local ThreadCache* pTLSThreadCache;
