#pragma once

#include <QVector>

#include <algorithm>
#include <functional>
#include <utility>

namespace Curation::Scoring {

// llama.cpp supports large logical batches, but in-process embedding/reranking inside
// OBS must be conservative: a crash in llama_decode would take OBS down. This helper
// treats batch calls as a bounded, cancellable micro-batch loop. Providers keep a
// single context lock while decoding, but callers still get deterministic batch
// semantics, stable result ordering, and zero unbounded queue growth.
template<typename Input, typename Output, typename Worker>
QVector<Output> runSafeLlamaCppBatch(const QVector<Input> &inputs, int requestedBatchSize,
				     const std::function<bool()> &cancellationCallback, Worker &&worker)
{
	QVector<Output> results;
	results.resize(inputs.size());
	if (inputs.isEmpty())
		return results;

	const int batchSize = std::clamp(requestedBatchSize <= 0 ? 1 : requestedBatchSize, 1, 32);
	for (int offset = 0; offset < inputs.size(); offset += batchSize) {
		if (cancellationCallback && cancellationCallback())
			break;

		const int end = std::min(offset + batchSize, static_cast<int>(inputs.size()));
		for (int i = offset; i < end; ++i) {
			if (cancellationCallback && cancellationCallback())
				return results;
			results[i] = worker(inputs.at(i), i);
		}
	}
	return results;
}

} // namespace Curation::Scoring
