#pragma once

#include "CoreMinimal.h"

// Minimum-cost maximum flow on a small directed graph with real capacities (successive shortest paths with
// Bellman-Ford, so negative edge costs are allowed). Used by the flow plan (Docs/Architecture.md, "Dispatching").
class FSTMinCostFlow
{
public:
	explicit FSTMinCostFlow(int32 NodeCount);

	// Adds an edge and returns its id for GetFlow.
	int32 AddEdge(int32 From, int32 To, double Capacity, double Cost);

	// Sends as much flow as possible from Source to Sink at the least total cost; returns the amount sent.
	double Solve(int32 Source, int32 Sink);

	double GetFlow(int32 Edge) const;

private:
	struct FEdge
	{
		int32 To = 0;
		double Capacity = 0.0;
		double Cost = 0.0;
		double Flow = 0.0;
	};
	// Edge 2k is the forward edge k, 2k+1 its reverse.
	TArray<FEdge> mEdges;
	TArray<TArray<int32>> mOut;
};
