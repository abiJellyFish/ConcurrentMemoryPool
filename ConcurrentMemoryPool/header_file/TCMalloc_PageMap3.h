#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <cassert>

// 不包含 Common.h，避免循环依赖
typedef size_t PageID;

// Three-level radix tree for 64-bit systems
// 使用固定的位数分配，避免节点过大
template <int BITS>
class TCMalloc_PageMap3 {
private:
	// 使用固定的位数分配，确保每层大小合理
	static const int ROOT_BITS = 10;    // 第一层 10 位
	static const int INTER_BITS = 11;   // 第二层 11 位  
	static const int LEAF_BITS = BITS - ROOT_BITS - INTER_BITS; // 剩余位给第三层

	static const int ROOT_LENGTH = 1 << ROOT_BITS;      // 1024
	static const int INTER_LENGTH = 1 << INTER_BITS;    // 2048
	static const int LEAF_LENGTH = 1 << LEAF_BITS;      // 根据 BITS 计算

	struct Leaf {
		void* values[LEAF_LENGTH];
	};

	struct Node {
		Leaf* leafs[INTER_LENGTH];
	};

	Node* root_[ROOT_LENGTH];

public:
	typedef uintptr_t Number;

	explicit TCMalloc_PageMap3() {
		memset(root_, 0, sizeof(root_));
	}

	void* get(Number k) const {
		if ((k >> BITS) > 0) {
			return nullptr;
		}

		const Number i1 = k >> (INTER_BITS + LEAF_BITS);
		const Number i2 = (k >> LEAF_BITS) & (INTER_LENGTH - 1);
		const Number i3 = k & (LEAF_LENGTH - 1);

		if (i1 >= ROOT_LENGTH || root_[i1] == nullptr || root_[i1]->leafs[i2] == nullptr) {
			return nullptr;
		}
		return root_[i1]->leafs[i2]->values[i3];
	}

	void set(Number k, void* v) {
		if ((k >> BITS) > 0) {
			return;
		}

		const Number i1 = k >> (INTER_BITS + LEAF_BITS);
		const Number i2 = (k >> LEAF_BITS) & (INTER_LENGTH - 1);
		const Number i3 = k & (LEAF_LENGTH - 1);

		if (i1 >= ROOT_LENGTH || root_[i1] == nullptr || root_[i1]->leafs[i2] == nullptr) {
			return;
		}

		root_[i1]->leafs[i2]->values[i3] = v;
	}

	void erase(Number k) {
		if ((k >> BITS) > 0) {
			return;
		}

		const Number i1 = k >> (INTER_BITS + LEAF_BITS);
		const Number i2 = (k >> LEAF_BITS) & (INTER_LENGTH - 1);
		const Number i3 = k & (LEAF_LENGTH - 1);

		if (i1 >= ROOT_LENGTH || root_[i1] == nullptr || root_[i1]->leafs[i2] == nullptr) {
			return;
		}

		root_[i1]->leafs[i2]->values[i3] = nullptr;
	}

	bool Ensure(Number start, size_t n) {
		for (Number key = start; key < start + n; ++key) {
			if ((key >> BITS) > 0) {
				return false;
			}

			const Number i1 = key >> (INTER_BITS + LEAF_BITS);
			const Number i2 = (key >> LEAF_BITS) & (INTER_LENGTH - 1);

			if (i1 >= ROOT_LENGTH)
				return false;

			if (root_[i1] == nullptr) {
				Node* node = (Node*)malloc(sizeof(Node));
				if (node == nullptr) return false;
				memset(node, 0, sizeof(*node));
				root_[i1] = node;
			}

			if (root_[i1]->leafs[i2] == nullptr) {
				Leaf* leaf = (Leaf*)malloc(sizeof(Leaf));
				if (leaf == nullptr) return false;
				memset(leaf, 0, sizeof(*leaf));
				root_[i1]->leafs[i2] = leaf;
			}
		}
		return true;
	}
};
