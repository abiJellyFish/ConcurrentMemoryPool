/*这里测试的是让多线程申请ntimes*rounds次，比较malloc和刚写完的ConcurrentAlloc的效率*/

/*比较的时候分两种情况，
一种是申请ntimes*rounds次同一个块大小的空间，
一种是申请ntimes*rounds次不同的块大小的空间*/

/*下面的代码稍微过一眼就好*/

#include"ConcurrentAlloc.h"
#include"ConcurrentAllocator.h"

// ntimes 一轮申请和释放内存的次数
// rounds 轮次
// nwors表示创建多少个线程
void BenchmarkMalloc(size_t ntimes, size_t nworks, size_t rounds)
{
	std::vector<std::thread> vthread(nworks);
	std::atomic<size_t> malloc_costtime = 0;
	std::atomic<size_t> free_costtime = 0;

	for (size_t k = 0; k < nworks; ++k)
	{
		vthread[k] = std::thread([&, k]() {
			std::vector<void*> v;
			v.reserve(ntimes);

			for (size_t j = 0; j < rounds; ++j)
			{
				size_t begin1 = clock();
				for (size_t i = 0; i < ntimes; i++)
				{
					//v.push_back(malloc(16)); // 每一次申请同一个桶中的块
					v.push_back(malloc((16 + i) % 8192 + 1));// 每一次申请不同桶中的块
				}
				size_t end1 = clock();

				size_t begin2 = clock();
				for (size_t i = 0; i < ntimes; i++)
				{
					free(v[i]);
				}
				size_t end2 = clock();
				v.clear();

				malloc_costtime += (end1 - begin1);
				free_costtime += (end2 - begin2);
			}
			});
	}

	for (auto& t : vthread)
	{
		t.join();
	}

	printf("%u个线程并发执行%u轮次，每轮次malloc %u次: 花费：%u ms\n",
		nworks, rounds, ntimes, malloc_costtime.load());

	printf("%u个线程并发执行%u轮次，每轮次free %u次: 花费：%u ms\n",
		nworks, rounds, ntimes, free_costtime.load());

	printf("%u个线程并发malloc&free %u次，总计花费：%u ms\n",
		nworks, nworks * rounds * ntimes, malloc_costtime.load() + free_costtime.load());
}


// 								单轮次申请释放次数 线程数 轮次
void BenchmarkConcurrentMalloc(size_t ntimes, size_t nworks, size_t rounds)
{
	std::vector<std::thread> vthread(nworks);
	std::atomic<size_t> malloc_costtime = 0;
	std::atomic<size_t> free_costtime = 0;

	for (size_t k = 0; k < nworks; ++k)
	{
		vthread[k] = std::thread([&]() {
			std::vector<void*> v;
			v.reserve(ntimes);

			for (size_t j = 0; j < rounds; ++j)
			{
				size_t begin1 = clock();
				for (size_t i = 0; i < ntimes; i++)
				{
					//v.push_back(ConcurrentAlloc(16));
					v.push_back(ConcurrentAlloc((16 + i) % 8192 + 1));
				}
				size_t end1 = clock();

				size_t begin2 = clock();
				for (size_t i = 0; i < ntimes; i++)
				{
					ConcurrentFree(v[i]);
				}
				size_t end2 = clock();
				v.clear();

				malloc_costtime += (end1 - begin1);
				free_costtime += (end2 - begin2);
			}
			});
	}

	for (auto& t : vthread)
	{
		t.join();
	}

	printf("%u个线程并发执行%u轮次，每轮次concurrent alloc %u次: 花费：%u ms\n",
		nworks, rounds, ntimes, malloc_costtime.load());

	printf("%u个线程并发执行%u轮次，每轮次concurrent dealloc %u次: 花费：%u ms\n",
		nworks, rounds, ntimes, free_costtime.load());

	printf("%u个线程并发concurrent alloc&dealloc %u次，总计花费：%u ms\n",
		nworks, nworks * rounds * ntimes, malloc_costtime.load() + free_costtime.load());
}

// 测试 std::vector 使用 std::allocator 的性能（创建大量小对象）
void BenchmarkVectorStdAllocator(size_t ntimes, size_t nworks, size_t rounds)
{
	std::vector<std::thread> vthread(nworks);
	std::atomic<size_t> create_costtime = 0;
	std::atomic<size_t> destroy_costtime = 0;

	for (size_t k = 0; k < nworks; ++k)
	{
		vthread[k] = std::thread([&]() {
			for (size_t j = 0; j < rounds; ++j)
			{
				std::vector<std::vector<int>> vecs;
				vecs.reserve(ntimes);

				// 创建大量小 vector 对象，每次都是新的内存分配
				size_t begin1 = clock();
				for (size_t i = 0; i < ntimes; i++)
				{
					vecs.emplace_back(10, i);  // 每个 vector 包含 10 个 int
				}
				size_t end1 = clock();

				// 销毁所有小 vector 对象
				size_t begin2 = clock();
				vecs.clear();
				vecs.shrink_to_fit();
				size_t end2 = clock();

				create_costtime += (end1 - begin1);
				destroy_costtime += (end2 - begin2);
			}
			});
	}

	for (auto& t : vthread)
	{
		t.join();
	}

	printf("%u个线程并发执行%u轮次，每轮次创建%u个小vector(std::allocator): 花费：%u ms\n",
		nworks, rounds, ntimes, create_costtime.load());

	printf("%u个线程并发执行%u轮次，每轮次销毁%u个小vector(std::allocator): 花费：%u ms\n",
		nworks, rounds, ntimes, destroy_costtime.load());

	printf("%u个线程并发创建&销毁%u个小vector(std::allocator)，总计花费：%u ms\n",
		nworks, nworks * rounds * ntimes, create_costtime.load() + destroy_costtime.load());
}

// 测试 std::vector 使用 ConcurrentAllocator 的性能（创建大量小对象）
void BenchmarkVectorConcurrentAllocator(size_t ntimes, size_t nworks, size_t rounds)
{
	std::vector<std::thread> vthread(nworks);
	std::atomic<size_t> create_costtime = 0;
	std::atomic<size_t> destroy_costtime = 0;

	for (size_t k = 0; k < nworks; ++k)
	{
		vthread[k] = std::thread([&]() {
			for (size_t j = 0; j < rounds; ++j)
			{
				std::vector<std::vector<int, ConcurrentAllocator<int>>, 
						   ConcurrentAllocator<std::vector<int, ConcurrentAllocator<int>>>> vecs;
				vecs.reserve(ntimes);

				// 创建大量小 vector 对象，每次都是新的内存分配
				size_t begin1 = clock();
				for (size_t i = 0; i < ntimes; i++)
				{
					vecs.emplace_back(10, i);  // 每个 vector 包含 10 个 int
				}
				size_t end1 = clock();

				// 销毁所有小 vector 对象
				size_t begin2 = clock();
				vecs.clear();
				vecs.shrink_to_fit();
				size_t end2 = clock();

				create_costtime += (end1 - begin1);
				destroy_costtime += (end2 - begin2);
			}
			});
	}

	for (auto& t : vthread)
	{
		t.join();
	}

	printf("%u个线程并发执行%u轮次，每轮次创建%u个小vector(ConcurrentAllocator): 花费：%u ms\n",
		nworks, rounds, ntimes, create_costtime.load());

	printf("%u个线程并发执行%u轮次，每轮次销毁%u个小vector(ConcurrentAllocator): 花费：%u ms\n",
		nworks, rounds, ntimes, destroy_costtime.load());

	printf("%u个线程并发创建&销毁%u个小vector(ConcurrentAllocator)，总计花费：%u ms\n",
		nworks, nworks * rounds * ntimes, create_costtime.load() + destroy_costtime.load());
}

int main()
{
	// size_t n = 10000;
    size_t n = 10000;

	cout << "==========================================================" << endl;
	fflush(stdout);
	
	// 这里表示4个线程，每个线程申请10万次，总共申请40万次
	BenchmarkConcurrentMalloc(n, 4, 1); 
	cout << endl << endl;
	fflush(stdout);
	
	// 这里表示4个线程，每个线程申请10万次，总共申请40万次
	BenchmarkMalloc(n, 4, 1);
	cout << endl << endl;
	fflush(stdout);

	// std::vector 使用 std::allocator 的性能测试
	BenchmarkVectorStdAllocator(n, 4, 1);
	cout << endl << endl;
	fflush(stdout);

	// std::vector 使用 ConcurrentAllocator 的性能测试
	BenchmarkVectorConcurrentAllocator(n, 4, 1);
	cout << "==========================================================" << endl;
	fflush(stdout);

	return 0;
}
