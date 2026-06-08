#pragma once

#include "CoreMinimal.h"
#include "Layout/SlateRect.h"

class FJsonObject;

class FIetaMCPStatusWindow
{
public:
    static void ShowWithParams(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);
    static void Show(const FString& CommandType);
    static void UpdateStatus(const FString& StatusText, const FString& CommandType);
    static void Hide();
    static FString BuildEditorLogStatusText();

private:
    static bool IsIetaStatusCommand(const FString& CommandType);
    static FText BuildProcessingBodyText(const FString& CommandType);
    static void SetProcessingStatus(const FString& CommandType);
    static FVector2D GetFinalWindowPosition();
    static void PositionWindowAtBottomRight(const TSharedRef<class SWindow>& Window);
    static void AddStatusWindow(const TSharedRef<class SWindow>& Window);
    static void PresentStatusWindow(const TSharedRef<class SWindow>& Window);
    static FSlateRect GetTargetWorkArea();
    static TSharedPtr<class SWindow> GetTargetParentWindow();
    static void CloseStatusWindow(uint64 RequestedGeneration);
    static void FlushSlateWindowNow(const TSharedRef<class SWindow>& Window);
    static void ResetWindowReferences();
    static void OnStatusWindowClosed(const TSharedRef<class SWindow>& ClosedWindow);

    static constexpr double CompletionVisibleSeconds = 5.0;
    static constexpr float WindowWidth = 520.0f;
    static constexpr float WindowHeight = 170.0f;
    static constexpr float ScreenMargin = 24.0f;

    static double LastShowTime;
    static TSharedPtr<class SWindow> StatusWindow;
    static TSharedPtr<class STextBlock> BodyTextBlock;
    static TSharedPtr<struct FSlateBrush> AvatarBrush;
    static class UTexture2D* AvatarTexture;
    static uint64 CloseRequestGeneration;
};
