#include "UI/IetaMCPStatusWindow.h"

#include "Containers/Ticker.h"
#include "Engine/Texture2D.h"
#include "Framework/Application/SlateApplication.h"
#include "ImageUtils.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateBrush.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

void FIetaMCPStatusWindow::ShowWithParams(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    (void)Params;
    Show(CommandType);
}

void FIetaMCPStatusWindow::Show(const FString& CommandType)
{
    if (!IsIetaStatusCommand(CommandType) || !FSlateApplication::IsInitialized())
    {
        return;
    }

    ++CloseRequestGeneration;

    if (StatusWindow.IsValid())
    {
        const TSharedPtr<SWindow> ExistingWindow = StatusWindow;
        LastShowTime = FPlatformTime::Seconds();
        SetProcessingStatus(CommandType);
        PresentStatusWindow(ExistingWindow.ToSharedRef());
        return;
    }

    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealMCP"));
    const FString ImagePath = Plugin.IsValid()
        ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("Ieta.png"))
        : FString();

    if (!AvatarTexture && FPaths::FileExists(ImagePath))
    {
        AvatarTexture = FImageUtils::ImportFileAsTexture2D(ImagePath);
        if (AvatarTexture)
        {
            AvatarTexture->AddToRoot();
            AvatarTexture->SRGB = true;
        }
    }

    if (AvatarTexture)
    {
        AvatarBrush = MakeShared<FSlateBrush>();
        AvatarBrush->SetResourceObject(AvatarTexture);
        AvatarBrush->ImageSize = FVector2D(64.0f, 64.0f);
        AvatarBrush->DrawAs = ESlateBrushDrawType::Image;
    }
    else
    {
        AvatarBrush.Reset();
    }

    const FText TitleText = FText::FromString(TEXT("이에타 상태"));
    const FVector2D FinalPosition = GetFinalWindowPosition();

    TSharedRef<SWindow> Window = SNew(SWindow)
        .Title(TitleText)
        .AutoCenter(EAutoCenter::None)
        .ScreenPosition(FinalPosition)
        .ClientSize(FVector2D(WindowWidth, WindowHeight))
        .SizingRule(ESizingRule::FixedSize)
        .SupportsMaximize(false)
        .SupportsMinimize(false)
        .HasCloseButton(true)
        .IsTopmostWindow(true)
        .FocusWhenFirstShown(false)
        [
            SNew(SBorder)
            .Padding(14.0f)
            .BorderBackgroundColor(FLinearColor(0.035f, 0.035f, 0.042f, 0.96f))
            [
                SNew(SHorizontalBox)

                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                [
                    SNew(SBox)
                    .WidthOverride(78.0f)
                    .HeightOverride(78.0f)
                    [
                        SNew(SImage)
                        .Image(AvatarBrush.IsValid() ? AvatarBrush.Get() : FCoreStyle::Get().GetDefaultBrush())
                    ]
                ]

                + SHorizontalBox::Slot()
                .FillWidth(1.0f)
                .Padding(14.0f, 0.0f, 0.0f, 0.0f)
                .VAlign(VAlign_Center)
                [
                    SNew(SVerticalBox)

                    + SVerticalBox::Slot()
                    .AutoHeight()
                    [
                        SNew(STextBlock)
                        .Text(TitleText)
                        .ColorAndOpacity(FLinearColor(1.0f, 0.86f, 0.48f, 1.0f))
                        .Font(FCoreStyle::GetDefaultFontStyle("Bold", 16))
                    ]

                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .Padding(0.0f, 6.0f, 0.0f, 0.0f)
                    [
                        SAssignNew(BodyTextBlock, STextBlock)
                        .Text(BuildProcessingBodyText(CommandType))
                        .AutoWrapText(true)
                        .ColorAndOpacity(FLinearColor(0.93f, 0.93f, 0.96f, 1.0f))
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 12))
                    ]
                ]
            ]
        ];

    Window->SetOnWindowClosed(FOnWindowClosed::CreateStatic(&FIetaMCPStatusWindow::OnStatusWindowClosed));
    StatusWindow = Window;
    LastShowTime = FPlatformTime::Seconds();
    AddStatusWindow(Window);
    SetProcessingStatus(CommandType);
    PresentStatusWindow(Window);
}

void FIetaMCPStatusWindow::UpdateStatus(const FString& StatusText, const FString& CommandType)
{
    const FString SlateCommand = CommandType.IsEmpty() ? TEXT("ieta_status") : CommandType;
    if (!IsIetaStatusCommand(SlateCommand) || !FSlateApplication::IsInitialized())
    {
        return;
    }

    if (!StatusWindow.IsValid())
    {
        Show(SlateCommand);
    }

    if (BodyTextBlock.IsValid())
    {
        BodyTextBlock->SetText(FText::FromString(FString::Printf(
            TEXT("%s\n명령: %s"),
            *StatusText,
            *SlateCommand)));
    }

    LastShowTime = FPlatformTime::Seconds();
    if (StatusWindow.IsValid())
    {
        PresentStatusWindow(StatusWindow.ToSharedRef());
    }
}

void FIetaMCPStatusWindow::Hide()
{
    if (!FSlateApplication::IsInitialized())
    {
        ResetWindowReferences();
        return;
    }

    if (StatusWindow.IsValid())
    {
        const double RemainingSeconds = CompletionVisibleSeconds - (FPlatformTime::Seconds() - LastShowTime);
        const uint64 RequestedGeneration = CloseRequestGeneration;
        if (RemainingSeconds > 0.0)
        {
            FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([RequestedGeneration](float)
            {
                if (CloseRequestGeneration != RequestedGeneration)
                {
                    return false;
                }

                CloseStatusWindow(RequestedGeneration);
                return false;
            }), static_cast<float>(RemainingSeconds));
            return;
        }

        CloseStatusWindow(RequestedGeneration);
        return;
    }

    ResetWindowReferences();
}

FString FIetaMCPStatusWindow::BuildEditorLogStatusText()
{
    const FString EditorLogPath = FPaths::Combine(FPaths::ProjectLogDir(), FString::Printf(TEXT("%s.log"), FApp::GetProjectName()));
    TArray<FString> Lines;
    if (!FPaths::FileExists(EditorLogPath) || !FFileHelper::LoadFileToStringArray(Lines, *EditorLogPath))
    {
        return FString();
    }

    int32 ErrorLineCount = 0;
    for (const FString& Line : Lines)
    {
        if (Line.Contains(TEXT("Error:"), ESearchCase::CaseSensitive))
        {
            ++ErrorLineCount;
        }
    }

    if (ErrorLineCount > 0)
    {
        return FString::Printf(TEXT("로그 오류가 %d줄 보여. 확인해둬."), ErrorLineCount);
    }

    return TEXT("로그 오류는 안 보여. 작업해도 돼.");
}

bool FIetaMCPStatusWindow::IsIetaStatusCommand(const FString& CommandType)
{
    return CommandType == TEXT("ieta_status");
}

FText FIetaMCPStatusWindow::BuildProcessingBodyText(const FString& CommandType)
{
    const FString EffectiveCommand = CommandType.IsEmpty() ? TEXT("ieta_status") : CommandType;
    return FText::FromString(FString::Printf(
        TEXT("확인 중\n명령: %s"),
        *EffectiveCommand));
}

void FIetaMCPStatusWindow::SetProcessingStatus(const FString& CommandType)
{
    if (BodyTextBlock.IsValid())
    {
        BodyTextBlock->SetText(BuildProcessingBodyText(CommandType));
    }
}

FVector2D FIetaMCPStatusWindow::GetFinalWindowPosition()
{
    const FSlateRect WorkArea = GetTargetWorkArea();
    return FVector2D(
        FMath::Max(WorkArea.Left + ScreenMargin, WorkArea.Right - WindowWidth - ScreenMargin),
        FMath::Max(WorkArea.Top + ScreenMargin, WorkArea.Bottom - WindowHeight - ScreenMargin));
}

void FIetaMCPStatusWindow::PositionWindowAtBottomRight(const TSharedRef<SWindow>& Window)
{
    const FSlateRect WorkArea = GetTargetWorkArea();
    const FSlateRect WindowRect = Window->GetRectInScreen();
    FVector2D WindowSize = Window->GetSizeInScreen();
    if (WindowRect.IsValid() && !WindowRect.IsEmpty())
    {
        WindowSize = FVector2D(WindowRect.Right - WindowRect.Left, WindowRect.Bottom - WindowRect.Top);
    }

    if (WindowSize.X <= 0.0f || WindowSize.Y <= 0.0f)
    {
        WindowSize = FVector2D(WindowWidth, WindowHeight);
    }

    const FVector2D FinalPosition(
        FMath::Max(WorkArea.Left + ScreenMargin, WorkArea.Right - WindowSize.X - ScreenMargin),
        FMath::Max(WorkArea.Top + ScreenMargin, WorkArea.Bottom - WindowSize.Y - ScreenMargin));
    Window->MoveWindowTo(FinalPosition);
}

void FIetaMCPStatusWindow::AddStatusWindow(const TSharedRef<SWindow>& Window)
{
    FSlateApplication& SlateApplication = FSlateApplication::Get();
    SlateApplication.AddWindow(Window, false);
}

void FIetaMCPStatusWindow::PresentStatusWindow(const TSharedRef<SWindow>& Window)
{
    Window->ShowWindow();
    PositionWindowAtBottomRight(Window);
    Window->BringToFront();
    FlushSlateWindowNow(Window);
}

FSlateRect FIetaMCPStatusWindow::GetTargetWorkArea()
{
    const TSharedPtr<SWindow> ParentWindow = GetTargetParentWindow();
    if (ParentWindow.IsValid())
    {
        const FSlateRect ParentRect = ParentWindow->GetRectInScreen();
        if (ParentRect.IsValid() && !ParentRect.IsEmpty())
        {
            return FSlateApplication::Get().GetWorkArea(ParentRect);
        }
    }

    return FSlateApplication::Get().GetPreferredWorkArea();
}

TSharedPtr<SWindow> FIetaMCPStatusWindow::GetTargetParentWindow()
{
    TSharedPtr<SWindow> ParentWindow = FSlateApplication::Get().FindBestParentWindowForDialogs(nullptr);
    if (ParentWindow == StatusWindow)
    {
        ParentWindow.Reset();
    }

    return ParentWindow;
}

void FIetaMCPStatusWindow::CloseStatusWindow(uint64 RequestedGeneration)
{
    if (CloseRequestGeneration != RequestedGeneration)
    {
        return;
    }

    if (!StatusWindow.IsValid())
    {
        ResetWindowReferences();
        return;
    }

    StatusWindow->RequestDestroyWindow();
}

void FIetaMCPStatusWindow::FlushSlateWindowNow(const TSharedRef<SWindow>& Window)
{
    FSlateApplication& SlateApplication = FSlateApplication::Get();
    SlateApplication.PumpMessages();
    SlateApplication.ForceRedrawWindow(Window);
}

void FIetaMCPStatusWindow::ResetWindowReferences()
{
    StatusWindow.Reset();
    AvatarBrush.Reset();
    BodyTextBlock.Reset();
}

void FIetaMCPStatusWindow::OnStatusWindowClosed(const TSharedRef<SWindow>& ClosedWindow)
{
    if (StatusWindow.IsValid() && StatusWindow.Get() == &ClosedWindow.Get())
    {
        ResetWindowReferences();
    }
}

double FIetaMCPStatusWindow::LastShowTime = 0.0;
TSharedPtr<SWindow> FIetaMCPStatusWindow::StatusWindow;
TSharedPtr<STextBlock> FIetaMCPStatusWindow::BodyTextBlock;
TSharedPtr<FSlateBrush> FIetaMCPStatusWindow::AvatarBrush;
UTexture2D* FIetaMCPStatusWindow::AvatarTexture = nullptr;
uint64 FIetaMCPStatusWindow::CloseRequestGeneration = 0;
