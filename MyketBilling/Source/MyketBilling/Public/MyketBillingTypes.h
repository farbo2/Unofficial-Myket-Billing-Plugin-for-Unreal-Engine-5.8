#pragma once

#include "CoreMinimal.h"
#include "MyketBillingTypes.generated.h"

/** Mirrors Myket's two product families: one-shot items and subscriptions. */
UENUM(BlueprintType)
enum class EMyketProductType : uint8
{
	InApp        UMETA(DisplayName = "In-App (Consumable / Non-Consumable)"),
	Subscription UMETA(DisplayName = "Subscription")
};

/** Connection lifecycle state, exposed to Blueprint so UI can disable buttons appropriately. */
UENUM(BlueprintType)
enum class EMyketConnectionState : uint8
{
	Disconnected UMETA(DisplayName = "Disconnected"),
	Connecting   UMETA(DisplayName = "Connecting"),
	Connected    UMETA(DisplayName = "Connected")
};

/** A single catalog entry returned by QueryInventory when bQuerySkuDetails is true. */
USTRUCT(BlueprintType)
struct MYKETBILLING_API FMyketProduct
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Myket")
	FString Sku;

	UPROPERTY(BlueprintReadOnly, Category = "Myket")
	FString Title;

	UPROPERTY(BlueprintReadOnly, Category = "Myket")
	FString Description;

	/** Localized, formatted price string as configured in the Myket developer panel. */
	UPROPERTY(BlueprintReadOnly, Category = "Myket")
	FString Price;

	UPROPERTY(BlueprintReadOnly, Category = "Myket")
	FString Type;
};

/** A purchase record, either freshly bought or returned from QueryInventory. */
USTRUCT(BlueprintType)
struct MYKETBILLING_API FMyketPurchase
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Myket")
	FString Sku;

	UPROPERTY(BlueprintReadOnly, Category = "Myket")
	FString OrderId;

	UPROPERTY(BlueprintReadOnly, Category = "Myket")
	FString ItemType;

	UPROPERTY(BlueprintReadOnly, Category = "Myket")
	FString DeveloperPayload;

	/** Raw JSON payload from Myket. Needed if you verify the signature on your own server. */
	UPROPERTY(BlueprintReadOnly, Category = "Myket")
	FString OriginalJson;

	/** Signature over OriginalJson. Send both to your backend to verify against your public key. */
	UPROPERTY(BlueprintReadOnly, Category = "Myket")
	FString Signature;

	UPROPERTY(BlueprintReadOnly, Category = "Myket")
	FString Token;

	UPROPERTY(BlueprintReadOnly, Category = "Myket")
	int64 PurchaseTime = 0;
};
