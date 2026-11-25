#include "UI/STStyle.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Rendering/DrawElements.h"

namespace
{
	FLinearColor Srgb(uint8 R, uint8 G, uint8 B, uint8 A = 255)
	{
		return FLinearColor::FromSRGBColor(FColor(R, G, B, A));
	}

	// Seconds over which a bar or ring eases to a new value. [s]
	constexpr float EaseSeconds = 0.35f;
	// One glow of a pulsing bar. [s]
	constexpr float PulseSeconds = 1.6f;

	float Ease(float Shown, float Target, float DeltaTime)
	{
		const float Step = FMath::Clamp(DeltaTime / EaseSeconds, 0.f, 1.f);
		return FMath::Abs(Target - Shown) < 0.001f ? Target : Shown + (Target - Shown) * (1.f - FMath::Pow(1.f - Step, 3.f));
	}
}

namespace STStyle
{
	const FLinearColor Orange = Srgb(250, 149, 73);
	const FLinearColor OrangeDeep = Srgb(122, 74, 34);
	const FLinearColor Panel = Srgb(30, 31, 34, 242);
	const FLinearColor Header = Srgb(38, 39, 43);
	const FLinearColor Card = Srgb(45, 47, 52);
	const FLinearColor Raised = Srgb(42, 38, 32);
	const FLinearColor Inset = Srgb(28, 29, 32);
	const FLinearColor Line = Srgb(58, 60, 65);
	const FLinearColor Text = Srgb(236, 234, 228);
	const FLinearColor Muted = Srgb(169, 171, 176);
	const FLinearColor Good = Srgb(79, 195, 161);
	const FLinearColor Warn = Srgb(242, 193, 78);
	const FLinearColor Bad = Srgb(229, 96, 77);
	const FLinearColor Dark = Srgb(26, 26, 26);
	const FLinearColor Alert = Srgb(46, 35, 24);

	FSlateBrush Rounded(const FLinearColor& Color, float Radius)
	{
		return FSlateRoundedBoxBrush(FSlateColor(Color), Radius);
	}

	FSlateBrush Outlined(const FLinearColor& Fill, const FLinearColor& Outline, float Width, float Radius)
	{
		FSlateRoundedBoxBrush Brush(FSlateColor(Fill), Radius, FSlateColor(Outline), Width);
		return Brush;
	}

	FLinearColor SeverityColor(uint8 Severity)
	{
		return Severity >= 2 ? Orange : Severity == 1 ? Warn : Good;
	}

	FString Duration(double Seconds)
	{
		if (Seconds < 0.0)
		{
			return TEXT("-");
		}
		if (Seconds < 90.0)
		{
			return FString::Printf(TEXT("%.0f s"), Seconds);
		}
		const int32 Minutes = FMath::RoundToInt(Seconds / 60.0);
		return Minutes < 60 ? FString::Printf(TEXT("%d min"), Minutes) : FString::Printf(TEXT("%d h %d min"), Minutes / 60, Minutes % 60);
	}
}

int32 USTLogo::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
	int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const FVector2f Size = AllottedGeometry.GetLocalSize();
	const float S = FMath::Min(Size.X, Size.Y);
	const FVector2f O((Size.X - S) * 0.5f, (Size.Y - S) * 0.5f);
	auto P = [&](float X, float Y) { return FVector2f(O.X + X / 28.f * S, O.Y + Y / 28.f * S); };
	const float Thick = FMath::Max(1.f, S / 14.f);
	const FPaintGeometry Paint = AllottedGeometry.ToPaintGeometry();
	TArray<FVector2f> Hex = {P(14, 2), P(25, 8), P(25, 20), P(14, 26), P(3, 20), P(3, 8), P(14, 2)};
	FSlateDrawElement::MakeLines(OutDrawElements, LayerId, Paint, Hex, ESlateDrawEffect::None, Frame * InWidgetStyle.GetColorAndOpacityTint(), true, Thick);
	// The locomotive front: cab outline, the window line and two wheels.
	TArray<FVector2f> Cab = {P(9, 18), P(9, 11), P(10, 9.3f), P(18, 9.3f), P(19, 11), P(19, 18)};
	FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 1, Paint, Cab, ESlateDrawEffect::None, Mark * InWidgetStyle.GetColorAndOpacityTint(), true, Thick * 0.9f);
	TArray<FVector2f> Window = {P(9, 14), P(19, 14)};
	FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 1, Paint, Window, ESlateDrawEffect::None, Mark * InWidgetStyle.GetColorAndOpacityTint(), true, Thick * 0.9f);
	for (const float X : {11.f, 17.f})
	{
		TArray<FVector2f> Wheel = {P(X - 1.2f, 20.5f), P(X + 1.2f, 20.5f)};
		FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 1, Paint, Wheel, ESlateDrawEffect::None, Mark * InWidgetStyle.GetColorAndOpacityTint(), true, Thick * 1.4f);
	}
	return LayerId + 2;
}

void USTBar::SetValue(float InValue)
{
	mTarget = FMath::Clamp(InValue, 0.f, 1.f);
}

void USTBar::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	mShown = Ease(mShown, mTarget, InDeltaTime);
	mTime += InDeltaTime;
}

int32 USTBar::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
	int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const FVector2f Size = AllottedGeometry.GetLocalSize();
	const float Radius = Size.Y * 0.5f;
	const FSlateBrush Back = STStyle::Rounded(STStyle::Inset, Radius);
	// MakeBox draws in the tint it is given, not the brush's colour.
	FSlateDrawElement::MakeBox(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(Size, FSlateLayoutTransform()), &Back, ESlateDrawEffect::None,
		STStyle::Inset * InWidgetStyle.GetColorAndOpacityTint());
	const float Width = Size.X * mShown;
	if (Width >= 1.f)
	{
		FLinearColor Fill = mFill;
		if (bPulse)
		{
			const float Glow = 0.5f + 0.5f * FMath::Sin(mTime * 2.f * PI / PulseSeconds);
			Fill = FLinearColor::LerpUsingHSV(mFill, FLinearColor::White, 0.25f * Glow);
		}
		const FSlateBrush Front = STStyle::Rounded(Fill, Radius);
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 1, AllottedGeometry.ToPaintGeometry(FVector2f(FMath::Max(Width, Size.Y), Size.Y), FSlateLayoutTransform()),
			&Front, ESlateDrawEffect::None, Fill * InWidgetStyle.GetColorAndOpacityTint());
	}
	return LayerId + 2;
}

void USTRing::SetValue(float InValue)
{
	mTarget = FMath::Clamp(InValue, 0.f, 1.f);
}

void USTRing::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	mShown = Ease(mShown, mTarget, InDeltaTime);
}

int32 USTRing::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
	int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const FVector2f Size = AllottedGeometry.GetLocalSize();
	const float S = FMath::Min(Size.X, Size.Y);
	const float Thick = FMath::Max(3.f, S * 0.11f);
	const FVector2f Centre = Size * 0.5f;
	const float Radius = S * 0.5f - Thick;
	auto Arc = [&](float From, float To, int32 Layer, const FLinearColor& Color)
	{
		TArray<FVector2f> Points;
		const int32 Steps = FMath::Max(2, FMath::CeilToInt((To - From) * 48.f));
		for (int32 Step = 0; Step <= Steps; ++Step)
		{
			const float Angle = (From + (To - From) * Step / Steps) * 2.f * PI - 0.5f * PI;
			Points.Add(Centre + FVector2f(FMath::Cos(Angle), FMath::Sin(Angle)) * Radius);
		}
		FSlateDrawElement::MakeLines(OutDrawElements, Layer, AllottedGeometry.ToPaintGeometry(), Points, ESlateDrawEffect::None, Color * InWidgetStyle.GetColorAndOpacityTint(), true, Thick);
	};
	Arc(0.f, 1.f, LayerId, STStyle::Line);
	if (mShown > 0.002f)
	{
		Arc(0.f, mShown, LayerId + 1, mFill);
	}
	return LayerId + 2;
}
