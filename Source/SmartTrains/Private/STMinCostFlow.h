#pragma once

#include "CoreMinimal.h"

// Maximum flow on a directed graph with real capacities, for belt networks (Docs/Architecture.md, "Station set-up").
class FSTMinCostFlow
{
public:
	explicit FSTMinCostFlow(int32 NodeCount);

	// Adds an edge and returns its id for GetFlow.
	int32 AddEdge(int32 From, int32 To, double Capacity, double Cost);

	// Sends as much flow as possible from Source to Sink, costs ignored (shortest augmenting paths by breadth-first
	// search); returns the amount sent.
	double SolveMaxFlow(int32 Source, int32 Sink);

	// After solving: true for each node still reachable from Source with room left, the source side of a minimum cut.
	TArray<bool> GetSourceSide(int32 Source) const;


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
