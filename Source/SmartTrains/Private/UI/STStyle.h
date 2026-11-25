#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Styling/SlateBrush.h"
#include "STStyle.generated.h"

// SmartTrains' look: dark panels with FICSIT orange, in the game's font (Docs/Architecture.md, "UI style").
namespace STStyle
{
	extern const FLinearColor Orange;
	extern const FLinearColor OrangeDeep;
	extern const FLinearColor Panel;
	extern const FLinearColor Header;
	extern const FLinearColor Card;
	extern const FLinearColor Raised;
	extern const FLinearColor Inset;
	extern const FLinearColor Line;
	extern const FLinearColor Text;
	extern const FLinearColor Muted;
	extern const FLinearColor Good;
	extern const FLinearColor Warn;
	extern const FLinearColor Bad;
	extern const FLinearColor Dark;
	// Background of a row or tile that needs a look.
	extern const FLinearColor Alert;

	// A filled box with rounded corners.
	FSlateBrush Rounded(const FLinearColor& Color, float Radius = 4.f);
	// A filled box with rounded corners and an outline.
	FSlateBrush Outlined(const FLinearColor& Fill, const FLinearColor& Outline, float Width = 1.f, float Radius = 4.f);

	// Colour of a severity: 0 fine, 1 worth a look, 2 needs a look.
	FLinearColor SeverityColor(uint8 Severity);

	// "45 s", "3 min", "1 h 10 min"; "-" below 0.
	FString Duration(double Seconds);
}

// The SmartTrains mark: a hexagon round a locomotive front, drawn at whatever size its slot gives it.
UCLASS()
class USTLogo : public UUserWidget
{
	GENERATED_BODY()

public:
	FLinearColor Frame = STStyle::Orange;
	FLinearColor Mark = STStyle::Text;

protected:
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
		int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
};

// A fill bar that eases to its value; with Pulse its fill glows on and off (a car being loaded).
UCLASS()
class USTBar : public UUserWidget
{
	GENERATED_BODY()

public:
	// 0 to 1.
	void SetValue(float InValue);
	void SetColor(const FLinearColor& InColor) { mFill = InColor; }
	void SetPulse(bool bInPulse) { bPulse = bInPulse; }

protected:
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
		int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

private:
	float mTarget = 0.f;
	float mShown = 0.f;
	float mTime = 0.f;
	bool bPulse = false;
	FLinearColor mFill = STStyle::Orange;
};

// A ring filled to its value (trains on a consumer's loop against its share), easing like the bar.
UCLASS()
class USTRing : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetValue(float InValue);
	void SetColor(const FLinearColor& InColor) { mFill = InColor; }

protected:
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
		int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

private:
	float mTarget = 0.f;
	float mShown = 0.f;
	FLinearColor mFill = STStyle::Orange;
};
