#pragma once

#include "CoreMinimal.h"

#if PLATFORM_ANDROID

/**
 * Thin C++ <-> Java bridge for MyketBillingBridge.java.
 *
 * C++ -> Java: plain static method calls via JNI (FJavaWrapper-style lookups).
 * Java -> C++: MyketBillingBridge declares `public static native void nativeOnXxx(...)`
 *              methods; RegisterNatives() binds those to the functions below at startup,
 *              so we don't depend on the game's Java package name for the classic
 *              Java_<package>_<Class>_<method> naming convention.
 *
 * Every function below returns true only if the call was actually dispatched
 * into Java. If it returns false (JNIEnv/class/method not resolvable, or a
 * Java exception escaped the dispatch itself), the caller MUST treat this as
 * an immediate failure and broadcast/reset its own state - Java will not be
 * calling back in that case, so nothing else will.
 *
 * The Generation parameters are opaque integers the Subsystem stamps on each
 * new attempt; Java echoes them back unchanged via the corresponding
 * native* callback. This lets the Subsystem recognize and drop a late
 * callback from an attempt that has since been superseded (a newer connect/
 * purchase/query call, or a manual reset) instead of letting it corrupt
 * current state.
 */
namespace MyketJNI
{
	/** Call once, e.g. from UMyketBillingSubsystem::Initialize. Safe to call more than once. */
	void RegisterNatives();

	bool OpenConnection(const FString& PublicKey, int32 Generation);
	bool QueryInventory(bool bQuerySkuDetails, const TArray<FString>& InAppSkus, const TArray<FString>& SubscriptionSkus, int32 Generation);
	bool LaunchPurchaseFlow(const FString& Sku, const FString& DeveloperPayload, int32 Generation);
	bool ConsumePurchase(const FString& ItemType, const FString& OriginalJson, const FString& Signature, int32 Generation);
	bool CloseConnection();
}

#endif // PLATFORM_ANDROID
