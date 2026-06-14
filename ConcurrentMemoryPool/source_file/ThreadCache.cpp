#include "ThreadCache.h"
#include "CentralCache.h"
#include "PageCache.h"
#include <random>
#include <algorithm>
#include <thread>

// 定义 thread_local 变量
thread_local ThreadCache* pTLSThreadCache = nullptr;

// 用于生成随机数（用于窃取时选择受害者线程）
static thread_local std::mt19937_64 g_rng(std::hash<std::thread::id>()(std::this_thread::get_id()));

// 线程申请size大小的空间
// 由于类在外部头文件里定义，加ThreadCache::（作用域解析符号）表示是成员函数
void* ThreadCache::Allocate(size_t size) {
	assert(size <= MAX_BYTES);

	size_t alignSize = SizeClass::RoundUp(size);
	size_t index = SizeClass::Index(size);

	// 调试模式：设置魔数
#ifdef _DEBUG_MODE
	void* obj = nullptr;
	if (!_freeLists[index].Empty()) {
		obj = _freeLists[index].Pop();
		SetDebugMagic(obj, alignSize);
		return obj;
	}

	// 本地 freelist 为空，尝试从窃取队列获取
	obj = FetchFromStealQueue(index, alignSize);
	if (obj != nullptr) {
		SetDebugMagic(obj, alignSize);
		return obj;
	}

	// 尝试窃取
	obj = StealFromOtherThread(index, alignSize);
	if (obj != nullptr) {
		SetDebugMagic(obj, alignSize);
		return obj;
	}

	// 向中央缓存申请
	obj = FetchFromCentralCache(index, alignSize);
	if (obj != nullptr) {
		SetDebugMagic(obj, alignSize);
	}
	return obj;
#else
	if (!_freeLists[index].Empty()) {
		return _freeLists[index].Pop();
	}
	else {
		// 本地 freelist 为空，尝试从窃取队列获取
		void* obj = FetchFromStealQueue(index, alignSize);
		if (obj != nullptr) {
			return obj;
		}

		// 尝试窃取
		obj = StealFromOtherThread(index, alignSize);
		if (obj != nullptr) {
			return obj;
		}

		// 向中央缓存申请
		return FetchFromCentralCache(index, alignSize);
	}
#endif
}

// 从线程中回收空间，内存后续会对齐
void ThreadCache::Deallocate(void* obj, size_t size) {
	assert(obj);
	assert(size <= MAX_BYTES);

#ifdef _DEBUG_MODE
	// 调试模式：检查魔数
	if (!CheckDebugMagic(obj, size)) {
		std::cerr << "[DEBUG] Memory corruption detected in Deallocate!" << std::endl;
		std::abort();
	}
#endif

	// 在哈希表中找到size对应的空闲链表，并回收空间
	size_t index = SizeClass::Index(size);
	_freeLists[index].Push(obj);

	// 当线程中某个桶内存块数量超过MaxSize时，归还MaxSize个内存块给cc
	if (_freeLists[index].Size() >= _freeLists[index].MaxSize()) {
		ListTooLong(_freeLists[index], size);
	}

	// 定期检查是否需要回收
	TryReclaim();
}

// 尝试从本地窃取队列获取对象
void* ThreadCache::FetchFromStealQueue(size_t index, size_t alignSize) {
	if (_stealQueue.Empty()) {
		return nullptr;
	}

	// 从窃取队列批量获取
	void* batch = _stealQueue.PopBatch(STEAL_BATCH_SIZE);
	if (batch == nullptr) {
		return nullptr;
	}

	// 找到 batch 的尾部
	void* tail = batch;
	size_t count = 1;
	while (ObjNext(tail) != nullptr) {
		tail = ObjNext(tail);
		++count;
	}

	// 将对象接入本地 freelist（除了第一个）
	if (count > 1) {
		_freeLists[index].PushRange(ObjNext(batch), tail, count - 1);
	}

	// 返回第一个对象
	return batch;
}

// 尝试从其他线程窃取
void* ThreadCache::StealFromOtherThread(size_t index, size_t alignSize) {
	// 只有在本地 freelist 非常空虚时才触发窃取
	if (_freeLists[index].Size() >= STEAL_THRESHOLD) {
		return nullptr;
	}

	// 获取所有活跃的 ThreadCache 实例
	// 注意：这里简化处理，实际实现可能需要维护一个全局线程列表
	ThreadCache* candidates[32];
	size_t candidateCount = 0;

	// 遍历尝试窃取
	for (size_t attempt = 0; attempt < MAX_STEAL_ATTEMPTS; ++attempt) {
		// 随机选择候选线程
		// 这里需要从 CentralCache 或全局注册表获取其他线程的 ThreadCache
		// 简化实现：通过多次尝试从不同线程的窃取队列获取

		// 由于我们没有全局线程列表，这里通过 CentralCache 间接获取
		// 实际实现中应该维护一个线程注册机制
		Span* span = nullptr;
		void* stolen = CentralCache::GetInstance()->StealFromThread(
			index, alignSize, candidates, candidateCount, span);

		if (stolen != nullptr && span != nullptr) {
			// 将窃取的对象接入本地 freelist
			void* tail = stolen;
			size_t count = 1;
			while (ObjNext(tail) != nullptr) {
				tail = ObjNext(tail);
				++count;
			}

			if (count > 1) {
				_freeLists[index].PushRange(ObjNext(stolen), tail, count - 1);
			}

			return stolen;
		}
	}

	return nullptr;
}

// 定期回收检查
void ThreadCache::TryReclaim() {
	// 如果分配失败次数达到阈值，触发紧急回收
	if (NeedEmergencyReclaim()) {
		// 强制归还一半的空闲对象
		for (size_t i = 0; i < FREE_LIST_NUM; ++i) {
			if (_freeLists[i].Size() > RECLAIM_THRESHOLD / 2) {
				void* start = nullptr;
				void* end = nullptr;
				size_t toReclaim = _freeLists[i].Size() / 2;
				if (toReclaim > 0) {
					_freeLists[i].PopRange(start, end, toReclaim);
					// 计算实际大小（需要从 index 反推）
					size_t size = SizeClass::RoundUp(
						(i < 16) ? (i + 1) * 8 :
						(i < 72) ? 128 + (i - 16) * 16 :
						(i < 128) ? 1024 + (i - 72) * 128 :
						(i < 184) ? 8 * 1024 + (i - 128) * 1024 :
						64 * 1024 + (i - 184) * 8 * 1024
					);
					CentralCache::GetInstance()->ReleaseListToSpans(start, size);
				}
			}
		}
		ResetAllocFailures();
	}

	// 正常回收：检查窃取队列是否过大
	size_t stealSize = _stealQueue.Size();
	if (stealSize > MAX_STEAL_QUEUE_SIZE) {
		// 归还一半到 CentralCache
		void* tail = nullptr;
		size_t count = 0;
		void* batch = _stealQueue.StealHalf(tail, count);
		if (batch != nullptr) {
			// 这里需要知道对象大小，但窃取队列存储的是通用对象
			// 简化处理：先不归还，等分配时再处理
		}
	}
}

// 获取当前空闲对象总数
size_t ThreadCache::GetTotalFreeCount() const {
	size_t total = 0;
	for (size_t i = 0; i < FREE_LIST_NUM; ++i) {
		total += _freeLists[i].Size();
	}
	total += _stealQueue.Size();
	return total;
}

// 线程缓存空间不足时，向中央缓存申请空间
void* ThreadCache::FetchFromCentralCache(size_t index, size_t alignSize) {
	// 慢开始反馈调节算法，也就是申请次数越多，分配的空间越大

	size_t maxSizeTc = _freeLists[index].MaxSize();
	size_t maxSizeCc = SizeClass::NumMoveSize(alignSize);
	size_t batchNum = (maxSizeTc < maxSizeCc) ? maxSizeTc : maxSizeCc;

	if (batchNum == _freeLists[index].MaxSize()) {
		_freeLists[index].MaxSize()++;
		// 下次申请多提供一块空间
	}

	void* start = nullptr; void* end = nullptr;
	size_t actulNum = CentralCache::GetInstance()->FetchRangeObj(start, end, batchNum, alignSize);

	// 分配失败处理
	if (actulNum == 0) {
		RecordAllocFailure();

		// 尝试从紧急内存池获取
		if (ENABLE_EMERGENCY_POOL) {
			void* emergency = EmergencyPool::Instance().Alloc(alignSize);
			if (emergency != nullptr) {
				return emergency;
			}
		}

		// 尝试强制回收后重试
		CentralCache::GetInstance()->ForceReclaimAll();
		actulNum = CentralCache::GetInstance()->FetchRangeObj(start, end, batchNum, alignSize);

		if (actulNum == 0) {
			return nullptr; // 分配失败
		}
	}

	assert(actulNum >= 1);
	// 如果只需要一块空间，直接将空间提供给线程，无需接入 _freeLists
	if (actulNum == 1) {
		assert(start == end);
		return start;
	}
	else {
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
