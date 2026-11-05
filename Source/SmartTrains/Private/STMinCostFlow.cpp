#include "STMinCostFlow.h"

namespace
{
	constexpr double FlowEpsilon = 1e-9;
	// Upper bound on augmenting paths, so a numerical corner case can't loop for long.
	constexpr int32 MaxAugmentations = 10000;
}

FSTMinCostFlow::FSTMinCostFlow(int32 NodeCount)
{
	mOut.SetNum(FMath::Max(0, NodeCount));
}

int32 FSTMinCostFlow::AddEdge(int32 From, int32 To, double Capacity, double Cost)
{
	const int32 Id = mEdges.Num() / 2;
	mOut[From].Add(mEdges.Num());
	mEdges.Add({To, Capacity, Cost, 0.0});
	mOut[To].Add(mEdges.Num());
	mEdges.Add({From, 0.0, -Cost, 0.0});
	return Id;
}

double FSTMinCostFlow::GetFlow(int32 Edge) const
{
	return mEdges.IsValidIndex(Edge * 2) ? mEdges[Edge * 2].Flow : 0.0;
}

double FSTMinCostFlow::Solve(int32 Source, int32 Sink)
{
	const int32 Count = mOut.Num();
	double Total = 0.0;
	TArray<double> Distance;
	TArray<int32> Via;
	for (int32 Round = 0; Round < MaxAugmentations; ++Round)
	{
		// Bellman-Ford over the residual graph.
		Distance.Init(TNumericLimits<double>::Max(), Count);
		Via.Init(INDEX_NONE, Count);
		Distance[Source] = 0.0;
		for (int32 Pass = 0; Pass < Count; ++Pass)
		{
			bool bChanged = false;
			for (int32 Node = 0; Node < Count; ++Node)
			{
				if (Distance[Node] == TNumericLimits<double>::Max())
				{
					continue;
				}
				for (const int32 EdgeIndex : mOut[Node])
				{
					const FEdge& Edge = mEdges[EdgeIndex];
					if (Edge.Capacity - Edge.Flow > FlowEpsilon && Distance[Node] + Edge.Cost < Distance[Edge.To] - 1e-12)
					{
						Distance[Edge.To] = Distance[Node] + Edge.Cost;
						Via[Edge.To] = EdgeIndex;
						bChanged = true;
					}
				}
			}
			if (!bChanged)
			{
				break;
			}
		}
		if (Via[Sink] == INDEX_NONE)
		{
			break;
		}
		double Push = TNumericLimits<double>::Max();
		for (int32 Node = Sink; Node != Source; Node = mEdges[Via[Node] ^ 1].To)
		{
			const FEdge& Edge = mEdges[Via[Node]];
			Push = FMath::Min(Push, Edge.Capacity - Edge.Flow);
		}
		if (Push <= FlowEpsilon)
		{
			break;
		}
		for (int32 Node = Sink; Node != Source; Node = mEdges[Via[Node] ^ 1].To)
		{
			mEdges[Via[Node]].Flow += Push;
			mEdges[Via[Node] ^ 1].Flow -= Push;
		}
		Total += Push;
	}
	return Total;
}
