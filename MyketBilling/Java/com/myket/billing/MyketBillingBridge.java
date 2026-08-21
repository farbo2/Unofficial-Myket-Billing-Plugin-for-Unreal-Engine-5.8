package com.myket.billing;

import android.app.Activity;
import android.text.TextUtils;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.lang.ref.WeakReference;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

import ir.myket.billingclient.IabHelper;
import ir.myket.billingclient.util.IabResult;
import ir.myket.billingclient.util.Inventory;
import ir.myket.billingclient.util.Purchase;
import ir.myket.billingclient.util.SkuDetails;

/**
 * Static bridge between UE5's C++/JNI layer and Myket's official IabHelper SDK
 * (https://github.com/myketstore/myket-billing-client), v1.19.
 *
 * Wired into GameActivity.java by MyketBilling_UPL.xml:
 *   - onCreate  -> setActivity(this)
 *   - onDestroy -> onDestroy(this)
 *
 * v1.19 of IabHelper manages the purchase result internally (via its own
 * DialogActivity/ProxyBillingActivity), so no onActivityResult hook is needed.
 *
 * All native* methods are bound from C++ via JNI RegisterNatives (see MyketBillingJNI.cpp),
 * so this class does not need to know the game's Java package name.
 *
 * Every native* callback below now carries a "generation" integer that the C++
 * layer stamped on the call that triggered it (see MyketBillingJNI.h). This
 * class treats it as an opaque value to store and echo back unchanged - the
 * Subsystem is what decides whether a generation is still current or stale.
 *
 * Production-hardening notes:
 *   - Every entry point validates its inputs and the current activity/helper
 *     state before touching IabHelper, and always reports a result back
 *     through the native* callbacks - callers never get silently ignored or
 *     left hanging with a permanently-stuck "in progress" flag.
 *   - sConnecting/sPurchaseInFlight/sQueryInFlight/sConsumeInFlight guard
 *     against re-entrant calls (e.g. a double-tapped Buy button), independent
 *     of whatever guard C++ also applies - this class should be safe to call
 *     in isolation. They also guard against each OTHER: the underlying
 *     IabHelper (ir.myket.billingclient.util.IAB.flagStartAsync /
 *     isAsyncOperationInProgress) only allows one async operation in flight
 *     per helper instance at all, not one per kind - see isAnyOperationInFlight().
 *   - Every IabHelper call and every listener body is wrapped in try/catch so
 *     an SDK-side exception can't crash the app or corrupt JNI state; the C++
 *     layer separately clears any pending JNI exception, but Java is the
 *     first line of defense.
 *   - Every entry point runs on the Activity's UI thread via runOnUiThread
 *     (through tryRunOnUi, which reports dispatch failure rather than
 *     silently dropping the call), since UE5's GameThread has no Android
 *     Looper and IabHelper's internal AIDL connection needs one.
 *
 * Known limitation: if Android kills the app process while Myket's payment
 * UI is in the foreground (e.g. under memory pressure), the in-memory
 * IabHelper state is lost with it, and no callback will fire on return. This
 * mirrors the behavior of a native app that doesn't persist billing state
 * across process death. Call QueryInventory on next launch to reconcile.
 */
public final class MyketBillingBridge
{
	private static final String TAG = "MyketBilling";

	// WeakReference, not a plain strong reference: a static field holding an
	// Activity strongly is a classic Android leak (keeps the Activity, and
	// everything it holds, alive past its own destruction). Using
	// WeakReference also gives us a stable identity to check against in
	// onDestroy(Activity) below.
	private static volatile WeakReference<Activity> sActivity = new WeakReference<>(null);
	private static volatile IabHelper sHelper;

	// The generation (see MyketBillingJNI.h) of whatever connection attempt
	// is currently represented by sHelper above, if any. Used by
	// cleanupConnection() to tell C++ "the connection you think is still
	// open is gone" with the correct generation, regardless of whether
	// teardown happened via an explicit CloseConnection or because Android
	// tore down and recreated GameActivity out from under us.
	private static volatile int sActiveConnectionGeneration = -1;

	private static final AtomicBoolean sConnecting = new AtomicBoolean(false);
	private static final AtomicBoolean sPurchaseInFlight = new AtomicBoolean(false);
	private static final AtomicBoolean sQueryInFlight = new AtomicBoolean(false);
	private static final AtomicBoolean sConsumeInFlight = new AtomicBoolean(false);

	// IabHelper (see ir.myket.billingclient.util.IAB.flagStartAsync /
	// isAsyncOperationInProgress, shared by ServiceIAB.launchPurchaseFlow,
	// queryInventoryAsync and consumeAsync alike) only allows ONE async
	// operation in flight per helper instance - not one per kind. Without this
	// check, starting a second kind while another is running reaches the SDK
	// and fails there with a generic IllegalStateException instead of this
	// clear message; the C++ Subsystem already guards this too (its checks
	// run first, on the Game Thread, before this class is ever called), but
	// this class is documented to be safe to call in isolation, so it needs
	// its own copy of the same guard. Call this BEFORE compareAndSet-ing your
	// own flag - at that point your own flag is still false, so a plain OR
	// across all three is exactly "is some other kind of operation running".
	private static boolean isAnyOperationInFlight()
	{
		return sPurchaseInFlight.get() || sQueryInFlight.get() || sConsumeInFlight.get();
	}

	private MyketBillingBridge() {}

	// ------------------------------------------------------------------
	// Lifecycle hooks (called from GameActivity via UPL)
	// ------------------------------------------------------------------

	public static void setActivity(Activity activity)
	{
		sActivity = new WeakReference<>(activity);
	}

	/**
	 * Called from GameActivity.onDestroy(this) via the UPL hook - note the
	 * (this) argument, not a no-arg call. Android normally guarantees the old
	 * Activity's onDestroy() completes before a recreated instance's onCreate()
	 * runs, so in the common single-instance case destroyedActivity is always
	 * whatever sActivity currently points to anyway. This check exists for the
	 * less common case where it might not be: Android multi-window/multi-instance
	 * configurations (e.g. "open in new window" on foldables/tablets) can have
	 * two live instances of GameActivity at once. Without this check, closing
	 * an old, already-superseded window could tear down the connection state
	 * that a *different*, currently-active window is using. Only an Activity
	 * this bridge still considers current is allowed to trigger cleanup.
	 */
	public static void onDestroy(Activity destroyedActivity)
	{
		Activity current = sActivity.get();
		if (current != null && current != destroyedActivity)
		{
			Log.w(TAG, "onDestroy: ignoring teardown for a non-current Activity instance.");
			return;
		}
		cleanupConnection();
	}

	/**
	 * Shared teardown logic for both onDestroy(Activity) above (Activity
	 * lifecycle - may be an explicit close or an Android-driven recreation)
	 * and CloseConnection() below (an explicit, application-level request,
	 * which always applies regardless of Activity identity).
	 *
	 * If there was a live helper, this synthesizes a native connection-failed
	 * notification carrying the generation that connection was created
	 * under (Myket's IabHelper has no separate "disconnected" concept the
	 * way Poolakey does, so nativeOnConnectionFailed is what transitions
	 * ConnectionState back to Disconnected and invalidates any in-flight
	 * purchase/query - see UMyketBillingSubsystem::HandleConnectionFailed).
	 * If C++'s generation counter already moved on on its own (the ordinary
	 * explicit-close path, where C++ updates its state before ever calling
	 * into Java), this notification arrives stale and is silently dropped -
	 * harmless. If C++ never got the memo (Activity recreation, where
	 * nothing called CloseConnection on C++'s behalf), this is what tells
	 * it, instead of leaving IsPurchaseInFlight stuck true forever with no
	 * callback ever arriving.
	 */
	private static void cleanupConnection()
	{
		final IabHelper helperToDispose = sHelper;
		final int generationToNotify = sActiveConnectionGeneration;
		final boolean hadHelper = (helperToDispose != null);

		sHelper = null;
		sActiveConnectionGeneration = -1;
		sConnecting.set(false);
		sPurchaseInFlight.set(false);
		sQueryInFlight.set(false);
		sConsumeInFlight.set(false);

		if (helperToDispose != null)
		{
			Activity a = sActivity.get();
			boolean disposedOnUi = false;
			if (a != null)
			{
				try
				{
					a.runOnUiThread(new Runnable()
					{
						@Override
						public void run()
						{
							try { helperToDispose.dispose(); } catch (Throwable t) { Log.e(TAG, "dispose failed", t); }
						}
					});
					disposedOnUi = true;
				}
				catch (Throwable t)
				{
					Log.e(TAG, "runOnUiThread dispatch failed during dispose, disposing synchronously", t);
				}
			}
			if (!disposedOnUi)
			{
				// No usable Activity, or dispatch failed - dispose synchronously
				// as a fallback rather than leaking the native helper.
				try { helperToDispose.dispose(); } catch (Throwable t) { Log.e(TAG, "dispose failed", t); }
			}
		}

		if (hadHelper)
		{
			nativeOnConnectionFailed("Connection lost (Activity destroyed or connection closed).", generationToNotify);
		}
	}

	// ------------------------------------------------------------------
	// Internal helpers
	// ------------------------------------------------------------------

	private static boolean isActivityUsable()
	{
		Activity a = sActivity.get();
		if (a == null || a.isFinishing()) return false;
		return !a.isDestroyed();
	}

	/**
	 * Attempts to run r on the UI thread. Returns false if that could not even
	 * be attempted (no/dead Activity, or runOnUiThread itself threw
	 * synchronously). Callers MUST treat a false return as an immediate
	 * terminal failure of whatever operation they were starting.
	 */
	private static boolean tryRunOnUi(final Runnable r)
	{
		Activity a = sActivity.get();
		if (a == null || a.isFinishing() || a.isDestroyed())
		{
			Log.e(TAG, "tryRunOnUi: no usable Activity.");
			return false;
		}
		try
		{
			a.runOnUiThread(r);
			return true;
		}
		catch (Throwable t)
		{
			Log.e(TAG, "runOnUiThread dispatch failed", t);
			return false;
		}
	}

	private static String describeThrowable(Throwable t)
	{
		if (t == null) return "Unknown error";
		String msg = t.getMessage();
		return TextUtils.isEmpty(msg) ? t.getClass().getSimpleName() : msg;
	}

	// ------------------------------------------------------------------
	// API called from C++ (MyketBillingJNI.cpp). Each takes a "generation"
	// int that is opaque here - just store it in the closure and echo it
	// back unchanged on the corresponding native* callback.
	// ------------------------------------------------------------------

	public static void openConnection(final String publicKey, final int generation)
	{
		if (TextUtils.isEmpty(publicKey))
		{
			nativeOnConnectionFailed("Public key is empty.", generation);
			return;
		}
		if (!isActivityUsable())
		{
			nativeOnConnectionFailed("Activity not ready yet.", generation);
			return;
		}
		// No "already connecting -> silently ignore" guard here anymore: C++
		// always relays every OpenConnection call and expects a definitive
		// terminal callback for the generation it sent (see the comment in
		// UMyketBillingSubsystem::OpenConnection for why silently dropping
		// duplicates was unsafe). runOnUiThread serializes calls onto a single
		// queue, and the block below already tears down whatever helper
		// existed before building a new one, so concurrent calls are handled
		// correctly without needing a separate guard here.
		sConnecting.set(true);

		if (!tryRunOnUi(new Runnable()
		{
			@Override
			public void run()
			{
				try
				{
					// Tear down any previous helper before creating a new one, so
					// re-connecting after a manual CloseConnection can't leak.
					// Also reset the in-flight flags here, mirroring what C++'s
					// OpenConnection does on its side (see the comment there): if
					// a purchase/query was in flight under the previous helper,
					// that helper is gone now, so leaving these flags true would
					// make the *next* launchPurchaseFlow/queryInventory call get
					// silently rejected by the compareAndSet guards below, even
					// after C++ has already decided (correctly) to allow it through.
					IabHelper previous = sHelper;
					sHelper = null;
					sPurchaseInFlight.set(false);
					sQueryInFlight.set(false);
					sConsumeInFlight.set(false);
					if (previous != null)
					{
						try { previous.dispose(); } catch (Throwable ignored) {}
					}

					Activity activity = sActivity.get();
					if (activity == null || activity.isFinishing() || activity.isDestroyed())
					{
						sConnecting.set(false);
						nativeOnConnectionFailed("Activity became unavailable while connecting.", generation);
						return;
					}

					final IabHelper helper = new IabHelper(activity, publicKey);
					helper.enableDebugLogging(false);
					sHelper = helper;
					sActiveConnectionGeneration = generation;

					helper.startSetup(new IabHelper.OnIabSetupFinishedListener()
					{
						@Override
						public void onIabSetupFinished(IabResult result)
						{
							sConnecting.set(false);
							if (result != null && result.isSuccess())
							{
								nativeOnConnectionSucceeded(generation);
							}
							else
							{
								// This attempt is dead - clear it so a later cleanupConnection()
								// (e.g. Activity teardown) doesn't think there's still a live
								// connection to synthesize a redundant failure notification for.
								if (sActiveConnectionGeneration == generation)
								{
									sHelper = null;
									sActiveConnectionGeneration = -1;
								}
								nativeOnConnectionFailed(result != null ? result.getMessage() : "Unknown setup failure.", generation);
							}
						}
					});
				}
				catch (Throwable t)
				{
					Log.e(TAG, "openConnection failed", t);
					sConnecting.set(false);
					nativeOnConnectionFailed("Exception while connecting: " + describeThrowable(t), generation);
				}
			}
		}))
		{
			sConnecting.set(false);
			nativeOnConnectionFailed("Could not dispatch to the UI thread (activity unavailable).", generation);
		}
	}

	public static void queryInventory(final boolean querySkuDetails, final String[] inAppSkus, final String[] subsSkus, final int generation)
	{
		final IabHelper helper = sHelper;
		if (helper == null)
		{
			nativeOnQueryInventoryFinished(false, "Not connected. Call OpenConnection first.", "[]", "[]", generation);
			return;
		}
		// Check BEFORE compareAndSet-ing our own flag below, so this doesn't
		// see (and reject on) the flag this very call is about to set.
		if (isAnyOperationInFlight())
		{
			nativeOnQueryInventoryFinished(false, "Another Myket operation (purchase or consume) is already in progress.", "[]", "[]", generation);
			return;
		}
		if (!sQueryInFlight.compareAndSet(false, true))
		{
			nativeOnQueryInventoryFinished(false, "A query is already in progress.", "[]", "[]", generation);
			return;
		}

		if (!tryRunOnUi(new Runnable()
		{
			@Override
			public void run()
			{
				try
				{
					List<String> allSkus = new ArrayList<>();
					if (inAppSkus != null) allSkus.addAll(java.util.Arrays.asList(inAppSkus));
					if (subsSkus != null) allSkus.addAll(java.util.Arrays.asList(subsSkus));

					helper.queryInventoryAsync(querySkuDetails, allSkus, new IabHelper.QueryInventoryFinishedListener()
					{
						@Override
						public void onQueryInventoryFinished(IabResult result, Inventory inventory)
						{
							sQueryInFlight.set(false);
							if (result == null || result.isFailure())
							{
								nativeOnQueryInventoryFinished(false, result != null ? result.getMessage() : "Unknown query failure.", "[]", "[]", generation);
								return;
							}
							String productsJson = buildProductsJson(inventory, querySkuDetails, inAppSkus, subsSkus);
							String purchasesJson = buildPurchasesJson(inventory, inAppSkus, subsSkus);
							nativeOnQueryInventoryFinished(true, "OK", productsJson, purchasesJson, generation);
						}
					});
				}
				catch (Throwable t)
				{
					Log.e(TAG, "queryInventory failed", t);
					sQueryInFlight.set(false);
					nativeOnQueryInventoryFinished(false, "Error querying inventory: " + describeThrowable(t), "[]", "[]", generation);
				}
			}
		}))
		{
			sQueryInFlight.set(false);
			nativeOnQueryInventoryFinished(false, "Could not dispatch to the UI thread (activity unavailable).", "[]", "[]", generation);
		}
	}

	public static void launchPurchaseFlow(final String sku, final String developerPayload, final int generation)
	{
		if (TextUtils.isEmpty(sku))
		{
			nativeOnPurchaseFinished(false, "SKU is empty.", "{}", generation);
			return;
		}
		final IabHelper helper = sHelper;
		if (helper == null)
		{
			nativeOnPurchaseFinished(false, "Not connected. Call OpenConnection first.", "{}", generation);
			return;
		}
		if (!isActivityUsable())
		{
			nativeOnPurchaseFinished(false, "Activity not available.", "{}", generation);
			return;
		}
		// Check BEFORE compareAndSet-ing our own flag below, so this doesn't
		// see (and reject on) the flag this very call is about to set.
		if (isAnyOperationInFlight())
		{
			nativeOnPurchaseFinished(false, "Another Myket operation (query or consume) is already in progress.", "{}", generation);
			return;
		}
		if (!sPurchaseInFlight.compareAndSet(false, true))
		{
			nativeOnPurchaseFinished(false, "A purchase is already in progress.", "{}", generation);
			return;
		}

		if (!tryRunOnUi(new Runnable()
		{
			@Override
			public void run()
			{
				try
				{
					if (!isActivityUsable())
					{
						sPurchaseInFlight.set(false);
						nativeOnPurchaseFinished(false, "Activity became unavailable.", "{}", generation);
						return;
					}

					Activity activity = sActivity.get();
					if (activity == null)
					{
						sPurchaseInFlight.set(false);
						nativeOnPurchaseFinished(false, "Activity became unavailable.", "{}", generation);
						return;
					}

					helper.launchPurchaseFlow(activity, sku, new IabHelper.OnIabPurchaseFinishedListener()
					{
						@Override
						public void onIabPurchaseFinished(IabResult result, Purchase purchase)
						{
							sPurchaseInFlight.set(false);
							if (result == null || result.isFailure() || purchase == null)
							{
								nativeOnPurchaseFinished(false, result != null ? result.getMessage() : "Unknown purchase failure.", "{}", generation);
								return;
							}
							nativeOnPurchaseFinished(true, "OK", purchaseToJson(purchase).toString(), generation);
						}
					}, developerPayload);
				}
				catch (Throwable t)
				{
					Log.e(TAG, "launchPurchaseFlow failed", t);
					sPurchaseInFlight.set(false);
					nativeOnPurchaseFinished(false, "Error launching purchase flow: " + describeThrowable(t), "{}", generation);
				}
			}
		}))
		{
			sPurchaseInFlight.set(false);
			nativeOnPurchaseFinished(false, "Could not dispatch to the UI thread (activity unavailable).", "{}", generation);
		}
	}

	public static void consumePurchase(final String itemType, final String originalJson, final String signature, final int generation)
	{
		if (TextUtils.isEmpty(originalJson) || TextUtils.isEmpty(signature))
		{
			nativeOnConsumeFinished(false, "Purchase JSON/signature is empty.", "{}", generation);
			return;
		}
		final IabHelper helper = sHelper;
		if (helper == null)
		{
			nativeOnConsumeFinished(false, "Not connected. Call OpenConnection first.", "{}", generation);
			return;
		}
		// Check BEFORE compareAndSet-ing our own flag below, so this doesn't
		// see (and reject on) the flag this very call is about to set.
		if (isAnyOperationInFlight())
		{
			nativeOnConsumeFinished(false, "Another Myket operation (query or purchase) is already in progress.", "{}", generation);
			return;
		}
		if (!sConsumeInFlight.compareAndSet(false, true))
		{
			nativeOnConsumeFinished(false, "A consume is already in progress.", "{}", generation);
			return;
		}

		if (!tryRunOnUi(new Runnable()
		{
			@Override
			public void run()
			{
				try
				{
					final Purchase purchase = new Purchase(itemType, originalJson, signature);
					helper.consumeAsync(purchase, new IabHelper.OnConsumeFinishedListener()
					{
						@Override
						public void onConsumeFinished(Purchase consumedPurchase, IabResult result)
						{
							sConsumeInFlight.set(false);
							if (result == null || result.isFailure())
							{
								nativeOnConsumeFinished(false, result != null ? result.getMessage() : "Unknown consume failure.",
									consumedPurchase != null ? purchaseToJson(consumedPurchase).toString() : "{}", generation);
								return;
							}
							nativeOnConsumeFinished(true, "OK", purchaseToJson(consumedPurchase).toString(), generation);
						}
					});
				}
				catch (JSONException e)
				{
					sConsumeInFlight.set(false);
					nativeOnConsumeFinished(false, "Malformed purchase JSON: " + describeThrowable(e), "{}", generation);
				}
				catch (Throwable t)
				{
					Log.e(TAG, "consumePurchase failed", t);
					sConsumeInFlight.set(false);
					nativeOnConsumeFinished(false, "Error consuming purchase: " + describeThrowable(t), "{}", generation);
				}
			}
		}))
		{
			sConsumeInFlight.set(false);
			nativeOnConsumeFinished(false, "Could not dispatch to the UI thread (activity unavailable).", "{}", generation);
		}
	}

	public static void closeConnection()
	{
		if (!tryRunOnUi(new Runnable() { @Override public void run() { cleanupConnection(); } }))
		{
			// Dispatch failed - fall back to synchronous cleanup right here so
			// state doesn't get left dangling just because the Activity is gone.
			cleanupConnection();
		}
	}

	// ------------------------------------------------------------------
	// JSON helpers - keep this in sync with UMyketBillingSubsystem's parsers
	// ------------------------------------------------------------------

	private static String buildProductsJson(Inventory inventory, boolean querySkuDetails, String[] inAppSkus, String[] subsSkus)
	{
		JSONArray array = new JSONArray();
		if (!querySkuDetails || inventory == null) return array.toString();

		try
		{
			if (inAppSkus != null)
			{
				for (String sku : inAppSkus)
				{
					SkuDetails details = inventory.getSkuDetails(sku);
					if (details != null) array.put(skuDetailsToJson(details));
				}
			}
			if (subsSkus != null)
			{
				for (String sku : subsSkus)
				{
					SkuDetails details = inventory.getSkuDetails(sku);
					if (details != null) array.put(skuDetailsToJson(details));
				}
			}
		}
		catch (Throwable t)
		{
			Log.e(TAG, "buildProductsJson failed", t);
		}
		return array.toString();
	}

	private static String buildPurchasesJson(Inventory inventory, String[] inAppSkus, String[] subsSkus)
	{
		JSONArray array = new JSONArray();
		if (inventory == null) return array.toString();

		try
		{
			if (inAppSkus != null)
			{
				for (String sku : inAppSkus)
				{
					Purchase p = inventory.getPurchase(sku);
					if (p != null) array.put(purchaseToJson(p));
				}
			}
			if (subsSkus != null)
			{
				for (String sku : subsSkus)
				{
					Purchase p = inventory.getPurchase(sku);
					if (p != null) array.put(purchaseToJson(p));
				}
			}
		}
		catch (Throwable t)
		{
			Log.e(TAG, "buildPurchasesJson failed", t);
		}
		return array.toString();
	}

	private static JSONObject skuDetailsToJson(SkuDetails details)
	{
		JSONObject obj = new JSONObject();
		if (details == null) return obj;
		try
		{
			obj.put("sku", nullToEmpty(details.getSku()));
			obj.put("title", nullToEmpty(details.getTitle()));
			obj.put("description", nullToEmpty(details.getDescription()));
			obj.put("price", nullToEmpty(details.getPrice()));
			obj.put("type", nullToEmpty(details.getType()));
		}
		catch (JSONException e)
		{
			Log.e(TAG, "skuDetailsToJson failed", e);
		}
		return obj;
	}

	private static JSONObject purchaseToJson(Purchase purchase)
	{
		JSONObject obj = new JSONObject();
		if (purchase == null) return obj;
		try
		{
			obj.put("sku", nullToEmpty(purchase.getSku()));
			obj.put("orderId", nullToEmpty(purchase.getOrderId()));
			obj.put("itemType", nullToEmpty(purchase.getItemType()));
			obj.put("developerPayload", nullToEmpty(purchase.getDeveloperPayload()));
			obj.put("originalJson", nullToEmpty(purchase.getOriginalJson()));
			obj.put("signature", nullToEmpty(purchase.getSignature()));
			obj.put("token", nullToEmpty(purchase.getToken()));
			obj.put("purchaseTime", purchase.getPurchaseTime());
		}
		catch (JSONException e)
		{
			Log.e(TAG, "purchaseToJson failed", e);
		}
		return obj;
	}

	private static String nullToEmpty(String s)
	{
		return s != null ? s : "";
	}

	// ------------------------------------------------------------------
	// Native callbacks, implemented in C++ and bound via RegisterNatives.
	// The trailing `generation` int on each is the value that was passed
	// into the corresponding call above - echoed back unchanged.
	// ------------------------------------------------------------------

	private static native void nativeOnConnectionSucceeded(int generation);
	private static native void nativeOnConnectionFailed(String message, int generation);
	private static native void nativeOnQueryInventoryFinished(boolean success, String message, String productsJson, String purchasesJson, int generation);
	private static native void nativeOnPurchaseFinished(boolean success, String message, String purchaseJson, int generation);
	private static native void nativeOnConsumeFinished(boolean success, String message, String purchaseJson, int generation);
}
