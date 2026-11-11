#pragma once

#include "CoreMinimal.h"
#include "STMigration.h"

class AFGTrainStationIdentifier;
class ASTSubsystem;
struct FSTNamingConfig;

// Station set-up (Docs/Architecture.md, "Station set-up"): platform roles and items from what is attached, and a
// station name from the factory behind it, its map area and its theoretical rate.
namespace STSetup
{
	// What the factory behind a station makes, and how much at most when trains bring all it needs.
	struct FFactory
	{
		TSubclassOf<UFGItemDescriptor> Product;
		// Theoretical most per minute (items, or m³ for fluids), limited by the weakest link apart from trains; 0 if
		// unknown.
		double Rate = 0.0;
		// What limits Rate: "2 belts out of P1 (Plastic, 1,560/min)", "8 Assemblers", ...
		FString Limit;
		// The product is made by machines (not only passed on from belts).
		bool bManufactured = false;
		// A station unloading one item: that item, how much of it the factory can use at most (this station's share,
		// within its own belts or pipes), and what limits that.
		TSubclassOf<UFGItemDescriptor> InputItem;
		double InputRate = 0.0;
		FString InputLimit;
		int32 Machines = 0;
		TArray<FString> Notes;
	};

	// Roles and items for Station's cargo platforms: role from what is attached, items from the connection trace,
	// leaving out items the factory makes itself. Platforms that already have a role are kept as they are (but Redo,
	// proposed afresh). bShare: platforms of one role serving the same machines share the items out (the migration
	// does that once for all stations instead).
	STMigration::FStationProposal ProposeStation(const ASTSubsystem& Subsystem, AFGTrainStationIdentifier* Station,
		const AFGBuildableTrainPlatformCargo* Redo = nullptr, bool bShare = true);

	// The factory behind a proposed station: machines reached through its platforms and on through machines (flood
	// fills), until other train platforms.
	FFactory AnalyzeFactory(const ASTSubsystem& Subsystem, const STMigration::FStationProposal& Proposal);

	// Sets Proposal's platforms (and with bRename the station's name), recording what was there in Undo if given.
	// Kept platforms are left alone; platforms already set the same keep their settings (only their machines are kept
	// again). Returns one line per change.
	TArray<FString> ApplyProposal(ASTSubsystem& Subsystem, const STMigration::FStationProposal& Proposal, bool bRename, const FString& Cause,
		FSTMigrationUndo* Undo = nullptr);

	// Fills Proposal.NewName and NameNote. Taken: names of other stations (and names given earlier in the same run);
	// the new name is added to it.
	void NameStation(const ASTSubsystem& Subsystem, STMigration::FStationProposal& Proposal, TSet<FString>& Taken);

	// The game's map area at Location ("Grass Fields"), or empty; OutSource says how it was found.
	FString GetMapArea(const UWorld* World, const FVector& Location, FString& OutSource);

	// Fills Format's {area}, {product}, {role}, {n} and {rate}; empty ones drop out with their brackets.
	FString FormatName(const FString& Format, const FString& Area, const FString& Product, const FString& Role, int32 Number, const FString& Rate);

	// The number for {n}: the one CurrentName already has if no other station took it, else the smallest from 1 whose
	// name (any rate) no station in Taken has.
	int32 PickNumber(const FString& Format, const FString& Area, const FString& Product, const FString& Role, const FString& CurrentName, const TSet<FString>& Taken);

	// True if Name is what Format gives for these values with any rate (or none).
	bool MatchesName(const FString& Name, const FString& Format, const FString& Area, const FString& Product, const FString& Role, int32 Number);

	// "240", "1,560", "7.5".
	FString FormatRate(double PerMinute);

	// Station-menu and chat lines for a proposal.
	TArray<FString> Describe(const STMigration::FStationProposal& Proposal);
}
