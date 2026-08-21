#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "MyketBillingTypes.h"
#include "MyketTestWidget.generated.h"

class UButton;
class UEditableTextBox;
class UMultiLineEditableTextBox;
class UMyketBillingSubsystem;

/**
 * Drop-in on-device test screen for Myket billing.
 *
 * Create a Widget Blueprint with this as its Parent Class (e.g. WBP_MyketTest),
 * then in the UMG Designer add widgets with EXACTLY these names (Details panel > Name):
 *
 *   ConnectButton        (Button)
 *   QueryInventoryButton (Button)
 *   BuyButton            (Button)
 *   ConsumeLastButton    (Button)
 *   SkuTextBox           (Editable Text Box)      <- SKU to buy, e.g. "gold_100"
 *   PublicKeyTextBox     (Editable Text Box)      <- your Base64 public key from the Myket panel
 *   LogTextBox           (Multi-Line Editable Text Box, Is Read Only = true)
 *
 * No Blueprint graph wiring is required - clicks, subsystem events, and logging
 * are all handled here in C++. Set default test SKUs on the widget's Class
 * Defaults if you don't want to type them by hand on the phone.
 */
UCLASS()
class MYKETBILLING_API UMyketTestWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Comma-separated in-app (non-subscription) SKUs to query, e.g. "gold_100,remove_ads" */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Myket|Test")
	FString DefaultInAppSkusCsv = TEXT("gold_100,remove_ads");

	/** Comma-separated subscription SKUs to query. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Myket|Test")
	FString DefaultSubscriptionSkusCsv;

protected:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UButton> ConnectButton;

	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UButton> QueryInventoryButton;

	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UButton> BuyButton;

	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UButton> ConsumeLastButton;

	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UEditableTextBox> SkuTextBox;

	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UEditableTextBox> PublicKeyTextBox;

	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UMultiLineEditableTextBox> LogTextBox;

private:
	UFUNCTION() void OnConnectClicked();
	UFUNCTION() void OnQueryInventoryClicked();
	UFUNCTION() void OnBuyClicked();
	UFUNCTION() void OnConsumeLastClicked();

	UFUNCTION() void HandleConnectionSucceeded();
	UFUNCTION() void HandleConnectionFailed(const FString& ErrorMessage);
	UFUNCTION() void HandleQueryInventoryFinished(bool bSuccess, const FString& Message, const TArray<FMyketProduct>& Products, const TArray<FMyketPurchase>& Purchases);
	UFUNCTION() void HandlePurchaseFinished(bool bSuccess, const FString& Message, const FMyketPurchase& Purchase);
	UFUNCTION() void HandleConsumeFinished(bool bSuccess, const FString& Message, const FMyketPurchase& Purchase);

	UMyketBillingSubsystem* GetBilling() const;
	void AppendLog(const FString& Line);
	static TArray<FString> SplitCsv(const FString& Csv);

	FMyketPurchase LastPurchase;
	FString LogBuffer;
};
