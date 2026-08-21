#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "MyketBillingTypes.h"
#include "MyketBillingSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FMyketOnConnectionSucceeded);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FMyketOnConnectionFailed, const FString&, ErrorMessage);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FMyketOnQueryInventoryFinished,
	bool, bSuccess,
	const FString&, Message,
	const TArray<FMyketProduct>&, Products,
	const TArray<FMyketPurchase>&, Purchases);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FMyketOnPurchaseFinished,
	bool, bSuccess,
	const FString&, Message,
	const FMyketPurchase&, Purchase);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FMyketOnConsumeFinished,
	bool, bSuccess,
	const FString&, Message,
	const FMyketPurchase&, Purchase);

/**
 * Blueprint entry point for Myket in-app billing.
 *
 * Usage from Blueprint:
 *   1. Get Game Instance Subsystem (Myket Billing Subsystem)
 *   2. Bind to OnConnectionSucceeded / OnConnectionFailed
 *   3. Call Open Connection with your Base64 public key from the Myket developer panel
 *   4. Once connected, call Query Inventory, then Launch Purchase Flow / Consume Purchase as needed
 *
 * On non-Android platforms every call immediately fails through the corresponding
 * delegate with an explanatory message, so Blueprint graphs remain testable in the editor.
 */
UCLASS(BlueprintType)
class MYKETBILLING_API UMyketBillingSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Must be called once, before any other Myket call. */
	UFUNCTION(BlueprintCallable, Category = "Myket|Billing")
	void OpenConnection(const FString& PublicKey);

	/**
	 * Refreshes unconsumed purchases and, optionally, catalog details (title/price/description).
	 * Call this right after OnConnectionSucceeded, every app launch, before showing any store UI.
	 */
	UFUNCTION(BlueprintCallable, Category = "Myket|Billing")
	void QueryInventory(bool bQuerySkuDetails, const TArray<FString>& InAppSkus, const TArray<FString>& SubscriptionSkus);

	/** Opens the Myket purchase UI for a single SKU (works for in-app items and subscriptions alike). */
	UFUNCTION(BlueprintCallable, Category = "Myket|Billing")
	void LaunchPurchaseFlow(const FString& Sku, const FString& DeveloperPayload);

	/** Consumable products only: call after delivering the item so it can be bought again. */
	UFUNCTION(BlueprintCallable, Category = "Myket|Billing")
	void ConsumePurchase(const FMyketPurchase& Purchase);

	/** Call when you're done with billing (e.g. game shutdown). */
	UFUNCTION(BlueprintCallable, Category = "Myket|Billing")
	void CloseConnection();

	UFUNCTION(BlueprintPure, Category = "Myket|Billing")
	bool IsPlatformSupported() const;

	/** Current connection lifecycle state. Blueprint should disable buttons appropriately based on this. */
	UFUNCTION(BlueprintPure, Category = "Myket|Billing")
	EMyketConnectionState GetConnectionState() const { return ConnectionState; }

	UFUNCTION(BlueprintPure, Category = "Myket|Billing")
	bool IsConnected() const { return ConnectionState == EMyketConnectionState::Connected; }

	/** True while a purchase flow is in progress. Disable the purchase button while true. */
	UFUNCTION(BlueprintPure, Category = "Myket|Billing")
	bool IsPurchaseInFlight() const { return bIsPurchaseInFlight; }

	/** True while a QueryInventory call is in progress. */
	UFUNCTION(BlueprintPure, Category = "Myket|Billing")
	bool IsQueryInFlight() const { return bIsQueryInFlight; }

	/** True while a ConsumePurchase call is in progress. */
	UFUNCTION(BlueprintPure, Category = "Myket|Billing")
	bool IsConsumeInFlight() const { return bIsConsumeInFlight; }

	/**
	 * True while ANY of Query/Purchase/Consume is in progress.
	 *
	 * Myket's IabHelper (see ir.myket.billingclient.util.IAB.flagStartAsync /
	 * isAsyncOperationInProgress, shared by ServiceIAB.launchPurchaseFlow,
	 * queryInventoryAsync and consumeAsync alike) only allows ONE async
	 * operation in flight per connection, no matter which of the three it is -
	 * starting a second one throws IllegalStateException inside the SDK.
	 * QueryInventory/LaunchPurchaseFlow/ConsumePurchase below all check this
	 * (not just their own flag) before dispatching, so Blueprint should
	 * generally gate all three buttons on this rather than assuming they're
	 * independent.
	 */
	UFUNCTION(BlueprintPure, Category = "Myket|Billing")
	bool IsAnyOperationInFlight() const { return bIsPurchaseInFlight || bIsQueryInFlight || bIsConsumeInFlight; }

	/**
	 * Manual recovery: force-clears the purchase/query/consume in-flight flags
	 * without touching the connection. Use this if a button is stuck disabled
	 * (e.g. IsPurchaseInFlight staying true) with no matching Finished delegate
	 * ever arriving - this can happen if the OS reclaims the app process
	 * mid-flow, or in rare cases where the underlying SDK's callback never
	 * fires.
	 *
	 * Safety note: this also advances the internal purchase/query/consume
	 * generation counters (whichever was actually in flight), so if the native
	 * layer's callback for that abandoned attempt does eventually arrive, it
	 * will be recognized as stale and silently dropped instead of corrupting
	 * the state of whatever you start next. It still does not cancel an
	 * in-progress native purchase - Myket's UI may still be showing and may
	 * still complete the transaction on the user's account; this only
	 * unblocks the Blueprint-side flag.
	 */
	UFUNCTION(BlueprintCallable, Category = "Myket|Billing")
	void ResetStuckFlags();

	UPROPERTY(BlueprintAssignable, Category = "Myket|Billing|Events")
	FMyketOnConnectionSucceeded OnConnectionSucceeded;

	UPROPERTY(BlueprintAssignable, Category = "Myket|Billing|Events")
	FMyketOnConnectionFailed OnConnectionFailed;

	UPROPERTY(BlueprintAssignable, Category = "Myket|Billing|Events")
	FMyketOnQueryInventoryFinished OnQueryInventoryFinished;

	UPROPERTY(BlueprintAssignable, Category = "Myket|Billing|Events")
	FMyketOnPurchaseFinished OnPurchaseFinished;

	UPROPERTY(BlueprintAssignable, Category = "Myket|Billing|Events")
	FMyketOnConsumeFinished OnConsumeFinished;

	/** Retrieves the live subsystem instance from anywhere, including the JNI glue layer. */
	static UMyketBillingSubsystem* Get();

	// --- Called on the game thread by the platform glue layer. Not for Blueprint use.
	// Each takes the "generation" it was issued for; if it no longer matches the
	// subsystem's current generation counter (a newer OpenConnection/Launch*/Close
	// happened since), the callback is stale and is dropped without touching state
	// or broadcasting - this is what stops a late-arriving result from a previous
	// connection/purchase/query/consume attempt from corrupting whatever is happening now. ---
	void HandleConnectionSucceeded(int32 Generation);
	void HandleConnectionFailed(const FString& Message, int32 Generation);
	void HandleQueryInventoryFinished(bool bSuccess, const FString& Message, const FString& ProductsJson, const FString& PurchasesJson, int32 Generation);
	void HandlePurchaseFinished(bool bSuccess, const FString& Message, const FString& PurchaseJson, int32 Generation);
	void HandleConsumeFinished(bool bSuccess, const FString& Message, const FString& PurchaseJson, int32 Generation);

private:
	static TWeakObjectPtr<UMyketBillingSubsystem> Instance;

	EMyketConnectionState ConnectionState = EMyketConnectionState::Disconnected;
	bool bIsPurchaseInFlight = false;
	bool bIsQueryInFlight = false;
	bool bIsConsumeInFlight = false;

	// Bumped on every OpenConnection/CloseConnection - invalidates in-flight
	// connect attempts and, transitively, anything gated on being connected.
	int32 ConnectionGeneration = 0;
	// Bumped on every LaunchPurchaseFlow/ResetStuckFlags(if a purchase was in
	// flight)/CloseConnection - invalidates a specific purchase attempt.
	int32 PurchaseGeneration = 0;
	// Bumped on every QueryInventory/ResetStuckFlags(if a query was in
	// flight)/CloseConnection.
	int32 QueryGeneration = 0;
	// Bumped on every ConsumePurchase/ResetStuckFlags(if a consume was in
	// flight)/CloseConnection. Deliberately its own counter, not a reuse of
	// ConnectionGeneration - a consume attempt needs to be invalidated
	// independently of the connection it started under (e.g. a second
	// ConsumePurchase call under the same still-open connection).
	int32 ConsumeGeneration = 0;

	static TArray<FMyketProduct> ParseProductsJson(const FString& Json);
	static TArray<FMyketPurchase> ParsePurchasesJson(const FString& Json);
	static FMyketPurchase ParsePurchaseJson(const FString& Json);
};
