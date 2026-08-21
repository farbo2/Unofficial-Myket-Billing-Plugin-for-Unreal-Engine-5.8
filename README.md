# Myket In-App Billing for Unreal Engine 5

A Blueprint-friendly Unreal Engine plugin for integrating Myket in-app billing on Android.

The plugin exposes Myket billing through an Unreal **Game Instance Subsystem**, providing Blueprint access to:

* Billing connection
* Product and purchase queries
* In-app purchases
* Subscriptions
* Consumable purchase consumption
* Purchase information and verification data
* Billing state
* Billing operation recovery

The Android billing implementation is handled internally by the plugin. Your Unreal project interacts with it through the Blueprint API.

---

## Requirements

* Unreal Engine 5
* Android target
* A Myket developer account
* An application registered in Myket
* Products and/or subscriptions configured in Myket

The complete billing flow should be tested on a real Android device.

---

# Installation

1. Copy the `MyketBilling` folder into your project's `Plugins` directory.

```text
YourProject/
└── Plugins/
    └── MyketBilling/
```

2. Open the Unreal Engine project.

3. Enable **Myket Billing** from:

```text
Edit → Plugins
```

4. Restart the editor if required.

5. Make sure your Android application package name matches the application registered in Myket.

6. Make sure your products and subscriptions are configured in the Myket developer panel.

The plugin adds the required Android billing configuration and dependency automatically during packaging.

---

# Getting Started

The main API is exposed through:

**Get Game Instance Subsystem → Myket Billing Subsystem**

A basic flow looks like this:

```text
Open Connection
      ↓
On Connection Succeeded
      ↓
Query Inventory
      ↓
Launch Purchase Flow
      ↓
On Purchase Finished
      ↓
Deliver Product
      ↓
Consume Purchase
      ↓
On Consume Finished
```

For non-consumable products and subscriptions, do not consume the purchase.

---

# Important: Only One Billing Operation at a Time

Myket's billing API allows only **one asynchronous billing operation at a time** for a connection.

This applies to:

* `Query Inventory`
* `Launch Purchase Flow`
* `Consume Purchase`

They must not run simultaneously.

Use:

`Is Any Operation In Flight`

to prevent overlapping operations.

For example:

```text
Purchase Button
      ↓
Is Any Operation In Flight?
      ↓
      No
      ↓
Launch Purchase Flow
```

This is preferable to checking only `Is Purchase In Flight`, because another operation such as an inventory query or consumption may already be running.

---

# Blueprint API

## Open Connection

Connects the plugin to Myket billing.

### Input

**Public Key**

The Base64 public key associated with your Myket application.

### Events

* `On Connection Succeeded`
* `On Connection Failed`

### Recommended usage

Call this when your billing system starts.

```text
BeginPlay
    ↓
Open Connection
    ↓
On Connection Succeeded
    ↓
Query Inventory
```

Do not start purchase, query, or consume operations before the connection succeeds.

---

## Query Inventory

Retrieves the user's purchases and, optionally, product details.

### Inputs

**Query SKU Details**

When enabled, the query also returns product information such as title, description, price, and product type.

**In-App SKUs**

Array of in-app product IDs.

Example:

```text
coins_100
remove_ads
premium_pack
```

**Subscription SKUs**

Array of subscription product IDs.

Example:

```text
premium_monthly
premium_yearly
```

### Event

`On Query Inventory Finished`

Returns:

* `Success`
* `Message`
* `Products`
* `Purchases`

### Recommended usage

Query inventory after connecting and when the application needs to restore or reconcile purchases.

```text
On Connection Succeeded
        ↓
Query Inventory
```

> **Important:** Do not rely only on the purchase callback to restore purchases. Query the inventory when the application starts.

---

## Launch Purchase Flow

Opens the Myket purchase interface for a single SKU.

### Inputs

**SKU**

The product ID configured in Myket.

**Developer Payload**

Application-defined data associated with the purchase.

### Event

`On Purchase Finished`

The same node can be used for:

* In-app products
* Subscriptions

### Important

Before starting a purchase, make sure no other billing operation is running.

Recommended check:

```text
Is Any Operation In Flight
        ↓
False
        ↓
Launch Purchase Flow
```

Also prevent multiple clicks on your purchase UI.

---

## Consume Purchase

Consumes a purchased product so that it can be purchased again.

### Input

**Purchase**

The `FMyketPurchase` returned by:

* `On Purchase Finished`
* `On Query Inventory Finished`

### Event

`On Consume Finished`

### Use for

* Coins
* Gems
* Consumable packs
* Other repeatable purchases

### Do not use for

* Permanent unlocks
* Remove Ads
* Other non-consumable products
* Subscriptions

### Recommended flow

```text
On Purchase Finished
        ↓
Validate Purchase
        ↓
Deliver Product
        ↓
Consume Purchase
        ↓
On Consume Finished
```

> **Important:** Deliver the purchased content before consuming it. Consumption should not be treated as the step that grants the item.

---

## Close Connection

Closes the current billing connection.

Use this when billing is no longer needed, such as during application shutdown.

You do not normally need to repeatedly connect and disconnect around individual purchases.

---

# Connection State

## Get Connection State

Returns the current connection state:

* `Disconnected`
* `Connecting`
* `Connected`

Use this to control your store UI.

---

## Is Connected

Returns `true` when the billing connection is ready.

Use this before starting billing operations.

---

## Is Platform Supported

Returns whether the current platform supports the billing integration.

The plugin is intended for Android.

---

# Operation State

## Is Purchase In Flight

Returns `true` while a purchase flow is being processed.

Useful for disabling the purchase button.

---

## Is Query In Flight

Returns `true` while `Query Inventory` is being processed.

---

## Is Consume In Flight

Returns `true` while `Consume Purchase` is being processed.

---

## Is Any Operation In Flight

Returns `true` when **any** of the following operations is active:

* Purchase
* Query
* Consume

This is the recommended check before starting a new billing operation.

```text
Is Any Operation In Flight?
        ↓
     False
        ↓
Start Billing Operation
```

Because Myket allows only one asynchronous billing operation at a time, this check is generally more useful than checking each operation separately.

---

# Reset Stuck Flags

Clears the local in-flight state for billing operations without closing the connection.

Use this only as a recovery mechanism when an operation remains stuck and its expected completion event is never received.

For example:

```text
Is Any Operation In Flight
        ↓
True for an abnormal amount of time
        ↓
Reset Stuck Flags
        ↓
Query Inventory
```

> **Important:** This does not cancel a transaction on Myket. If a purchase was actually completed, it may still exist on the user's account. Query the inventory after recovery.

---

# Blueprint Events

## On Connection Succeeded

Called when the billing connection has been successfully established.

A common next step is:

```text
On Connection Succeeded
        ↓
Query Inventory
```

---

## On Connection Failed

Returns:

**Error Message**

Use this to handle connection errors or display an appropriate message.

---

## On Query Inventory Finished

Returns:

* `Success`
* `Message`
* `Products`
* `Purchases`

### Products

Each `FMyketProduct` contains:

| Field         | Description               |
| ------------- | ------------------------- |
| `Sku`         | Product identifier        |
| `Title`       | Product title             |
| `Description` | Product description       |
| `Price`       | Localized formatted price |
| `Type`        | Myket product type        |

`Products` is populated when SKU details are requested.

### Purchases

Each `FMyketPurchase` contains information about a purchase returned by Myket.

Use this array to restore or reconcile existing purchases.

---

## On Purchase Finished

Returns:

* `Success`
* `Message`
* `Purchase`

The `FMyketPurchase` structure contains:

| Field              | Description                                    |
| ------------------ | ---------------------------------------------- |
| `Sku`              | Purchased product identifier                   |
| `OrderId`          | Order identifier                               |
| `ItemType`         | Product type                                   |
| `DeveloperPayload` | Developer payload associated with the purchase |
| `OriginalJson`     | Original purchase data                         |
| `Signature`        | Purchase signature                             |
| `Token`            | Purchase token                                 |
| `PurchaseTime`     | Purchase timestamp                             |

---

## On Consume Finished

Returns:

* `Success`
* `Message`
* `Purchase`

Use the event result to determine whether the consumption request succeeded.

---

# Product Types

## In-App Products

Myket exposes in-app products through the `InApp` product type.

An in-app product can be used as either a consumable or a permanent purchase depending on how your game handles it.

### Consumable

Examples:

```text
coins_100
gems_500
energy_pack
```

Typical flow:

```text
Purchase
   ↓
Validate
   ↓
Deliver Item
   ↓
Consume
```

### Non-Consumable

Examples:

```text
remove_ads
premium_unlock
```

Do not consume these purchases.

Instead, restore their entitlement using `Query Inventory`.

---

## Subscriptions

Subscriptions are represented by the `Subscription` product type.

Examples:

```text
premium_monthly
premium_yearly
```

Do not consume subscription purchases.

Use inventory queries to restore the appropriate subscription entitlement.

---

# Purchase Restoration

A purchase system should not depend entirely on `On Purchase Finished`.

The application may be interrupted or terminated while a billing flow is active.

A recommended startup flow is:

```text
Game Start
    ↓
Open Connection
    ↓
On Connection Succeeded
    ↓
Query Inventory
    ↓
Process Purchases
    ↓
Restore Entitlements
```

This is especially important for:

* Non-consumable products
* Subscriptions
* Purchases interrupted by application termination

For consumables, your game should also make sure the same purchase cannot grant its contents multiple times.

---

# Purchase Verification

The plugin exposes the purchase information required for external verification:

* `OriginalJson`
* `Signature`

For valuable purchases, send the relevant purchase data to your backend and perform verification there.

Recommended architecture:

```text
Myket
   ↓
Purchase
   ↓
Unreal Client
   ↓
Your Backend
   ↓
Verify Purchase
   ↓
Grant Entitlement
```

Do not place private backend credentials or other sensitive verification secrets inside the Android client.

> **Important:** A successful client-side purchase callback should not be treated as sufficient protection for valuable currency or permanent entitlements.

---

# Developer Payload

`DeveloperPayload` is application-defined data associated with the purchase request.

Use it only when your application has a specific reason to associate additional information with a transaction.

Do not rely on the payload alone as proof that a purchase is valid.

For important entitlement checks, use the purchase information and appropriate verification.

---

# Preventing Duplicate Rewards

A purchase can be returned by both purchase callbacks and inventory queries.

Your entitlement system should therefore be able to recognize purchases that have already been processed.

Useful identifiers include:

* `OrderId`
* `Token`
* `Sku`

A safe flow is:

```text
Purchase Received
       ↓
Already Processed?
    ↙          ↘
  Yes           No
   ↓             ↓
Ignore        Verify
                 ↓
            Grant Item
                 ↓
        Mark As Processed
                 ↓
              Consume
```

Do not grant currency or other valuable content every time a purchase is returned without checking whether it has already been processed.

---

# Test Widget

The plugin includes an optional C++ test widget:

`MyketTestWidget`

It provides a simple on-device interface for testing:

* Connection
* Inventory queries
* Purchases
* Consumption
* Billing logs

The test widget is not required for production.

---

## Creating the Test Widget

1. Create a new **Widget Blueprint**.

2. Set its parent class to:

```text
Myket Test Widget
```

3. Add the following widgets with the exact names:

| Widget Name            | Type                         |
| ---------------------- | ---------------------------- |
| `ConnectButton`        | Button                       |
| `QueryInventoryButton` | Button                       |
| `BuyButton`            | Button                       |
| `ConsumeLastButton`    | Button                       |
| `SkuTextBox`           | Editable Text Box            |
| `PublicKeyTextBox`     | Editable Text Box            |
| `LogTextBox`           | Multi-Line Editable Text Box |

Set `LogTextBox` to read-only.

The test widget handles its own button events and billing delegates. No Blueprint event wiring is required.

---

## Test Widget Settings

The test widget exposes:

### Default In-App SKUs CSV

Comma-separated in-app SKUs.

Example:

```text
coins_100,remove_ads
```

### Default Subscription SKUs CSV

Comma-separated subscription SKUs.

Example:

```text
premium_monthly,premium_yearly
```

The first in-app SKU is automatically placed in the SKU input field when the widget is created.

---

## Displaying the Test Widget

Create and add the widget from a Blueprint:

```text
Event BeginPlay
      ↓
Create Widget
      ↓
Add to Viewport
```

Use your Widget Blueprint derived from `MyketTestWidget`.

---

## Testing With the Widget

On an Android device:

```text
Connect
   ↓
Query Inventory
   ↓
Buy
   ↓
Consume Last
```

The log field displays the results of the billing operations.

The `Consume Last` button operates on the most recent purchase stored by the test widget.

---

# Android Configuration

The plugin automatically adds the Android configuration required by the Myket billing integration during packaging.

This includes the required billing permission, Myket billing dependency, Java bridge, and GameActivity integration.

You should not need to manually add the Myket billing library to your Unreal project.

---

# Production Recommendations

### Connect Before Billing

Do not start purchase, query, or consume operations before the connection succeeds.

### Allow Only One Operation

Use `Is Any Operation In Flight` to prevent overlapping billing operations.

### Query on Startup

Query inventory after connecting so your game can restore and reconcile existing purchases.

### Consume Only Consumables

Do not consume permanent purchases or subscriptions.

### Deliver Before Consume

For consumables, grant the item before consuming the purchase.

### Prevent Duplicate Delivery

Store enough purchase information to recognize transactions that have already been processed.

### Verify Valuable Purchases

For valuable content, use server-side verification whenever possible.

### Handle Interrupted Transactions

If the application is interrupted during a billing operation, reconnect and query inventory when the game starts again.

---

# Troubleshooting

## Connection Fails

Check:

1. The application is running on Android.
2. Myket is available on the device.
3. The Android package name matches the application registered in Myket.
4. The correct public key is being used.
5. Products and subscriptions are correctly configured.
6. The device has network access.

---

## Purchase Does Not Start

Check:

1. `Is Connected` is `true`.
2. The SKU exactly matches the configured Myket product.
3. `Is Any Operation In Flight` is `false`.
4. Myket is available on the device.
5. You are testing a packaged Android build.

---

## Query and Consume Conflict

Only one asynchronous billing operation can run at a time.

Do not do this:

```text
Query Inventory
     ↓
Consume Purchase
```

before the query has finished.

Instead:

```text
Query Inventory
     ↓
On Query Inventory Finished
     ↓
Consume Purchase
```

And make sure `Is Any Operation In Flight` is `false` before starting the consume operation.

---

## Purchase Succeeds but the Item Is Missing After Restart

Do not rely only on `On Purchase Finished`.

On the next launch:

```text
Open Connection
      ↓
Query Inventory
      ↓
Process Purchases
      ↓
Restore Entitlement
```

---

## Operation Is Stuck

If an operation remains active and its completion event is never received:

1. Use `Reset Stuck Flags`.
2. Query the inventory again.
3. Reconcile the returned purchases with your entitlement system.

Resetting the local state does not cancel a transaction that may still exist on Myket.

---

# Shipping Checklist

* [ ] Myket application is configured.
* [ ] Android package name matches the registered application.
* [ ] Correct public key is being used.
* [ ] All product SKUs have been verified.
* [ ] Subscription SKUs have been verified if applicable.
* [ ] Connection flow has been tested.
* [ ] Inventory query has been tested.
* [ ] Successful purchases have been tested.
* [ ] Cancelled purchases have been tested.
* [ ] Failed purchases have been tested.
* [ ] Consumable purchases have been consumed successfully.
* [ ] Non-consumable purchases are not consumed.
* [ ] Subscriptions are not consumed.
* [ ] Purchase restoration has been tested after restarting the application.
* [ ] Interrupted billing flows have been tested.
* [ ] Duplicate purchase delivery is prevented.
* [ ] Valuable purchases are verified appropriately.
* [ ] The final packaged Android build has been tested on a real device.

---

# API Overview

| Node                         | Purpose                                         |
| ---------------------------- | ----------------------------------------------- |
| `Open Connection`            | Connect to Myket billing                        |
| `Query Inventory`            | Retrieve purchases and optional product details |
| `Launch Purchase Flow`       | Start an in-app purchase or subscription        |
| `Consume Purchase`           | Consume a consumable purchase                   |
| `Close Connection`           | Close the billing connection                    |
| `Is Platform Supported`      | Check platform support                          |
| `Get Connection State`       | Get the current connection state                |
| `Is Connected`               | Check whether billing is connected              |
| `Is Purchase In Flight`      | Check for an active purchase                    |
| `Is Query In Flight`         | Check for an active inventory query             |
| `Is Consume In Flight`       | Check for an active consumption operation       |
| `Is Any Operation In Flight` | Check whether any billing operation is active   |
| `Reset Stuck Flags`          | Recover from an abnormal stuck operation        |

### Events

| Event                         | Output                                        |
| ----------------------------- | --------------------------------------------- |
| `On Connection Succeeded`     | —                                             |
| `On Connection Failed`        | `Error Message`                               |
| `On Query Inventory Finished` | `Success`, `Message`, `Products`, `Purchases` |
| `On Purchase Finished`        | `Success`, `Message`, `Purchase`              |
| `On Consume Finished`         | `Success`, `Message`, `Purchase`              |

---

# License and Third-Party Components

This plugin integrates with the Myket billing client for Android.

The Myket billing client is a separate third-party component and is subject to its own license and terms.

Refer to Myket's official developer documentation for the current billing requirements and service terms.
