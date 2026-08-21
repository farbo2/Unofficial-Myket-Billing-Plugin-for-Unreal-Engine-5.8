#include "MyketTestWidget.h"
#include "MyketBillingSubsystem.h"
#include "Components/Button.h"
#include "Components/EditableTextBox.h"
#include "Components/MultiLineEditableTextBox.h"
#include "Kismet/GameplayStatics.h"

void UMyketTestWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (SkuTextBox && SkuTextBox->GetText().IsEmpty())
	{
		TArray<FString> Skus = SplitCsv(DefaultInAppSkusCsv);
		if (Skus.Num() > 0)
		{
			SkuTextBox->SetText(FText::FromString(Skus[0]));
		}
	}

	if (ConnectButton)        ConnectButton->OnClicked.AddDynamic(this, &UMyketTestWidget::OnConnectClicked);
	if (QueryInventoryButton) QueryInventoryButton->OnClicked.AddDynamic(this, &UMyketTestWidget::OnQueryInventoryClicked);
	if (BuyButton)            BuyButton->OnClicked.AddDynamic(this, &UMyketTestWidget::OnBuyClicked);
	if (ConsumeLastButton)    ConsumeLastButton->OnClicked.AddDynamic(this, &UMyketTestWidget::OnConsumeLastClicked);

	if (UMyketBillingSubsystem* Billing = GetBilling())
	{
		Billing->OnConnectionSucceeded.AddDynamic(this, &UMyketTestWidget::HandleConnectionSucceeded);
		Billing->OnConnectionFailed.AddDynamic(this, &UMyketTestWidget::HandleConnectionFailed);
		Billing->OnQueryInventoryFinished.AddDynamic(this, &UMyketTestWidget::HandleQueryInventoryFinished);
		Billing->OnPurchaseFinished.AddDynamic(this, &UMyketTestWidget::HandlePurchaseFinished);
		Billing->OnConsumeFinished.AddDynamic(this, &UMyketTestWidget::HandleConsumeFinished);

		AppendLog(Billing->IsPlatformSupported()
			? TEXT("Ready. Enter your public key and tap Connect.")
			: TEXT("WARNING: running on a non-Android platform - calls will report failure by design."));
	}
	else
	{
		AppendLog(TEXT("ERROR: could not get MyketBillingSubsystem from GameInstance."));
	}
}

void UMyketTestWidget::NativeDestruct()
{
	if (UMyketBillingSubsystem* Billing = GetBilling())
	{
		Billing->OnConnectionSucceeded.RemoveAll(this);
		Billing->OnConnectionFailed.RemoveAll(this);
		Billing->OnQueryInventoryFinished.RemoveAll(this);
		Billing->OnPurchaseFinished.RemoveAll(this);
		Billing->OnConsumeFinished.RemoveAll(this);
	}
	Super::NativeDestruct();
}

UMyketBillingSubsystem* UMyketTestWidget::GetBilling() const
{
	if (UGameInstance* GI = UGameplayStatics::GetGameInstance(this))
	{
		return GI->GetSubsystem<UMyketBillingSubsystem>();
	}
	return nullptr;
}

// ---------------------------------------------------------------------
// Button handlers
// ---------------------------------------------------------------------

void UMyketTestWidget::OnConnectClicked()
{
	UMyketBillingSubsystem* Billing = GetBilling();
	if (!Billing) return;

	const FString PublicKey = PublicKeyTextBox ? PublicKeyTextBox->GetText().ToString() : FString();
	if (PublicKey.IsEmpty())
	{
		AppendLog(TEXT("Enter your public key first."));
		return;
	}

	AppendLog(TEXT("Connecting..."));
	Billing->OpenConnection(PublicKey);
}

void UMyketTestWidget::OnQueryInventoryClicked()
{
	UMyketBillingSubsystem* Billing = GetBilling();
	if (!Billing) return;

	AppendLog(TEXT("Querying inventory..."));
	Billing->QueryInventory(true, SplitCsv(DefaultInAppSkusCsv), SplitCsv(DefaultSubscriptionSkusCsv));
}

void UMyketTestWidget::OnBuyClicked()
{
	UMyketBillingSubsystem* Billing = GetBilling();
	if (!Billing) return;

	const FString Sku = SkuTextBox ? SkuTextBox->GetText().ToString() : FString();
	if (Sku.IsEmpty())
	{
		AppendLog(TEXT("Enter a SKU to buy first."));
		return;
	}

	AppendLog(FString::Printf(TEXT("Launching purchase flow for '%s'..."), *Sku));
	Billing->LaunchPurchaseFlow(Sku, TEXT("test-payload"));
}

void UMyketTestWidget::OnConsumeLastClicked()
{
	UMyketBillingSubsystem* Billing = GetBilling();
	if (!Billing) return;

	if (LastPurchase.Sku.IsEmpty())
	{
		AppendLog(TEXT("No purchase to consume yet - buy or query inventory first."));
		return;
	}

	AppendLog(FString::Printf(TEXT("Consuming '%s'..."), *LastPurchase.Sku));
	Billing->ConsumePurchase(LastPurchase);
}

// ---------------------------------------------------------------------
// Subsystem event handlers
// ---------------------------------------------------------------------

void UMyketTestWidget::HandleConnectionSucceeded()
{
	AppendLog(TEXT("Connected successfully."));
}

void UMyketTestWidget::HandleConnectionFailed(const FString& ErrorMessage)
{
	AppendLog(FString::Printf(TEXT("Connection FAILED: %s"), *ErrorMessage));
}

void UMyketTestWidget::HandleQueryInventoryFinished(bool bSuccess, const FString& Message, const TArray<FMyketProduct>& Products, const TArray<FMyketPurchase>& Purchases)
{
	AppendLog(FString::Printf(TEXT("Query inventory %s: %s"), bSuccess ? TEXT("OK") : TEXT("FAILED"), *Message));

	for (const FMyketProduct& P : Products)
	{
		AppendLog(FString::Printf(TEXT("  [Product] %s - %s (%s)"), *P.Sku, *P.Price, *P.Title));
	}
	for (const FMyketPurchase& Pu : Purchases)
	{
		AppendLog(FString::Printf(TEXT("  [Owned] %s (order %s)"), *Pu.Sku, *Pu.OrderId));
		LastPurchase = Pu;
	}
}

void UMyketTestWidget::HandlePurchaseFinished(bool bSuccess, const FString& Message, const FMyketPurchase& Purchase)
{
	AppendLog(FString::Printf(TEXT("Purchase %s: %s"), bSuccess ? TEXT("OK") : TEXT("FAILED"), *Message));
	if (bSuccess)
	{
		AppendLog(FString::Printf(TEXT("  Bought '%s', order %s"), *Purchase.Sku, *Purchase.OrderId));
		LastPurchase = Purchase;
	}
}

void UMyketTestWidget::HandleConsumeFinished(bool bSuccess, const FString& Message, const FMyketPurchase& Purchase)
{
	AppendLog(FString::Printf(TEXT("Consume %s: %s"), bSuccess ? TEXT("OK") : TEXT("FAILED"), *Message));
	if (bSuccess)
	{
		LastPurchase = FMyketPurchase();
	}
}

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------

void UMyketTestWidget::AppendLog(const FString& Line)
{
	const FString Timestamp = FDateTime::Now().ToString(TEXT("%H:%M:%S"));
	LogBuffer = FString::Printf(TEXT("[%s] %s\n%s"), *Timestamp, *Line, *LogBuffer);

	if (LogTextBox)
	{
		LogTextBox->SetText(FText::FromString(LogBuffer));
	}
	UE_LOG(LogTemp, Log, TEXT("[MyketTest] %s"), *Line);
}

TArray<FString> UMyketTestWidget::SplitCsv(const FString& Csv)
{
	TArray<FString> Result;
	if (Csv.IsEmpty()) return Result;

	TArray<FString> Raw;
	Csv.ParseIntoArray(Raw, TEXT(","), true);
	for (FString& S : Raw)
	{
		S.TrimStartAndEndInline();
		if (!S.IsEmpty())
		{
			Result.Add(S);
		}
	}
	return Result;
}
