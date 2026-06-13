#pragma once

#include <iostream>
#include <mutex>

using std::cout;
using std::endl;

template<class T> // 根据不同类型创建不同的定长内存池
class ObjectPool{
private:
	char* _memory = nullptr; // 指向申请的大内存块的指针
	// 每次从大块空间划分走一块空间时，_memory向后移动一个T类型大小
	// 由于void* 不能自增或解引用，所以使用char*
	
	size_t _remanentBytes = 0; // 大块内存在划分过程中的剩余字节数
	// 因为在划分时可能大块内存无法整除T类型大小，所以剩余空间小于T大小时就新申请一大块内存
	// size_t 是无符号整型，大小由平台决定

	void* _freelist = nullptr; // 自由链表，连接要归还的内存空间
	// 将分配走的小块空间用链表连接，归还顺序不固定
	// 使用头插法，在内存块开头选取一个指针大小的空间（64位下是8字节）来指向后面的内存块

public:
	std::mutex _poolMtx; // 保证多线程安全

	// 申请空间函数ObjectPool.h
	T* New(){
		std::lock_guard<std::mutex> lock(_poolMtx); // 添加锁保护
		
		T* obj = nullptr; // 用于返回申请的空间
		
		// 如果空闲链表不为空，说明有可以重复使用的内存块
		if(_freelist){
			void* next = *(void**) _freelist;
			obj = (T*) _freelist;
			_freelist = next; // 删除头指针
			
			// cout << "Reuse memory blocks." << endl;
		}
		else{
			// 第一次申请，以及剩余空间小于T大小时，新申请一大块空间
			if(_remanentBytes < sizeof(T)){
				_remanentBytes = 128 * 1024;
				_memory = (char*)malloc(_remanentBytes);
				
				// cout << "Request memory pool space." << endl;
			
				if(_memory == nullptr){
					throw std::bad_alloc(); // 如果申请失败，抛出异常
				}
			}
		
			obj = (T*) _memory; // 划分T大小的空间，指向其开头
			// 如果T的大小比指针还小，则小内存块补为指针大小，才保证能接入空闲指针
			size_t objSize = (sizeof(T) < sizeof(void*)) ? sizeof(void*) : sizeof(T);
			_memory += objSize;
			_remanentBytes -= objSize;
			
			// cout << "Allocate space from the memory pool." << objSize << " bytes" << endl;
		}
		
		new(obj) T; // 调用构造函数对分配的空间初始化
		return obj;
	}
	
	// 回收归还的小空间，接入到空闲链表之后
	void Delete(T* obj){
		std::lock_guard<std::mutex> lock(_poolMtx); // 添加锁保护
		
		obj->~T(); // 显式调用析构函数清理	

		*(void**)obj = _freelist; // 创建指向旧内存块（初始为空）的新块
		// 用双层指针再解引用，可以不论32位或64位获得指针大小
		_freelist = obj; // 头指针指向新内存块
		
		// cout << "Return space to the memory pool." << endl;
	}
	// 因为最终会归还所有的空间，不会造成内存泄漏
};
