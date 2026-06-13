#pragma once

#include <memory>
#include <cstddef>
#include "ConcurrentAlloc.h"

// 自定义内存分配器，使用并发内存池
template <typename T>
class ConcurrentAllocator {
public:
    // 类型别名
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using reference = T&;
    using const_reference = const T&;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    
    // rebind 机制，用于分配不同类型
    template <typename U>
    struct rebind {
        using other = ConcurrentAllocator<U>;
    };
    
    // 默认构造函数
    ConcurrentAllocator() noexcept = default;
    
    // 拷贝构造函数
    ConcurrentAllocator(const ConcurrentAllocator&) noexcept = default;
    
    // 从其他类型的分配器构造
    template <typename U>
    ConcurrentAllocator(const ConcurrentAllocator<U>&) noexcept {}
    
    // 析构函数
    ~ConcurrentAllocator() noexcept = default;
    
    // 分配内存（不构造对象）
    pointer allocate(size_type n) {
        if (n == 0) return nullptr;
        if (n > max_size()) throw std::bad_alloc();
        
        size_t bytes = n * sizeof(T);
        void* ptr = ConcurrentAlloc(bytes);
        
        if (ptr == nullptr) throw std::bad_alloc();
        
        return static_cast<pointer>(ptr);
    }
    
    // 释放内存（不析构对象）
    void deallocate(pointer p, size_type n) noexcept {
        (void)n; // 忽略大小参数，ConcurrentFree 不需要
        if (p == nullptr) return;
        ConcurrentFree(p);
    }
    
    // 构造对象（C++17 之前）
    template <typename U, typename... Args>
    void construct(U* p, Args&&... args) {
        ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }
    
    // 析构对象（C++17 之前）
    template <typename U>
    void destroy(U* p) {
        p->~U();
    }
    
    // 获取最大分配大小
    size_type max_size() const noexcept {
        return SIZE_MAX / sizeof(T);
    }
    
    // 地址获取
    pointer address(reference x) const noexcept {
        return std::addressof(x);
    }
    
    const_pointer address(const_reference x) const noexcept {
        return std::addressof(x);
    }
};

// 分配器相等性比较
template <typename T, typename U>
bool operator==(const ConcurrentAllocator<T>&, const ConcurrentAllocator<U>&) noexcept {
    return true;
}

template <typename T, typename U>
bool operator!=(const ConcurrentAllocator<T>&, const ConcurrentAllocator<U>&) noexcept {
    return false;
}

// C++17 特性：allocate_at_least（可选）
#if __cplusplus >= 201703L
template <typename T>
struct std::allocator_traits<ConcurrentAllocator<T>> {
    using allocator_type = ConcurrentAllocator<T>;
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using void_pointer = void*;
    using const_void_pointer = const void*;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    using propagate_on_container_copy_assignment = std::false_type;
    using propagate_on_container_move_assignment = std::false_type;
    using propagate_on_container_swap = std::false_type;
    using is_always_equal = std::true_type;
    
    template <typename U>
    using rebind_alloc = ConcurrentAllocator<U>;
    
    template <typename U>
    using rebind_traits = std::allocator_traits<ConcurrentAllocator<U>>;
    
    static pointer allocate(allocator_type& a, size_type n) {
        return a.allocate(n);
    }
    
    static pointer allocate(allocator_type& a, size_type n, const_void_pointer hint) {
        (void)hint;
        return a.allocate(n);
    }
    
    static void deallocate(allocator_type& a, pointer p, size_type n) {
        a.deallocate(p, n);
    }
    
    template <typename U, typename... Args>
    static void construct(allocator_type& a, U* p, Args&&... args) {
        a.construct(p, std::forward<Args>(args)...);
    }
    
    template <typename U>
    static void destroy(allocator_type& a, U* p) {
        a.destroy(p);
    }
    
    static size_type max_size(const allocator_type& a) noexcept {
        return a.max_size();
    }
    
    static allocator_type select_on_container_copy_construction(const allocator_type& a) {
        return a;
    }
};
#endif
