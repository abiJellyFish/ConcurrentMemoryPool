#pragma once 

#include "Common.h"
#include "ThreadCache.h"
#include "PageCache.h"
#include <thread>
#include <mutex>
#include <iostream>
#include <atomic>

// 线程申请空间的接口
void* ConcurrentAlloc(size_t size);

// 线程回收空间的接口
void ConcurrentFree(void* obj);
