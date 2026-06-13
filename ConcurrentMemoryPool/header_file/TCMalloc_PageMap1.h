#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

#include "Common.h"

// Single-level array
template <int BITS>
class TCMalloc_PageMap1 {
private:
	static const int LENGTH = 1 << BITS; // 数组要开的长度
	void** array_; // 底层存放指针的数组

public:
	typedef uintptr_t Number;

	explicit TCMalloc_PageMap1() {// 开空间
		size_t size = sizeof(void*) << BITS;
		// 直接按页大小对齐，不使用 SizeClass::_RoundUp（它只处理256KB以内）
		size_t alignSize = (size + (1 << PAGE_SHIFT) - 1) & ~((1 << PAGE_SHIFT) - 1);
		array_ = (void**)SystemAlloc(alignSize >> PAGE_SHIFT);
		memset(array_, 0, sizeof(void*) << BITS);
	}

	// Return the current value for KEY.  Returns NULL if not yet set,
	// or if k is out of range.
	void* get(Number k) const { // 通过k来获取对应的指针
		if ((k >> BITS) > 0) {
			return nullptr;
		}
		return array_[k];
	}

	// REQUIRES "k" is in range "[0,2^BITS-1]".
	// REQUIRES "k" has been ensured before.
	//
	// Sets the value 'v' for key 'k'.
	void set(Number k, void* v) { // 将v设置到k下标
		array_[k] = v;
	}

	// Erase the value for key 'k'
	void erase(Number k) { // 移除k下标的值
		array_[k] = nullptr;
	}
};