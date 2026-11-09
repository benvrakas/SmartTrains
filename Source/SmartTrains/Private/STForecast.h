#pragma once

#include "CoreMinimal.h"

#include "STSubsystem.h"

// How a node's stock develops over the planning horizon, and what that costs (Docs/Architecture.md, "Forecast cost").
// Stock is piecewise linear between events, clamped to 0..capacity.
namespace STForecast
{
	// Planning horizon. [s]
	constexpr double HorizonSeconds = 3600.0;
	// A car load of production lost because a supplier is full costs as much as being a car load short for this long
	// at a consumer: little, so it only decides between otherwise equal trips (production nobody needs isn't worth a
	// train; the server log of 2026-11-07 had most suppliers full). [min]
	constexpr double LostLoadMinutes = 0.25;
	// Horizon planner: most stops in a train's plan (several trips).
	constexpr int32 HorizonStops = 6;
	// What a second of a train's time costs against the forecast cost a trip removes: a trip's score is its value less
	// this times its duration. [car-load minutes per second]
	constexpr double TrainSecondCost = 0.01;

	// Cost of Node over [Now, Now + HorizonSeconds] with Events (any order) on top of its stock, in car-load minutes
	// (amount / CarCapacity): demand, priority weight x shortfall below target over time; supply, production lost while
	// it is full (x LostLoadMinutes).
	double Cost(const FSTNode& Node, const TArray<FSTNodeEvent>& Events, double Now, double CarCapacity);

	// The drop in Node's cost when Added joins Events: what a delivery or pickup is worth.
	double Gain(const FSTNode& Node, const TArray<FSTNodeEvent>& Events, const FSTNodeEvent& Added, double Now, double CarCapacity);

	// When Node's stock with Events first falls below its target (demand) or reaches capacity (supply), or Now +
	// HorizonSeconds if not within the horizon. [game seconds]
	double Deadline(const FSTNode& Node, const TArray<FSTNodeEvent>& Events, double Now, double CarCapacity);

	// The rate Node's stock changes by without trains: use (demand) or production (supply). A supplier that is
	// nearly full produces at least a car load per half hour, since a full one can't measure its production. [/s]
	double GetRate(const FSTNode& Node, double CarCapacity);
}
