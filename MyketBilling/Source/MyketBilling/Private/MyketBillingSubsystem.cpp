#include "MyketBillingSubsystem.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if PLATFORM_ANDROID
#include "MyketBillingJNI.h"
#endif

TWeakObjectPtr<UMyketBillingSubsystem> UMyketBillingSubsystem::Instance;

UMyketBillingSubsystem* UMyketBillingSubsystem::Get()
{
	return Instance.Get();
}

void UMyketBillingSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Instance = this;
	ConnectionState = EMyketConnectionState::Disconnected;
	bIsPurchaseInFlight = false;
	bIsQueryInFlight = false;
	bIsConsumeInFlight = false;
	ConnectionGeneration = 0;
	PurchaseGeneration = 0;
	QueryGeneration = 0;
	ConsumeGeneration = 0;

#if PLATFORM_ANDROID
	MyketJNI::RegisterNatives();
#endif
}

void UMyketBillingSubsystem::Deinitialize()
{
	// Make sure we don't leak a live native IabHelper if the GameInstance is
	// torn down (PIE stop, returning to main menu, etc.) while still connected.
	if (ConnectionState != EMyketConnectionState::Disconnected)
	{
		++ConnectionGeneration;
		++PurchaseGeneration;
		++QueryGeneration;
		++ConsumeGeneration;
#if PLATFORM_ANDROID
		MyketJNI::CloseConnection();
#endif
	}

	if (Instance.Get() == this)
	{
		Instance = nullptr;
	}
	Super::Deinitialize();
}

bool UMyketBillingSubsystem::IsPlatformSupported() const
{
#if PLATFORM_ANDROID
	return true;
#else
	return false;
#endif
}

static const FString GNotAndroidMessage = TEXT("Myket billing is only available on Android.");

void UMyketBillingSubsystem::OpenConnection(const FString& PublicKey)
{
	if (PublicKey.IsEmpty())
	{
		AsyncTask(ENamedThreads::GameThread, [this]() { OnConnectionFailed.Broadcast(TEXT("Public key is empty.")); });
		return;
	}

	// Deliberately NOT short-circuiting here based on ConnectionState, even
	// though "already Connected/Connecting -> no-op" might look like a safe
	// optimization. It isn't: ConnectionState is only updated asynchronously
	// (via HandleConnectionSucceeded/Failed, dispatched through AsyncTask from
	// a JNI callback), while Java's real Activity/IabHelper state changes
	// synchronously with Android's Activity lifecycle. If GameActivity is
	// destroyed and recreated, Java's onDestroy() -> setActivity() sequence
	// happens immediately and synchronously, but the corresponding
	// HandleConnectionFailed()/state update on the C++ side is only queued,
	// not yet processed. A Blueprint call to OpenConnection landing in that
	// window would see a stale ConnectionState == Connected and could
	// short-circuit to a false OnConnectionSucceeded without ever contacting
	// Java - meaning C++ thinks it's connected while Java has nothing behind
	// it. So every call here is always relayed to Java, which tears down
	// whatever it has and starts fresh (see MyketBillingBridge.openConnection)
	// and is the only thing allowed to declare success or failure for a given
	// generation.
	//
	// Because Java unconditionally dispose()s whatever IabHelper it had
	// before building a new one, any purchase/query that was in flight under
	// the PREVIOUS connection is now orphaned - its listener may still fire
	// asynchronously even though the helper backing it is gone. Without
	// bumping Purchase/QueryGeneration here too, a late callback from that
	// orphaned attempt would still match the current generation (OpenConnection
	// alone never touched them) and get accepted as if it belonged to the new
	// connection. Only bump - and only broadcast a failure - for whichever was
	// actually in flight, mirroring ResetStuckFlags's reasoning.
	if (bIsPurchaseInFlight)
	{
		bIsPurchaseInFlight = false;
		++PurchaseGeneration;
		AsyncTask(ENamedThreads::GameThread, [this]() { OnPurchaseFinished.Broadcast(false, TEXT("Reconnecting - previous purchase attempt abandoned."), FMyketPurchase()); });
	}
	if (bIsQueryInFlight)
	{
		bIsQueryInFlight = false;
		++QueryGeneration;
		AsyncTask(ENamedThreads::GameThread, [this]() { OnQueryInventoryFinished.Broadcast(false, TEXT("Reconnecting - previous query abandoned."), TArray<FMyketProduct>(), TArray<FMyketPurchase>()); });
	}
	if (bIsConsumeInFlight)
	{
		bIsConsumeInFlight = false;
		++ConsumeGeneration;
		AsyncTask(ENamedThreads::GameThread, [this]() { OnConsumeFinished.Broadcast(false, TEXT("Reconnecting - previous consume attempt abandoned."), FMyketPurchase()); });
	}

	ConnectionState = EMyketConnectionState::Connecting;
	const int32 ThisGeneration = ++ConnectionGeneration;

#if PLATFORM_ANDROID
	if (!MyketJNI::OpenConnection(PublicKey, ThisGeneration))
	{
		if (ThisGeneration == ConnectionGeneration)
		{
			ConnectionState = EMyketConnectionState::Disconnected;
		}
		AsyncTask(ENamedThreads::GameThread, [this]() { OnConnectionFailed.Broadcast(TEXT("Failed to dispatch to the Java layer (JNI unavailable).")); });
	}
#else
	ConnectionState = EMyketConnectionState::Disconnected;
	AsyncTask(ENamedThreads::GameThread, [this]() { OnConnectionFailed.Broadcast(GNotAndroidMessage); });
#endif
}

void UMyketBillingSubsystem::QueryInventory(bool bQuerySkuDetails, const TArray<FString>& InAppSkus, const TArray<FString>& SubscriptionSkus)
{
	if (ConnectionState != EMyketConnectionState::Connected)
	{
		AsyncTask(ENamedThreads::GameThread, [this]()
		{
			OnQueryInventoryFinished.Broadcast(false, TEXT("Not connected. Call Open Connection first and wait for On Connection Succeeded."), TArray<FMyketProduct>(), TArray<FMyketPurchase>());
		});
		return;
	}
	if (bIsQueryInFlight)
	{
		AsyncTask(ENamedThreads::GameThread, [this]()
		{
			OnQueryInventoryFinished.Broadcast(false, TEXT("A query is already in progress."), TArray<FMyketProduct>(), TArray<FMyketPurchase>());
		});
		return;
	}
	// Myket's IabHelper only allows ONE async operation in flight per
	// connection, period - not one-per-kind. Purchase and Consume share the
	// exact same single-flight guard inside the SDK as Query does, so a Query
	// started while either is still running would hit that guard and fail
	// inside Java with a generic SDK exception instead of this clear message.
	if (bIsPurchaseInFlight || bIsConsumeInFlight)
	{
		AsyncTask(ENamedThreads::GameThread, [this]()
		{
			OnQueryInventoryFinished.Broadcast(false, TEXT("Another Myket operation (purchase or consume) is already in progress. The SDK only allows one at a time - wait for it to finish first."), TArray<FMyketProduct>(), TArray<FMyketPurchase>());
		});
		return;
	}

	bIsQueryInFlight = true;
	const int32 ThisGeneration = ++QueryGeneration;

#if PLATFORM_ANDROID
	if (!MyketJNI::QueryInventory(bQuerySkuDetails, InAppSkus, SubscriptionSkus, ThisGeneration))
	{
		if (ThisGeneration == QueryGeneration) bIsQueryInFlight = false;
		AsyncTask(ENamedThreads::GameThread, [this]()
		{
			OnQueryInventoryFinished.Broadcast(false, TEXT("Failed to dispatch to the Java layer (JNI unavailable)."), TArray<FMyketProduct>(), TArray<FMyketPurchase>());
		});
	}
#else
	bIsQueryInFlight = false;
	AsyncTask(ENamedThreads::GameThread, [this]()
	{
		OnQueryInventoryFinished.Broadcast(false, GNotAndroidMessage, TArray<FMyketProduct>(), TArray<FMyketPurchase>());
	});
#endif
}

void UMyketBillingSubsystem::LaunchPurchaseFlow(const FString& Sku, const FString& DeveloperPayload)
{
	if (Sku.IsEmpty())
	{
		AsyncTask(ENamedThreads::GameThread, [this]() { OnPurchaseFinished.Broadcast(false, TEXT("SKU is empty."), FMyketPurchase()); });
		return;
	}
	if (ConnectionState != EMyketConnectionState::Connected)
	{
		AsyncTask(ENamedThreads::GameThread, [this]() { OnPurchaseFinished.Broadcast(false, TEXT("Not connected. Call Open Connection first and wait for On Connection Succeeded."), FMyketPurchase()); });
		return;
	}
	if (bIsPurchaseInFlight)
	{
		AsyncTask(ENamedThreads::GameThread, [this]() { OnPurchaseFinished.Broadcast(false, TEXT("A purchase is already in progress."), FMyketPurchase()); });
		return;
	}
	// Same single-flight SDK guard as QueryInventory above - Query and Consume
	// share it too, so block here with a clear message rather than letting
	// the launch fail deep inside Java with a generic SDK exception.
	if (bIsQueryInFlight || bIsConsumeInFlight)
	{
		AsyncTask(ENamedThreads::GameThread, [this]() { OnPurchaseFinished.Broadcast(false, TEXT("Another Myket operation (query or consume) is already in progress. The SDK only allows one at a time - wait for it to finish first."), FMyketPurchase()); });
		return;
	}

	bIsPurchaseInFlight = true;
	const int32 ThisGeneration = ++PurchaseGeneration;

#if PLATFORM_ANDROID
	if (!MyketJNI::LaunchPurchaseFlow(Sku, DeveloperPayload, ThisGeneration))
	{
		if (ThisGeneration == PurchaseGeneration) bIsPurchaseInFlight = false;
		AsyncTask(ENamedThreads::GameThread, [this]() { OnPurchaseFinished.Broadcast(false, TEXT("Failed to dispatch to the Java layer (JNI unavailable)."), FMyketPurchase()); });
	}
#else
	bIsPurchaseInFlight = false;
	AsyncTask(ENamedThreads::GameThread, [this]() { OnPurchaseFinished.Broadcast(false, GNotAndroidMessage, FMyketPurchase()); });
#endif
}

void UMyketBillingSubsystem::ConsumePurchase(const FMyketPurchase& Purchase)
{
	if (Purchase.OriginalJson.IsEmpty() || Purchase.Signature.IsEmpty())
	{
		AsyncTask(ENamedThreads::GameThread, [this]() { OnConsumeFinished.Broadcast(false, TEXT("Purchase is missing OriginalJson/Signature - pass the struct you got from On Purchase Finished or On Query Inventory Finished."), FMyketPurchase()); });
		return;
	}
	if (ConnectionState != EMyketConnectionState::Connected)
	{
		AsyncTask(ENamedThreads::GameThread, [this]() { OnConsumeFinished.Broadcast(false, TEXT("Not connected. Call Open Connection first."), FMyketPurchase()); });
		return;
	}
	if (bIsConsumeInFlight)
	{
		AsyncTask(ENamedThreads::GameThread, [this]() { OnConsumeFinished.Broadcast(false, TEXT("A consume is already in progress."), FMyketPurchase()); });
		return;
	}
	// Myket's IabHelper only allows ONE async operation in flight per
	// connection - Query and Purchase share the exact same single-flight
	// guard inside the SDK (ir.myket.billingclient.util.IAB.flagStartAsync /
	// isAsyncOperationInProgress, shared by ServiceIAB.launchPurchaseFlow,
	// queryInventoryAsync, and consumeAsync alike) as Consume does. Without
	// this check, calling Consume while a Query or Purchase is still running
	// would reach Java and fail there with a generic IllegalStateException
	// from the SDK instead of this clear message - and, depending on timing,
	// could instead be the one that knocks out whichever of Query/Purchase
	// was already in flight.
	if (bIsQueryInFlight || bIsPurchaseInFlight)
	{
		AsyncTask(ENamedThreads::GameThread, [this]() { OnConsumeFinished.Broadcast(false, TEXT("Another Myket operation (query or purchase) is already in progress. The SDK only allows one at a time - wait for it to finish first."), FMyketPurchase()); });
		return;
	}

	bIsConsumeInFlight = true;
	// Its own counter, not ConnectionGeneration: a consume attempt needs to
	// be invalidated independently of the connection it started under (e.g.
	// ResetStuckFlags or a second ConsumePurchase call under the same
	// still-open connection must not also invalidate an unrelated, still-live
	// connection).
	const int32 ThisGeneration = ++ConsumeGeneration;

#if PLATFORM_ANDROID
	if (!MyketJNI::ConsumePurchase(Purchase.ItemType, Purchase.OriginalJson, Purchase.Signature, ThisGeneration))
	{
		if (ThisGeneration == ConsumeGeneration) bIsConsumeInFlight = false;
		AsyncTask(ENamedThreads::GameThread, [this]() { OnConsumeFinished.Broadcast(false, TEXT("Failed to dispatch to the Java layer (JNI unavailable)."), FMyketPurchase()); });
	}
#else
	bIsConsumeInFlight = false;
	AsyncTask(ENamedThreads::GameThread, [this]() { OnConsumeFinished.Broadcast(false, GNotAndroidMessage, FMyketPurchase()); });
#endif
}

void UMyketBillingSubsystem::CloseConnection()
{
	ConnectionState = EMyketConnectionState::Disconnected;
	bIsPurchaseInFlight = false;
	bIsQueryInFlight = false;
	bIsConsumeInFlight = false;
	// Advancing every counter here means any connection/purchase/query/consume
	// callback still in flight from before this CloseConnection will carry
	// an old generation and get dropped by the Handle* methods below instead
	// of being applied to a connection that, from the Subsystem's point of
	// view, no longer exists.
	++ConnectionGeneration;
	++PurchaseGeneration;
	++QueryGeneration;
	++ConsumeGeneration;

#if PLATFORM_ANDROID
	MyketJNI::CloseConnection();
#endif
}

void UMyketBillingSubsystem::ResetStuckFlags()
{
	// Only advance the generation for whichever operation was actually stuck -
	// bumping both unconditionally would also silently drop the real result of
	// a legitimate, still-in-flight operation of the *other* kind (e.g. calling
	// this to recover a stuck purchase while a genuine query is still running).
	if (bIsPurchaseInFlight)
	{
		bIsPurchaseInFlight = false;
		++PurchaseGeneration;
	}
	if (bIsQueryInFlight)
	{
		bIsQueryInFlight = false;
		++QueryGeneration;
	}
	if (bIsConsumeInFlight)
	{
		bIsConsumeInFlight = false;
		++ConsumeGeneration;
	}
}

// ---------------------------------------------------------------------------
// Callbacks from the platform glue layer (already on the game thread).
// Every one of these first checks its Generation against the current
// counter and drops the callback (log + return, no state mutation, no
// delegate broadcast) if it's stale. This is what stops a late-arriving
// result from a superseded connection/purchase/query attempt from
// corrupting whatever is happening now.
// ---------------------------------------------------------------------------

void UMyketBillingSubsystem::HandleConnectionSucceeded(int32 Generation)
{
	if (Generation != ConnectionGeneration)
	{
		UE_LOG(LogTemp, Warning, TEXT("[MyketBilling] Dropping stale HandleConnectionSucceeded (gen %d, current %d)."), Generation, ConnectionGeneration);
		return;
	}
	ConnectionState = EMyketConnectionState::Connected;
	OnConnectionSucceeded.Broadcast();
}

void UMyketBillingSubsystem::HandleConnectionFailed(const FString& Message, int32 Generation)
{
	if (Generation != ConnectionGeneration)
	{
		UE_LOG(LogTemp, Warning, TEXT("[MyketBilling] Dropping stale HandleConnectionFailed (gen %d, current %d)."), Generation, ConnectionGeneration);
		return;
	}
	ConnectionState = EMyketConnectionState::Disconnected;
	bIsPurchaseInFlight = false;
	bIsQueryInFlight = false;
	bIsConsumeInFlight = false;
	// A purchase/query/consume that started under this connection is now
	// meaningless - bump these too so a late callback for any of them gets
	// dropped as stale instead of being applied after the connection it
	// belonged to is gone.
	++ConnectionGeneration;
	++PurchaseGeneration;
	++QueryGeneration;
	++ConsumeGeneration;
	OnConnectionFailed.Broadcast(Message);
}

void UMyketBillingSubsystem::HandleQueryInventoryFinished(bool bSuccess, const FString& Message, const FString& ProductsJson, const FString& PurchasesJson, int32 Generation)
{
	if (Generation != QueryGeneration)
	{
		UE_LOG(LogTemp, Warning, TEXT("[MyketBilling] Dropping stale HandleQueryInventoryFinished (gen %d, current %d)."), Generation, QueryGeneration);
		return;
	}
	bIsQueryInFlight = false;
	TArray<FMyketProduct> Products = bSuccess ? ParseProductsJson(ProductsJson) : TArray<FMyketProduct>();
	TArray<FMyketPurchase> Purchases = bSuccess ? ParsePurchasesJson(PurchasesJson) : TArray<FMyketPurchase>();
	OnQueryInventoryFinished.Broadcast(bSuccess, Message, Products, Purchases);
}

void UMyketBillingSubsystem::HandlePurchaseFinished(bool bSuccess, const FString& Message, const FString& PurchaseJson, int32 Generation)
{
	if (Generation != PurchaseGeneration)
	{
		UE_LOG(LogTemp, Warning, TEXT("[MyketBilling] Dropping stale HandlePurchaseFinished (gen %d, current %d)."), Generation, PurchaseGeneration);
		return;
	}
	bIsPurchaseInFlight = false;
	FMyketPurchase Purchase = bSuccess ? ParsePurchaseJson(PurchaseJson) : FMyketPurchase();
	OnPurchaseFinished.Broadcast(bSuccess, Message, Purchase);
}

void UMyketBillingSubsystem::HandleConsumeFinished(bool bSuccess, const FString& Message, const FString& PurchaseJson, int32 Generation)
{
	if (Generation != ConsumeGeneration)
	{
		UE_LOG(LogTemp, Warning, TEXT("[MyketBilling] Dropping stale HandleConsumeFinished (gen %d, current %d)."), Generation, ConsumeGeneration);
		return;
	}
	bIsConsumeInFlight = false;
	FMyketPurchase Purchase = bSuccess ? ParsePurchaseJson(PurchaseJson) : FMyketPurchase();
	OnConsumeFinished.Broadcast(bSuccess, Message, Purchase);
}

// ---------------------------------------------------------------------------
// JSON parsing. The Java bridge (MyketBillingBridge.java) is responsible for
// producing JSON in exactly this shape - see its buildProductJson / buildPurchaseJson.
// All fields are read defensively - a missing/malformed field just leaves the
// default value rather than crashing.
// ---------------------------------------------------------------------------

TArray<FMyketProduct> UMyketBillingSubsystem::ParseProductsJson(const FString& Json)
{
	TArray<FMyketProduct> Result;
	if (Json.IsEmpty()) return Result;

	TArray<TSharedPtr<FJsonValue>> JsonArray;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, JsonArray))
	{
		UE_LOG(LogTemp, Warning, TEXT("[MyketBilling] Failed to parse products JSON: %s"), *Json);
		return Result;
	}

	for (const TSharedPtr<FJsonValue>& Value : JsonArray)
	{
		if (!Value.IsValid()) continue;

		const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
		if (!Value->TryGetObject(ObjPtr) || !ObjPtr || !ObjPtr->IsValid()) continue;

		FMyketProduct Product;
		(*ObjPtr)->TryGetStringField(TEXT("sku"), Product.Sku);
		(*ObjPtr)->TryGetStringField(TEXT("title"), Product.Title);
		(*ObjPtr)->TryGetStringField(TEXT("description"), Product.Description);
		(*ObjPtr)->TryGetStringField(TEXT("price"), Product.Price);
		(*ObjPtr)->TryGetStringField(TEXT("type"), Product.Type);
		Result.Add(Product);
	}
	return Result;
}

FMyketPurchase UMyketBillingSubsystem::ParsePurchaseJson(const FString& Json)
{
	FMyketPurchase Purchase;
	if (Json.IsEmpty()) return Purchase;

	TSharedPtr<FJsonObject> Obj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[MyketBilling] Failed to parse purchase JSON: %s"), *Json);
		return Purchase;
	}

	Obj->TryGetStringField(TEXT("sku"), Purchase.Sku);
	Obj->TryGetStringField(TEXT("orderId"), Purchase.OrderId);
	Obj->TryGetStringField(TEXT("itemType"), Purchase.ItemType);
	Obj->TryGetStringField(TEXT("developerPayload"), Purchase.DeveloperPayload);
	Obj->TryGetStringField(TEXT("originalJson"), Purchase.OriginalJson);
	Obj->TryGetStringField(TEXT("signature"), Purchase.Signature);
	Obj->TryGetStringField(TEXT("token"), Purchase.Token);

	double PurchaseTime = 0.0;
	if (Obj->TryGetNumberField(TEXT("purchaseTime"), PurchaseTime))
	{
		Purchase.PurchaseTime = (int64)PurchaseTime;
	}

	return Purchase;
}

TArray<FMyketPurchase> UMyketBillingSubsystem::ParsePurchasesJson(const FString& Json)
{
	TArray<FMyketPurchase> Result;
	if (Json.IsEmpty()) return Result;

	TArray<TSharedPtr<FJsonValue>> JsonArray;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, JsonArray))
	{
		UE_LOG(LogTemp, Warning, TEXT("[MyketBilling] Failed to parse purchases array JSON: %s"), *Json);
		return Result;
	}

	for (const TSharedPtr<FJsonValue>& Value : JsonArray)
	{
		if (!Value.IsValid()) continue;

		const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
		if (!Value->TryGetObject(ObjPtr) || !ObjPtr || !ObjPtr->IsValid()) continue;

		FString ObjJson;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ObjJson);
		FJsonSerializer::Serialize((*ObjPtr).ToSharedRef(), Writer);
		Result.Add(ParsePurchaseJson(ObjJson));
	}
	return Result;
}
