#include "MyketBillingJNI.h"

#if PLATFORM_ANDROID

#include "MyketBillingSubsystem.h"
#include "Async/Async.h"
#include "Android/AndroidApplication.h"
#include "Android/AndroidJNI.h"

// Must match the package declared at the top of MyketBillingBridge.java
// and the destination path used for it in MyketBilling_UPL.xml's <copyDir>.
#define MYKET_BRIDGE_CLASS "com/myket/billing/MyketBillingBridge"

namespace
{
	jclass GBridgeClass = nullptr;
	bool bNativesRegistered = false;

	jclass GetBridgeClass(JNIEnv* Env)
	{
		if (!GBridgeClass)
		{
			jclass LocalClass = FAndroidApplication::FindJavaClass(MYKET_BRIDGE_CLASS);
			if (LocalClass)
			{
				GBridgeClass = (jclass)Env->NewGlobalRef(LocalClass);
				Env->DeleteLocalRef(LocalClass);
			}
		}
		return GBridgeClass;
	}

	/** Clears and logs any pending Java exception so it can't corrupt the next JNI call. */
	bool CheckAndClearException(JNIEnv* Env, const TCHAR* Where)
	{
		if (Env->ExceptionCheck())
		{
			Env->ExceptionDescribe();
			Env->ExceptionClear();
			UE_LOG(LogTemp, Error, TEXT("[MyketBilling] Java exception during %s - see logcat above for details."), Where);
			return true;
		}
		return false;
	}

	/** GetStaticMethodID with a friendly log on failure (e.g. after a Java-side signature change). */
	jmethodID FindStaticMethodOrLog(JNIEnv* Env, jclass Clazz, const char* Name, const char* Sig)
	{
		jmethodID Method = Env->GetStaticMethodID(Clazz, Name, Sig);
		if (!Method || CheckAndClearException(Env, *FString::Printf(TEXT("GetStaticMethodID(%hs)"), Name)))
		{
			UE_LOG(LogTemp, Error, TEXT("[MyketBilling] Could not resolve Java method %hs%hs - Java/C++ are out of sync."), Name, Sig);
			return nullptr;
		}
		return Method;
	}

	FString JStringToFString(JNIEnv* Env, jstring JStr)
	{
		if (!JStr) return FString();
		const char* Chars = Env->GetStringUTFChars(JStr, nullptr);
		if (!Chars) return FString();
		FString Result = FString(UTF8_TO_TCHAR(Chars));
		Env->ReleaseStringUTFChars(JStr, Chars);
		return Result;
	}

	jstring FStringToJString(JNIEnv* Env, const FString& Str)
	{
		return Env->NewStringUTF(TCHAR_TO_UTF8(*Str));
	}

	jobjectArray FStringArrayToJObjectArray(JNIEnv* Env, const TArray<FString>& Array)
	{
		jclass StringClass = Env->FindClass("java/lang/String");
		if (!StringClass)
		{
			CheckAndClearException(Env, TEXT("FindClass(java/lang/String)"));
			return nullptr;
		}
		jobjectArray Result = Env->NewObjectArray(Array.Num(), StringClass, Env->NewStringUTF(""));
		for (int32 i = 0; i < Array.Num() && Result; ++i)
		{
			jstring Elem = FStringToJString(Env, Array[i]);
			Env->SetObjectArrayElement(Result, i, Elem);
			Env->DeleteLocalRef(Elem);
		}
		Env->DeleteLocalRef(StringClass);
		return Result;
	}

	// ---------------------------------------------------------------------
	// Java -> C++ callbacks. Registered via RegisterNatives below, so these
	// bind regardless of the game's Java package name. Every callback now
	// carries the generation it was issued for (see MyketBillingJNI.h); the
	// Subsystem's Handle* methods are responsible for comparing it against
	// their current generation counter and dropping stale calls.
	// ---------------------------------------------------------------------

	void JNICALL Native_OnConnectionSucceeded(JNIEnv* Env, jclass Clazz, jint Generation)
	{
		AsyncTask(ENamedThreads::GameThread, [Generation]()
		{
			if (UMyketBillingSubsystem* Subsystem = UMyketBillingSubsystem::Get())
			{
				Subsystem->HandleConnectionSucceeded(Generation);
			}
		});
	}

	void JNICALL Native_OnConnectionFailed(JNIEnv* Env, jclass Clazz, jstring Message, jint Generation)
	{
		FString MessageStr = JStringToFString(Env, Message);
		AsyncTask(ENamedThreads::GameThread, [MessageStr, Generation]()
		{
			if (UMyketBillingSubsystem* Subsystem = UMyketBillingSubsystem::Get())
			{
				Subsystem->HandleConnectionFailed(MessageStr, Generation);
			}
		});
	}

	void JNICALL Native_OnQueryInventoryFinished(JNIEnv* Env, jclass Clazz, jboolean bSuccess, jstring Message, jstring ProductsJson, jstring PurchasesJson, jint Generation)
	{
		FString MessageStr = JStringToFString(Env, Message);
		FString ProductsJsonStr = JStringToFString(Env, ProductsJson);
		FString PurchasesJsonStr = JStringToFString(Env, PurchasesJson);
		bool bOk = (bSuccess != JNI_FALSE);
		AsyncTask(ENamedThreads::GameThread, [bOk, MessageStr, ProductsJsonStr, PurchasesJsonStr, Generation]()
		{
			if (UMyketBillingSubsystem* Subsystem = UMyketBillingSubsystem::Get())
			{
				Subsystem->HandleQueryInventoryFinished(bOk, MessageStr, ProductsJsonStr, PurchasesJsonStr, Generation);
			}
		});
	}

	void JNICALL Native_OnPurchaseFinished(JNIEnv* Env, jclass Clazz, jboolean bSuccess, jstring Message, jstring PurchaseJson, jint Generation)
	{
		FString MessageStr = JStringToFString(Env, Message);
		FString PurchaseJsonStr = JStringToFString(Env, PurchaseJson);
		bool bOk = (bSuccess != JNI_FALSE);
		AsyncTask(ENamedThreads::GameThread, [bOk, MessageStr, PurchaseJsonStr, Generation]()
		{
			if (UMyketBillingSubsystem* Subsystem = UMyketBillingSubsystem::Get())
			{
				Subsystem->HandlePurchaseFinished(bOk, MessageStr, PurchaseJsonStr, Generation);
			}
		});
	}

	void JNICALL Native_OnConsumeFinished(JNIEnv* Env, jclass Clazz, jboolean bSuccess, jstring Message, jstring PurchaseJson, jint Generation)
	{
		FString MessageStr = JStringToFString(Env, Message);
		FString PurchaseJsonStr = JStringToFString(Env, PurchaseJson);
		bool bOk = (bSuccess != JNI_FALSE);
		AsyncTask(ENamedThreads::GameThread, [bOk, MessageStr, PurchaseJsonStr, Generation]()
		{
			if (UMyketBillingSubsystem* Subsystem = UMyketBillingSubsystem::Get())
			{
				Subsystem->HandleConsumeFinished(bOk, MessageStr, PurchaseJsonStr, Generation);
			}
		});
	}
}

namespace MyketJNI
{
	void RegisterNatives()
	{
		if (bNativesRegistered) return;

		JNIEnv* Env = FAndroidApplication::GetJavaEnv();
		if (!Env)
		{
			UE_LOG(LogTemp, Error, TEXT("[MyketBilling] No JNIEnv available - cannot register natives."));
			return;
		}

		jclass BridgeClass = GetBridgeClass(Env);
		if (!BridgeClass)
		{
			UE_LOG(LogTemp, Error, TEXT("[MyketBilling] Could not find Java class %s - is the UPL wired up correctly?"), TEXT(MYKET_BRIDGE_CLASS));
			return;
		}

		static const JNINativeMethod Methods[] =
		{
			{ "nativeOnConnectionSucceeded",     "(I)V",                                                        (void*)&Native_OnConnectionSucceeded },
			{ "nativeOnConnectionFailed",        "(Ljava/lang/String;I)V",                                      (void*)&Native_OnConnectionFailed },
			{ "nativeOnQueryInventoryFinished",  "(ZLjava/lang/String;Ljava/lang/String;Ljava/lang/String;I)V", (void*)&Native_OnQueryInventoryFinished },
			{ "nativeOnPurchaseFinished",        "(ZLjava/lang/String;Ljava/lang/String;I)V",                   (void*)&Native_OnPurchaseFinished },
			{ "nativeOnConsumeFinished",         "(ZLjava/lang/String;Ljava/lang/String;I)V",                   (void*)&Native_OnConsumeFinished },
		};

		if (Env->RegisterNatives(BridgeClass, Methods, UE_ARRAY_COUNT(Methods)) == 0)
		{
			bNativesRegistered = true;
		}
		else
		{
			CheckAndClearException(Env, TEXT("RegisterNatives"));
			UE_LOG(LogTemp, Error, TEXT("[MyketBilling] RegisterNatives failed."));
		}
	}

	bool OpenConnection(const FString& PublicKey, int32 Generation)
	{
		JNIEnv* Env = FAndroidApplication::GetJavaEnv();
		jclass BridgeClass = Env ? GetBridgeClass(Env) : nullptr;
		if (!Env || !BridgeClass) return false;

		jmethodID Method = FindStaticMethodOrLog(Env, BridgeClass, "openConnection", "(Ljava/lang/String;I)V");
		if (!Method) return false;

		jstring JKey = FStringToJString(Env, PublicKey);
		Env->CallStaticVoidMethod(BridgeClass, Method, JKey, (jint)Generation);
		bool bThrew = CheckAndClearException(Env, TEXT("openConnection"));
		Env->DeleteLocalRef(JKey);
		return !bThrew;
	}

	bool QueryInventory(bool bQuerySkuDetails, const TArray<FString>& InAppSkus, const TArray<FString>& SubscriptionSkus, int32 Generation)
	{
		JNIEnv* Env = FAndroidApplication::GetJavaEnv();
		jclass BridgeClass = Env ? GetBridgeClass(Env) : nullptr;
		if (!Env || !BridgeClass) return false;

		jmethodID Method = FindStaticMethodOrLog(Env, BridgeClass, "queryInventory", "(Z[Ljava/lang/String;[Ljava/lang/String;I)V");
		if (!Method) return false;

		jobjectArray JInApp = FStringArrayToJObjectArray(Env, InAppSkus);
		jobjectArray JSubs = FStringArrayToJObjectArray(Env, SubscriptionSkus);
		if (!JInApp || !JSubs)
		{
			if (JInApp) Env->DeleteLocalRef(JInApp);
			if (JSubs) Env->DeleteLocalRef(JSubs);
			return false;
		}
		Env->CallStaticVoidMethod(BridgeClass, Method, (jboolean)bQuerySkuDetails, JInApp, JSubs, (jint)Generation);
		bool bThrew = CheckAndClearException(Env, TEXT("queryInventory"));
		Env->DeleteLocalRef(JInApp);
		Env->DeleteLocalRef(JSubs);
		return !bThrew;
	}

	bool LaunchPurchaseFlow(const FString& Sku, const FString& DeveloperPayload, int32 Generation)
	{
		JNIEnv* Env = FAndroidApplication::GetJavaEnv();
		jclass BridgeClass = Env ? GetBridgeClass(Env) : nullptr;
		if (!Env || !BridgeClass) return false;

		jmethodID Method = FindStaticMethodOrLog(Env, BridgeClass, "launchPurchaseFlow", "(Ljava/lang/String;Ljava/lang/String;I)V");
		if (!Method) return false;

		jstring JSku = FStringToJString(Env, Sku);
		jstring JPayload = FStringToJString(Env, DeveloperPayload);
		Env->CallStaticVoidMethod(BridgeClass, Method, JSku, JPayload, (jint)Generation);
		bool bThrew = CheckAndClearException(Env, TEXT("launchPurchaseFlow"));
		Env->DeleteLocalRef(JSku);
		Env->DeleteLocalRef(JPayload);
		return !bThrew;
	}

	bool ConsumePurchase(const FString& ItemType, const FString& OriginalJson, const FString& Signature, int32 Generation)
	{
		JNIEnv* Env = FAndroidApplication::GetJavaEnv();
		jclass BridgeClass = Env ? GetBridgeClass(Env) : nullptr;
		if (!Env || !BridgeClass) return false;

		jmethodID Method = FindStaticMethodOrLog(Env, BridgeClass, "consumePurchase", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)V");
		if (!Method) return false;

		jstring JItemType = FStringToJString(Env, ItemType);
		jstring JOriginalJson = FStringToJString(Env, OriginalJson);
		jstring JSignature = FStringToJString(Env, Signature);
		Env->CallStaticVoidMethod(BridgeClass, Method, JItemType, JOriginalJson, JSignature, (jint)Generation);
		bool bThrew = CheckAndClearException(Env, TEXT("consumePurchase"));
		Env->DeleteLocalRef(JItemType);
		Env->DeleteLocalRef(JOriginalJson);
		Env->DeleteLocalRef(JSignature);
		return !bThrew;
	}

	bool CloseConnection()
	{
		JNIEnv* Env = FAndroidApplication::GetJavaEnv();
		jclass BridgeClass = Env ? GetBridgeClass(Env) : nullptr;
		if (!Env || !BridgeClass) return false;

		jmethodID Method = FindStaticMethodOrLog(Env, BridgeClass, "closeConnection", "()V");
		if (!Method) return false;

		Env->CallStaticVoidMethod(BridgeClass, Method);
		return !CheckAndClearException(Env, TEXT("closeConnection"));
	}
}

#endif // PLATFORM_ANDROID
