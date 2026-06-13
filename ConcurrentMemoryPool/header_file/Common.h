#pragma once

#include "ObjectPool.h"
#include <iostream>
#include <cassert>
#include <cstddef> // 标准类型，比如size_t
#include <mutex>
#include <stdexcept> // 抛出异常头文件
#include <unordered_map> 
#include <vector>

using std::cout;
using std::endl;

// 208个不同大小的空闲链表。用static const代替宏
static const size_t FREE_LIST_NUM = 208;
// 线程单次最多申请256KB的内存
static const size_t MAX_BYTES = 256 * 1024;
// pc中sapn管理的最大页数（从1开始直接映射）
static const size_t PAGE_NUM = 129;
// 每页有多少位（8KB）
static const size_t PAGE_SHIFT = 13;

typedef size_t PageID;

// pc从系统申请内存
#ifdef _WIN32
    // 内存管理API
    #include <Windows.h>
#else
    // Linux 平台,内存映射头文件
    #include <sys/mman.h>   // mmap,munmap 系统调用
    #include <unistd.h>     // sysconf,获取系统页大小
    #include <errno.h>      // 错误码
#endif

// 在堆上按页申请内存
inline static void* SystemAlloc(size_t kpage)
{
#ifdef _WIN32
    // nullptr：让系统自动选择虚拟地址
    // MEM_COMMIT | MEM_RESERVE：保留虚拟地址并提交物理内存
    // PAGE_READWRITE：内存保护属性（可读写）
    void* ptr = VirtualAlloc(
        nullptr,
        kpage << 13,          // 总字节数量，8KB 为一页
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE
    );
#else
    // Linux 平台
    // nullptr：让系统自动选择虚拟地址
    // total_size：总字节数
    // PROT_READ | PROT_WRITE：内存保护属性（可读写）
    // MAP_PRIVATE | MAP_ANONYMOUS：私有匿名映射（不关联文件，仅用于内存分配）
    // -1：文件描述符（匿名映射）
    // 0：文件偏移量（匿名映射）
    size_t total_size = kpage << 13;
    void* ptr = mmap(
        nullptr,
        total_size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0
    );

    // mmap 失败时返回 MAP_FAILED，也就是(void*)-1
    if (ptr == MAP_FAILED)
    {
        ptr = nullptr;
    }
#endif

    // 分配失败返回 nullptr，由调用者处理
    if (ptr == nullptr)
    {
        return nullptr;
    }

    return ptr;
}

// 在堆上释放空间
inline static void SystemFree(void* ptr)
{
#ifdef _WIN32
    VirtualFree(ptr, 0, MEM_RELEASE);
#else

#endif
}

// static避免多个文件调用产生冲突
// 需要修改指针，所以加引用
static void*& ObjNext(void* obj){
	return *(void**)obj;
}

// 空闲链表类，用来管理从线程回收的空间
class FreeList{
public:
	size_t Size(){ return _size;}

	// 范围删除size个内存块，返回删除的空间
	void PopRange(void*& start, void*& end, size_t size){
		assert(_size >= size);

		start = _freeList;
		end  = _freeList;
		for(size_t i = 0; i < size - 1; ++i){
			end = ObjNext(end);
		}

		_freeList = ObjNext(end);
		ObjNext(end) = nullptr;

		_size -= size;
	}

  // cc 提供空间给tc 时，一次性接入整个链表
  void PushRange(void* start, void* end, size_t size){
    ObjNext(end) = _freeList;
    _freeList = start;

	_size += size;
  }

  // 判断哈希表是否为空
  bool Empty(){
    return _freeList == nullptr;
  }

	// 回收空间
	void Push(void* obj){
		assert(obj); // 判断指针是否不为空

		ObjNext(obj) = _freeList;
		_freeList = obj;

		++_size;
	}

	// 为线程提供空间
	void* Pop(){
		assert(_freeList); // 判断链表是否为空，不为空才能提供空间

		void* obj = _freeList;
		_freeList = ObjNext(obj);

		--_size;

		return obj;
	}
  
  // 线程缓存能向中央缓存申请的最大空间（达到上限前）
  size_t& MaxSize(){ return _maxSize;}

private:
	void* _freeList = nullptr;
	size_t _maxSize = 1;

	size_t _size = 0; // 统计当前空闲链表的内存块数量
};

// 计算空闲链表中，每个分区对齐后使用的字节数。
class SizeClass{
public:
	// 线程缓存能向中央缓存申请空间的上限
	static size_t NumMoveSize(size_t size){
	  assert(size > 0);
	  int num = MAX_BYTES / size;

	  if(num > 512){ num = 512;} // 防止空间浪费
	  if(num < 2){ num = 2;}

	  return num;
	}

	  // cc 向pc 申请span时，先将内存块转换为对应的页数
	  static size_t NumMovePage(size_t size){
		//  tc向cc申请内存块的最大数量
		size_t num = NumMoveSize(size);
		size_t npage = num * size;
		// 除以每页大小（位数），求申请的最大页面数
		npage >>= PAGE_SHIFT;

		if(npage == 0){ npage = 1;}
		return npage;
	  }

	static size_t _RoundUp(size_t size, size_t alignNum){
		if(size % alignNum){
			// 如果对齐后多出一截，要补齐
			return (size / alignNum + 1) * alignNum;
		}
		else{
			return size;
		}
	}	

	static size_t RoundUp(size_t size){
		// [1, 128]字节，每格对齐8字节
		if(size <= 128){ return _RoundUp(size, 8);}

		// [128 + 1, 1024] 16B
		else if(size <= 1024){ return _RoundUp(size, 16);}

		
		//[1024 + 1, 8 * 1024] 128B
		else if(size <= 8 * 1024){ return _RoundUp(size, 128);} 

		//[8 * 1024 + 1， 64 * 1024] 1024B
		else if(size <= 64 * 1024){ return _RoundUp(size, 1024);} 

		//[64 * 1024 + 1, 256 * 1024] 8 * 1024B
		else if(size <= 256 * 1024){ return _RoundUp(size, 8 * 1024);} 

		//线程缓存单次最多申请256KB
		else{ assert(false); return -1;}
	}
	
	// 求size对应在哈希表中的下标
	static inline size_t _Index(size_t size, size_t align_shift)
	{							
	// align_shift是指对齐数的二进制位数
		return ((size + (1 << align_shift) - 1) >> align_shift) - 1;
		//_Index的返回值需要加上前面所有区域的哈希桶的个数
	}

	// 计算映射的空闲链表桶位置
	static inline size_t Index(size_t size)
	{
		assert(size <= MAX_BYTES);

		// 每个区间的链表数量
		static int group_array[4] = { 16, 56, 56, 56 };
		if (size <= 128)
		{ // [1,128] 8B 对应二进制位为3位
			return _Index(size, 3); // 3是指对齐数的二进制位位数
		}
		else if (size <= 1024)
		{ // [128+1,1024] 16B -->4位
			return _Index(size - 128, 4) + group_array[0];
		}
		else if (size <= 8 * 1024)
		{ // [1024+1,8*1024] 128B -->7位
			return _Index(size - 1024, 7) + group_array[1] + group_array[0];
		}
		else if (size <= 64 * 1024)
		{ // [8*1024+1,64*1024] 1024B -->10位
			return _Index(size - 8 * 1024, 10) + group_array[2] + group_array[1]
				+ group_array[0];
		}
		else if (size <= 256 * 1024)
		{ // [64*1024+1,256*1024] 8 * 1024B  -->13位
			return _Index(size - 64 * 1024, 13) + group_array[3] +
				group_array[2] + group_array[1] + group_array[0];
		}
		else
		{
			// 单次申请空间超过256KB，对齐到页大小
			return _RoundUp(size, 1 << PAGE_SHIFT);
		}
		return -1;
	}
};

struct Span{
  public:
    size_t _pageID = 0; // 页号。size_t的大小会根据平台自动改变
    size_t _n = 0; // span管理的页数量
	size_t _objSize = 0; // span管理的页切分的内存块大小

    void* _freeList = nullptr; // 挂载小空间的头节点
    size_t use_count = 0; // 计算span分配的小空间数量

    Span* _prev = nullptr; // 指向前一个节点
    Span* _next = nullptr; // 指向后一个节点

	bool _isUse = false; // 判断当前span在cc中还是在pc中，false表示在pc中，true表示在cc中
	
	// 默认构造函数
	Span() = default;
};

class SpanList{
  public:
  	std::mutex _mtx; // 互斥锁，中央缓存每个哈希桶的桶锁
  
    void PushFront(Span* span){ Insert(Begin(), span);}
    Span* PopFront(){
      Span* front = _head->_next;
      Erase(front);

      return front;
    }

    bool Empty(){ return _head == _head->_next;}
    // spanlist的头节点和尾节点
    Span* Begin(){ return _head->_next;}
    Span* End(){ return _head;}

    SpanList(){
      _head = new Span;

      _head->_next = _head;
      _head->_prev = _head;
    }

    void Insert(Span* pos, Span* ptr){
      // ptr插入到pos前面
      assert(pos); assert(ptr);

      Span* prev = pos->_prev;
      prev->_next = ptr;
      ptr->_prev = prev;
      ptr->_next = pos;
      pos->_prev = ptr;
    }

    void Erase(Span* pos){
      assert(pos); assert(pos != _head);

      Span* prev = pos->_prev;
      Span* next = pos->_next;
      prev->_next = next;
      next->_prev = prev;
      // pos节点需要回收，不直接删除
    }

  private:
    Span* _head; // 哨兵头节点
};