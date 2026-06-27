#pragma once

#include <QVector>

#include <algorithm>
#include <cmath>
#include <future>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace Curation::Scoring::ParallelChunkMap {

inline int boundedWorkerCount(int taskCount, int preferredMaxWorkers)
{
	if (taskCount <= 1)
		return 1;
	const unsigned int hardware = std::max(1u, std::thread::hardware_concurrency());
	return std::clamp(taskCount, 1, std::max(1, std::min(preferredMaxWorkers, static_cast<int>(hardware))));
}

template<typename Chunk> inline void appendMoved(QVector<typename Chunk::value_type> &target, Chunk chunk)
{
	for (auto it = chunk.begin(); it != chunk.end(); ++it)
		target.append(std::move(*it));
}

template<typename ChunkFunc> auto runIndexed(int itemCount, int preferredMaxWorkers, ChunkFunc chunkFunc)
{
	using Chunk = std::decay_t<decltype(chunkFunc(0, 0))>;
	Chunk result;
	if (itemCount <= 0)
		return result;
	result.reserve(itemCount);

	const int workerCount = boundedWorkerCount(itemCount, preferredMaxWorkers);
	if (workerCount <= 1)
		return chunkFunc(0, itemCount);

	std::vector<std::future<Chunk>> futures;
	futures.reserve(workerCount);
	const int chunkSize = std::max(
		1, static_cast<int>(std::ceil(static_cast<double>(itemCount) / static_cast<double>(workerCount))));
	for (int first = 0; first < itemCount; first += chunkSize) {
		const int last = std::min(itemCount, first + chunkSize);
		futures.emplace_back(std::async(std::launch::async,
						[chunkFunc, first, last]() mutable { return chunkFunc(first, last); }));
	}

	for (std::future<Chunk> &future : futures)
		appendMoved(result, future.get());
	return result;
}

template<typename Container, typename Transform>
auto transformIndexed(const Container &items, int preferredMaxWorkers, Transform transform)
{
	using Result = std::decay_t<decltype(transform(items.at(0), 0))>;
	return runIndexed(static_cast<int>(items.size()), preferredMaxWorkers,
			  [&items, &transform](int first, int last) {
				  QVector<Result> chunk;
				  chunk.reserve(std::max(0, last - first));
				  for (int i = first; i < last; ++i)
					  chunk.append(transform(items.at(i), i));
				  return chunk;
			  });
}

} // namespace Curation::Scoring::ParallelChunkMap
