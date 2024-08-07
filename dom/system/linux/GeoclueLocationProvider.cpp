/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Author: Maciej S. Szmigiero <mail@maciej.szmigiero.name>
 */

#include "GeoclueLocationProvider.h"

#include <gio/gio.h>
#include <glib.h>
#include "mozilla/FloatingPoint.h"
#include "mozilla/GRefPtr.h"
#include "mozilla/GUniquePtr.h"
#include "mozilla/Logging.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/StaticPrefs_geo.h"
#include "mozilla/UniquePtrExtensions.h"
#include "mozilla/WeakPtr.h"
#include "mozilla/XREAppData.h"
#include "mozilla/dom/GeolocationPosition.h"
#include "mozilla/dom/GeolocationPositionErrorBinding.h"
#include "MLSFallback.h"
#include "nsAppRunner.h"
#include "nsCOMPtr.h"
#include "nsIDOMGeoPosition.h"
#include "nsINamed.h"
#include "nsITimer.h"
#include "nsStringFwd.h"
#include "prtime.h"

namespace mozilla::dom {

static LazyLogModule gGCLocationLog("GeoclueLocation");

#define GCL_LOG(level, ...) \
  MOZ_LOG(gGCLocationLog, mozilla::LogLevel::level, (__VA_ARGS__))

static const char* const kGeoclueBusName = "org.freedesktop.GeoClue2";
static const char* const kGCManagerPath = "/org/freedesktop/GeoClue2/Manager";
static const char* const kGCManagerInterface =
    "org.freedesktop.GeoClue2.Manager";
static const char* const kGCClientInterface = "org.freedesktop.GeoClue2.Client";
static const char* const kGCLocationInterface =
    "org.freedesktop.GeoClue2.Location";
static const char* const kDBPropertySetMethod =
    "org.freedesktop.DBus.Properties.Set";

/*
 * Minimum altitude reported as valid (in meters),
 * https://en.wikipedia.org/wiki/List_of_places_on_land_with_elevations_below_sea_level
 * says that lowest land in the world is at -430 m, so let's use -500 m here.
 */
static const double kGCMinAlt = -500;

/*
 * Matches "enum GClueAccuracyLevel" values, see:
 * https://www.freedesktop.org/software/geoclue/docs/geoclue-gclue-enums.html#GClueAccuracyLevel
 */
enum class GCAccuracyLevel {
  None = 0,
  Country = 1,
  City = 4,
  Neighborhood = 5,
  Street = 6,
  Exact = 8,
};

/*
 * Whether to reuse D-Bus proxies between uses of this provider.
 * Usually a good thing, can be disabled for debug purposes.
 */
static const bool kGCReuseDBusProxy = true;

class GCLocProviderPriv final : public nsIGeolocationProvider,
                                public SupportsWeakPtr {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIGEOLOCATIONPROVIDER

  GCLocProviderPriv();

 private:
  enum class Accuracy { Unset, Low, High };
  // States:
  //   Uninit: The default / initial state, with no client proxy yet.
  //   Initing: Takes care of establishing the client connection (GetClient /
  //            ConnectClient / SetDesktopID).
  //   SettingAccuracy: Does SetAccuracy operation, knows it should just go idle
  //                    after finishing it.
  //   SettingAccuracyForStart: Does SetAccuracy operation, knows it then needs
  //                            to do a Start operation after finishing it.
  //   Idle: Fully initialized, but not running state (quiescent).
  //   Starting: Starts the client by calling the Start D-Bus method.
  //   Started: Normal running state.
  //   Stopping: Stops the client by calling the Stop D-Bus method, knows it
  //             should just go idle after finishing it.
  //   StoppingForRestart: Stops the client by calling the Stop D-Bus method as
  //                       a part of a Stop -> Start sequence (with possibly
  //                       an accuracy update between these method calls).
  //
  // Valid state transitions are:
  //  (any state) -> Uninit: Transition when a D-Bus call failed or
  //                         provided invalid data.
  //
  //   Watch() startup path:
  //   Uninit -> Initing: Transition after getting the very first Watch()
  //   request
  //                      or any such request while not having the client proxy.
  //   Initing -> SettingAccuracyForStart: Transition after getting a successful
  //                                       SetDesktopID response.
  //   SettingAccuracyForStart -> Starting: Transition after getting a
  //   successful
  //                                        SetAccuracy response.
  //   Idle -> Starting: Transition after getting a Watch() request while in
  //   fully
  //                     initialized, but not running state.
  //   SettingAccuracy -> SettingAccuracyForStart: Transition after getting a
  //   Watch()
  //                                               request in the middle of
  //                                               setting accuracy during idle
  //                                               status.
  //   Stopping -> StoppingForRestart: Transition after getting a Watch()
  //   request
  //                                   in the middle of doing a Stop D-Bus call
  //                                   for idle status.
  //   StoppingForRestart -> Starting: Transition after getting a successful
  //                                   Stop response as a part of a Stop ->
  //                                   Start sequence while the previously set
  //                                   accuracy is still correct.
  //   StoppingForRestart -> SettingAccuracyForStart: Transition after getting
  //                                                  a successful Stop response
  //                                                  as a part of a Stop ->
  //                                                  Start sequence but the set
  //                                                  accuracy needs updating.
  //   Starting -> Started: Transition after getting a successful Start
  //   response.
  //
  //   Shutdown() path:
  //   (any state) -> Uninit: Transition when not reusing the client proxy for
  //                          any reason.
  //   Started -> Stopping: Transition from normal running state when reusing
  //                        the client proxy.
  //   SettingAccuracyForStart -> SettingAccuracy: Transition when doing
  //                                               a shutdown in the middle of
  //                                               setting accuracy for a start
  //                                               when reusing the client
  //                                               proxy.
  //   SettingAccuracy -> Idle: Transition after getting a successful
  //   SetAccuracy
  //                            response.
  //   StoppingForRestart -> Stopping: Transition when doing shutdown
  //                                   in the middle of a Stop -> Start sequence
  //                                   when reusing the client proxy.
  //   Stopping -> Idle: Transition after getting a successful Stop response.
  //
  //   SetHighAccuracy() path:
  //   Started -> StoppingForRestart: Transition when accuracy needs updating
  //                                  on a running client.
  //   (the rest of the flow in StoppingForRestart state is the same as when
  //    being in this state in the Watch() startup path)
  enum class ClientState {
    Uninit,
    Initing,
    SettingAccuracy,
    SettingAccuracyForStart,
    Idle,
    Starting,
    Started,
    Stopping,
    StoppingForRestart
  };

  ~GCLocProviderPriv();

  static bool AlwaysHighAccuracy();

  void SetState(ClientState aNewState, const char* aNewStateStr);

  void Update(nsIDOMGeoPosition* aPosition);
  MOZ_CAN_RUN_SCRIPT void NotifyError(int aError);
  MOZ_CAN_RUN_SCRIPT void DBusProxyError(const GError* aGError,
                                         bool aResetManager = false);

  MOZ_CAN_RUN_SCRIPT static void GetClientResponse(GDBusProxy* aProxy,
                                                   GAsyncResult* aResult,
                                                   gpointer aUserData);
  void ConnectClient(const gchar* aClientPath);
  MOZ_CAN_RUN_SCRIPT static void ConnectClientResponse(GObject* aObject,
                                                       GAsyncResult* aResult,
                                                       gpointer aUserData);
  void SetDesktopID();
  MOZ_CAN_RUN_SCRIPT static void SetDesktopIDResponse(GDBusProxy* aProxy,
                                                      GAsyncResult* aResult,
                                                      gpointer aUserData);
  void SetAccuracy();
  MOZ_CAN_RUN_SCRIPT static void SetAccuracyResponse(GDBusProxy* aProxy,
                                                     GAsyncResult* aResult,
                                                     gpointer aUserData);
  void StartClient();
  MOZ_CAN_RUN_SCRIPT static void StartClientResponse(GDBusProxy* aProxy,
                                                     GAsyncResult* aResult,
                                                     gpointer aUserData);
  void StopClient(bool aForRestart);
  MOZ_CAN_RUN_SCRIPT static void StopClientResponse(GDBusProxy* aProxy,
                                                    GAsyncResult* aResult,
                                                    gpointer aUserData);
  void StopClientNoWait();
  void MaybeRestartForAccuracy();

  MOZ_CAN_RUN_SCRIPT static void GCManagerOwnerNotify(GObject* aObject,
                                                      GParamSpec* aPSpec,
                                                      gpointer aUserData);
  static void GCClientSignal(GDBusProxy* aProxy, gchar* aSenderName,
                             gchar* aSignalName, GVariant* aParameters,
                             gpointer aUserData);
  void ConnectLocation(const gchar* aLocationPath);
  static bool GetLocationProperty(GDBusProxy* aProxyLocation,
                                  const gchar* aName, double* aOut);
  static void ConnectLocationResponse(GObject* aObject, GAsyncResult* aResult,
                                      gpointer aUserData);

  void StartLastPositionTimer();
  void StopPositionTimer();
  void UpdateLastPosition();

  void StartMLSFallbackTimerIfNeeded();
  void StopMLSFallbackTimer();
  void MLSFallbackTimerFired();

  bool InDBusCall();
  bool InDBusStoppingCall();
  bool InDBusStoppedCall();

  void DeleteManager();
  void DoShutdown(bool aDeleteClient, bool aDeleteManager);
  void DoShutdownClearCallback(bool aDestroying);

  nsresult FallbackToMLS();
  void StopMLSFallback();

  void WatchStart();

  Accuracy mAccuracyWanted = Accuracy::Unset;
  Accuracy mAccuracySet = Accuracy::Unset;
  RefPtr<GDBusProxy> mProxyManager;
  RefPtr<GDBusProxy> mProxyClient;
  RefPtr<GCancellable> mCancellable;
  nsCOMPtr<nsIGeolocationUpdate> mCallback;
  ClientState mClientState = ClientState::Uninit;
  RefPtr<nsIDOMGeoPosition> mLastPosition;
  RefPtr<nsITimer> mLastPositionTimer;
  RefPtr<nsITimer> mMLSFallbackTimer;
  RefPtr<MLSFallback> mMLSFallback;
};

class GCLocWeakCallback final : public nsITimerCallback, public nsINamed {
  using Method = void (GCLocProviderPriv::*)();

 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSITIMERCALLBACK

  explicit GCLocWeakCallback(GCLocProviderPriv* aParent, const char* aName,
                             Method aMethod)
      : mParent(aParent), mName(aName), mMethod(aMethod) {}

  NS_IMETHOD GetName(nsACString& aName) override {
    aName = mName;
    return NS_OK;
  }

 private:
  ~GCLocWeakCallback() = default;
  WeakPtr<GCLocProviderPriv> mParent;
  const char* mName = nullptr;
  Method mMethod = nullptr;
};

NS_IMPL_ISUPPORTS(GCLocWeakCallback, nsITimerCallback, nsINamed)

NS_IMETHODIMP
GCLocWeakCallback::Notify(nsITimer* aTimer) {
  if (RefPtr parent = mParent.get()) {
    (parent->*mMethod)();
  }
  return NS_OK;
}

//
// GCLocProviderPriv
//

#define GCLP_SETSTATE(this, state) this->SetState(ClientState::state, #state)

GCLocProviderPriv::GCLocProviderPriv() {
  if (AlwaysHighAccuracy()) {
    mAccuracyWanted = Accuracy::High;
  } else {
    mAccuracyWanted = Accuracy::Low;
  }
}

GCLocProviderPriv::~GCLocProviderPriv() { DoShutdownClearCallback(true); }

bool GCLocProviderPriv::AlwaysHighAccuracy() {
  return StaticPrefs::geo_provider_geoclue_always_high_accuracy();
}

void GCLocProviderPriv::SetState(ClientState aNewState,
                                 const char* aNewStateStr) {
  if (mClientState == aNewState) {
    return;
  }

  GCL_LOG(Debug, "changing state to %s", aNewStateStr);
  mClientState = aNewState;
}

void GCLocProviderPriv::Update(nsIDOMGeoPosition* aPosition) {
  if (!mCallback) {
    return;
  }

  mCallback->Update(aPosition);
}

void GCLocProviderPriv::UpdateLastPosition() {
  MOZ_DIAGNOSTIC_ASSERT(mLastPosition, "No last position to update");
  StopPositionTimer();
  StopMLSFallbackTimer();
  Update(mLastPosition);
}

nsresult GCLocProviderPriv::FallbackToMLS() {
  GCL_LOG(Debug, "trying to fall back to MLS");
  StopMLSFallback();

  RefPtr fallback = new MLSFallback(0);
  MOZ_TRY(fallback->Startup(mCallback));

  GCL_LOG(Debug, "Started up MLS fallback");
  mMLSFallback = std::move(fallback);
  return NS_OK;
}

void GCLocProviderPriv::StopMLSFallback() {
  if (!mMLSFallback) {
    return;
  }
  GCL_LOG(Debug, "Clearing MLS fallback");
  if (mMLSFallback) {
    mMLSFallback->Shutdown();
    mMLSFallback = nullptr;
  }
}

void GCLocProviderPriv::NotifyError(int aError) {
  if (!mCallback) {
    return;
  }

  // We errored out, try to fall back to MLS.
  if (NS_SUCCEEDED(FallbackToMLS())) {
    return;
  }

  nsCOMPtr callback = mCallback;
  callback->NotifyError(aError);
}

void GCLocProviderPriv::DBusProxyError(const GError* aGError,
                                       bool aResetManager) {
  // that G_DBUS_ERROR below is actually a function call, not a constant
  GQuark gdbusDomain = G_DBUS_ERROR;
  int error = GeolocationPositionError_Binding::POSITION_UNAVAILABLE;
  if (aGError) {
    if (g_error_matches(aGError, gdbusDomain, G_DBUS_ERROR_TIMEOUT) ||
        g_error_matches(aGError, gdbusDomain, G_DBUS_ERROR_TIMED_OUT)) {
      error = GeolocationPositionError_Binding::TIMEOUT;
    } else if (g_error_matches(aGError, gdbusDomain,
                               G_DBUS_ERROR_LIMITS_EXCEEDED) ||
               g_error_matches(aGError, gdbusDomain,
                               G_DBUS_ERROR_ACCESS_DENIED) ||
               g_error_matches(aGError, gdbusDomain,
                               G_DBUS_ERROR_AUTH_FAILED)) {
      error = GeolocationPositionError_Binding::PERMISSION_DENIED;
    }
  }

  DoShutdown(true, aResetManager);
  NotifyError(error);
}

void GCLocProviderPriv::GetClientResponse(GDBusProxy* aProxy,
                                          GAsyncResult* aResult,
                                          gpointer aUserData) {
  GUniquePtr<GError> error;
  RefPtr<GVariant> variant = dont_AddRef(
      g_dbus_proxy_call_finish(aProxy, aResult, getter_Transfers(error)));
  if (!variant) {
    GCL_LOG(Error, "Failed to get client: %s\n", error->message);
    // if cancelled |self| might no longer be there
    if (!g_error_matches(error.get(), G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      RefPtr self = static_cast<GCLocProviderPriv*>(aUserData);
      self->DBusProxyError(error.get(), true);
    }
    return;
  }

  RefPtr self = static_cast<GCLocProviderPriv*>(aUserData);
  MOZ_DIAGNOSTIC_ASSERT(self->mClientState == ClientState::Initing,
                        "Client in a wrong state");

  auto signalError = MakeScopeExit([&]() MOZ_CAN_RUN_SCRIPT_BOUNDARY {
    self->DBusProxyError(nullptr, true);
  });

  if (!g_variant_is_of_type(variant, G_VARIANT_TYPE_TUPLE)) {
    GCL_LOG(Error, "Unexpected get client call return type: %s\n",
            g_variant_get_type_string(variant));
    return;
  }

  if (g_variant_n_children(variant) < 1) {
    GCL_LOG(Error,
            "Not enough params in get client call return: %" G_GSIZE_FORMAT
            "\n",
            g_variant_n_children(variant));
    return;
  }

  variant = dont_AddRef(g_variant_get_child_value(variant, 0));
  if (!g_variant_is_of_type(variant, G_VARIANT_TYPE_OBJECT_PATH)) {
    GCL_LOG(Error, "Unexpected get client call return type inside tuple: %s\n",
            g_variant_get_type_string(variant));
    return;
  }

  const gchar* clientPath = g_variant_get_string(variant, nullptr);
  GCL_LOG(Debug, "Client path: %s\n", clientPath);

  signalError.release();
  self->ConnectClient(clientPath);
}

void GCLocProviderPriv::ConnectClient(const gchar* aClientPath) {
  MOZ_DIAGNOSTIC_ASSERT(mClientState == ClientState::Initing,
                        "Client in a wrong state");
  MOZ_ASSERT(mCancellable, "Watch() wasn't successfully called");
  g_dbus_proxy_new_for_bus(
      G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr, kGeoclueBusName,
      aClientPath, kGCClientInterface, mCancellable,
      reinterpret_cast<GAsyncReadyCallback>(ConnectClientResponse), this);
}

void GCLocProviderPriv::ConnectClientResponse(GObject* aObject,
                                              GAsyncResult* aResult,
                                              gpointer aUserData) {
  GUniquePtr<GError> error;
  RefPtr<GDBusProxy> proxyClient =
      dont_AddRef(g_dbus_proxy_new_finish(aResult, getter_Transfers(error)));
  if (!proxyClient) {
    // if cancelled |self| might no longer be there
    if (!g_error_matches(error.get(), G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      GCL_LOG(Error, "Failed to connect to client: %s\n", error->message);
      RefPtr self = static_cast<GCLocProviderPriv*>(aUserData);
      self->DBusProxyError(error.get());
    }
    return;
  }
  RefPtr self = static_cast<GCLocProviderPriv*>(aUserData);
  self->mProxyClient = std::move(proxyClient);

  MOZ_DIAGNOSTIC_ASSERT(self->mClientState == ClientState::Initing,
                        "Client in a wrong state");

  GCL_LOG(Info, "Client interface connected\n");

  g_signal_connect(self->mProxyClient, "g-signal", G_CALLBACK(GCClientSignal),
                   self);
  self->SetDesktopID();
}

void GCLocProviderPriv::SetDesktopID() {
  MOZ_DIAGNOSTIC_ASSERT(mClientState == ClientState::Initing,
                        "Client in a wrong state");
  MOZ_DIAGNOSTIC_ASSERT(mProxyClient && mCancellable,
                        "Watch() wasn't successfully called");

  nsAutoCString appName;
  gAppData->GetDBusAppName(appName);
  g_dbus_proxy_call(mProxyClient, kDBPropertySetMethod,
                    g_variant_new("(ssv)", kGCClientInterface, "DesktopId",
                                  g_variant_new_string(appName.get())),
                    G_DBUS_CALL_FLAGS_NONE, -1, mCancellable,
                    reinterpret_cast<GAsyncReadyCallback>(SetDesktopIDResponse),
                    this);
}

void GCLocProviderPriv::SetDesktopIDResponse(GDBusProxy* aProxy,
                                             GAsyncResult* aResult,
                                             gpointer aUserData) {
  GUniquePtr<GError> error;

  RefPtr<GVariant> variant = dont_AddRef(
      g_dbus_proxy_call_finish(aProxy, aResult, getter_Transfers(error)));
  if (!variant) {
    // if cancelled |self| might no longer be there
    if (!g_error_matches(error.get(), G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      GCL_LOG(Error, "Failed to set DesktopId: %s\n", error->message);
      RefPtr self = static_cast<GCLocProviderPriv*>(aUserData);
      self->DBusProxyError(error.get());
    }
    return;
  }

  RefPtr self = static_cast<GCLocProviderPriv*>(aUserData);
  MOZ_DIAGNOSTIC_ASSERT(self->mClientState == ClientState::Initing,
                        "Client in a wrong state");

  GCLP_SETSTATE(self, Idle);
  self->SetAccuracy();
}

void GCLocProviderPriv::SetAccuracy() {
  MOZ_DIAGNOSTIC_ASSERT(mClientState == ClientState::Idle,
                        "Client in a wrong state");
  MOZ_DIAGNOSTIC_ASSERT(mProxyClient && mCancellable,
                        "Watch() wasn't successfully called");
  MOZ_ASSERT(mAccuracyWanted != Accuracy::Unset, "Invalid accuracy");

  guint32 accuracy;
  if (mAccuracyWanted == Accuracy::High) {
    accuracy = (guint32)GCAccuracyLevel::Exact;
  } else {
    accuracy = (guint32)GCAccuracyLevel::City;
  }

  mAccuracySet = mAccuracyWanted;
  GCLP_SETSTATE(this, SettingAccuracyForStart);
  g_dbus_proxy_call(
      mProxyClient, kDBPropertySetMethod,
      g_variant_new("(ssv)", kGCClientInterface, "RequestedAccuracyLevel",
                    g_variant_new_uint32(accuracy)),
      G_DBUS_CALL_FLAGS_NONE, -1, mCancellable,
      reinterpret_cast<GAsyncReadyCallback>(SetAccuracyResponse), this);
}

void GCLocProviderPriv::SetAccuracyResponse(GDBusProxy* aProxy,
                                            GAsyncResult* aResult,
                                            gpointer aUserData) {
  GUniquePtr<GError> error;
  RefPtr<GVariant> variant = dont_AddRef(
      g_dbus_proxy_call_finish(aProxy, aResult, getter_Transfers(error)));
  if (!variant) {
    // if cancelled |self| might no longer be there
    if (!g_error_matches(error.get(), G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      GCL_LOG(Error, "Failed to set requested accuracy level: %s\n",
              error->message);
      RefPtr self = static_cast<GCLocProviderPriv*>(aUserData);
      self->DBusProxyError(error.get());
    }
    return;
  }

  RefPtr self = static_cast<GCLocProviderPriv*>(aUserData);
  MOZ_DIAGNOSTIC_ASSERT(
      self->mClientState == ClientState::SettingAccuracyForStart ||
          self->mClientState == ClientState::SettingAccuracy,
      "Client in a wrong state");
  bool wantStart = self->mClientState == ClientState::SettingAccuracyForStart;
  GCLP_SETSTATE(self, Idle);

  if (wantStart) {
    self->StartClient();
  }
}

void GCLocProviderPriv::StartClient() {
  MOZ_DIAGNOSTIC_ASSERT(mClientState == ClientState::Idle,
                        "Client in a wrong state");
  MOZ_DIAGNOSTIC_ASSERT(mProxyClient && mCancellable,
                        "Watch() wasn't successfully called");
  GCLP_SETSTATE(this, Starting);
  g_dbus_proxy_call(
      mProxyClient, "Start", nullptr, G_DBUS_CALL_FLAGS_NONE, -1, mCancellable,
      reinterpret_cast<GAsyncReadyCallback>(StartClientResponse), this);
}

void GCLocProviderPriv::StartClientResponse(GDBusProxy* aProxy,
                                            GAsyncResult* aResult,
                                            gpointer aUserData) {
  GUniquePtr<GError> error;

  RefPtr<GVariant> variant = dont_AddRef(
      g_dbus_proxy_call_finish(aProxy, aResult, getter_Transfers(error)));
  if (!variant) {
    // if cancelled |self| might no longer be there
    if (!g_error_matches(error.get(), G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      GCL_LOG(Error, "Failed to start client: %s\n", error->message);
      RefPtr self = static_cast<GCLocProviderPriv*>(aUserData);
      /*
       * A workaround for
       * https://gitlab.freedesktop.org/geoclue/geoclue/-/issues/143 We need to
       * get a new client instance once the agent finally connects to the
       * Geoclue service, otherwise every Start request on the old client
       * interface will be denied. We need to reconnect to the Manager interface
       * to achieve this since otherwise GetClient call will simply return the
       * old client instance.
       */
      bool resetManager = g_error_matches(error.get(), G_DBUS_ERROR,
                                          G_DBUS_ERROR_ACCESS_DENIED);
      self->DBusProxyError(error.get(), resetManager);
    }
    return;
  }

  RefPtr self = static_cast<GCLocProviderPriv*>(aUserData);
  MOZ_DIAGNOSTIC_ASSERT(self->mClientState == ClientState::Starting,
                        "Client in a wrong state");
  GCLP_SETSTATE(self, Started);
  // If we're started, and we don't get any location update in a reasonable
  // amount of time, we fallback to MLS.
  self->StartMLSFallbackTimerIfNeeded();
  self->MaybeRestartForAccuracy();
}

void GCLocProviderPriv::StopClient(bool aForRestart) {
  MOZ_DIAGNOSTIC_ASSERT(mClientState == ClientState::Started,
                        "Client in a wrong state");
  MOZ_DIAGNOSTIC_ASSERT(mProxyClient && mCancellable,
                        "Watch() wasn't successfully called");

  if (aForRestart) {
    GCLP_SETSTATE(this, StoppingForRestart);
  } else {
    GCLP_SETSTATE(this, Stopping);
  }

  g_dbus_proxy_call(
      mProxyClient, "Stop", nullptr, G_DBUS_CALL_FLAGS_NONE, -1, mCancellable,
      reinterpret_cast<GAsyncReadyCallback>(StopClientResponse), this);
}

void GCLocProviderPriv::StopClientResponse(GDBusProxy* aProxy,
                                           GAsyncResult* aResult,
                                           gpointer aUserData) {
  GUniquePtr<GError> error;
  RefPtr<GVariant> variant = dont_AddRef(
      g_dbus_proxy_call_finish(aProxy, aResult, getter_Transfers(error)));
  if (!variant) {
    // if cancelled |self| might no longer be there
    if (!g_error_matches(error.get(), G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      GCL_LOG(Error, "Failed to stop client: %s\n", error->message);
      RefPtr self = static_cast<GCLocProviderPriv*>(aUserData);
      self->DBusProxyError(error.get());
    }
    return;
  }

  RefPtr self = static_cast<GCLocProviderPriv*>(aUserData);
  MOZ_DIAGNOSTIC_ASSERT(self->InDBusStoppingCall(), "Client in a wrong state");
  bool wantRestart = self->mClientState == ClientState::StoppingForRestart;
  GCLP_SETSTATE(self, Idle);

  if (!wantRestart) {
    return;
  }

  if (self->mAccuracyWanted != self->mAccuracySet) {
    self->SetAccuracy();
  } else {
    self->StartClient();
  }
}

void GCLocProviderPriv::StopClientNoWait() {
  MOZ_DIAGNOSTIC_ASSERT(mProxyClient, "Watch() wasn't successfully called");
  g_dbus_proxy_call(mProxyClient, "Stop", nullptr, G_DBUS_CALL_FLAGS_NONE, -1,
                    nullptr, nullptr, nullptr);
}

void GCLocProviderPriv::MaybeRestartForAccuracy() {
  if (mAccuracyWanted == mAccuracySet) {
    return;
  }

  if (mClientState != ClientState::Started) {
    return;
  }

  // Setting a new accuracy requires restarting the client
  StopClient(true);
}

void GCLocProviderPriv::GCManagerOwnerNotify(GObject* aObject,
                                             GParamSpec* aPSpec,
                                             gpointer aUserData) {
  RefPtr self = static_cast<GCLocProviderPriv*>(aUserData);
  GUniquePtr<gchar> managerOwner(
      g_dbus_proxy_get_name_owner(self->mProxyManager));
  if (!managerOwner) {
    GCL_LOG(Info, "The Manager interface has lost its owner\n");
    self->DBusProxyError(nullptr, true);
  }
}

void GCLocProviderPriv::GCClientSignal(GDBusProxy* aProxy, gchar* aSenderName,
                                       gchar* aSignalName,
                                       GVariant* aParameters,
                                       gpointer aUserData) {
  GCL_LOG(Info, "%s: %s (%s)\n", __PRETTY_FUNCTION__, aSignalName,
          GUniquePtr<gchar>(g_variant_print(aParameters, TRUE)).get());

  if (g_strcmp0(aSignalName, "LocationUpdated")) {
    return;
  }

  if (!g_variant_is_of_type(aParameters, G_VARIANT_TYPE_TUPLE)) {
    GCL_LOG(Error, "Unexpected location updated signal params type: %s\n",
            g_variant_get_type_string(aParameters));
    return;
  }

  if (g_variant_n_children(aParameters) < 2) {
    GCL_LOG(Error,
            "Not enough params in location updated signal: %" G_GSIZE_FORMAT
            "\n",
            g_variant_n_children(aParameters));
    return;
  }

  RefPtr<GVariant> variant =
      dont_AddRef(g_variant_get_child_value(aParameters, 1));
  if (!g_variant_is_of_type(variant, G_VARIANT_TYPE_OBJECT_PATH)) {
    GCL_LOG(Error,
            "Unexpected location updated signal new location path type: %s\n",
            g_variant_get_type_string(variant));
    return;
  }

  RefPtr self = static_cast<GCLocProviderPriv*>(aUserData);
  const gchar* locationPath = g_variant_get_string(variant, nullptr);
  GCL_LOG(Verbose, "New location path: %s\n", locationPath);
  self->ConnectLocation(locationPath);
}

void GCLocProviderPriv::ConnectLocation(const gchar* aLocationPath) {
  MOZ_ASSERT(mCancellable, "Startup() wasn't successfully called");
  g_dbus_proxy_new_for_bus(
      G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr, kGeoclueBusName,
      aLocationPath, kGCLocationInterface, mCancellable,
      reinterpret_cast<GAsyncReadyCallback>(ConnectLocationResponse), this);
}

bool GCLocProviderPriv::GetLocationProperty(GDBusProxy* aProxyLocation,
                                            const gchar* aName, double* aOut) {
  RefPtr<GVariant> property =
      dont_AddRef(g_dbus_proxy_get_cached_property(aProxyLocation, aName));
  if (!g_variant_is_of_type(property, G_VARIANT_TYPE_DOUBLE)) {
    GCL_LOG(Error, "Unexpected location property %s type: %s\n", aName,
            g_variant_get_type_string(property));
    return false;
  }

  *aOut = g_variant_get_double(property);
  return true;
}

void GCLocProviderPriv::ConnectLocationResponse(GObject* aObject,
                                                GAsyncResult* aResult,
                                                gpointer aUserData) {
  GUniquePtr<GError> error;
  RefPtr<GDBusProxy> proxyLocation =
      dont_AddRef(g_dbus_proxy_new_finish(aResult, getter_Transfers(error)));
  if (!proxyLocation) {
    if (!g_error_matches(error.get(), G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      GCL_LOG(Warning, "Failed to connect to location: %s\n", error->message);
    }
    return;
  }

  RefPtr self = static_cast<GCLocProviderPriv*>(aUserData);
  /*
   * nsGeoPositionCoords will convert NaNs to null for optional properties of
   * the JavaScript Coordinates object.
   */
  double lat = UnspecifiedNaN<double>();
  double lon = UnspecifiedNaN<double>();
  double alt = UnspecifiedNaN<double>();
  double hError = UnspecifiedNaN<double>();
  const double vError = UnspecifiedNaN<double>();
  double heading = UnspecifiedNaN<double>();
  double speed = UnspecifiedNaN<double>();
  struct {
    const gchar* name;
    double* out;
  } props[] = {
      {"Latitude", &lat},    {"Longitude", &lon},   {"Altitude", &alt},
      {"Accuracy", &hError}, {"Heading", &heading}, {"Speed", &speed},
  };

  for (auto& prop : props) {
    if (!GetLocationProperty(proxyLocation, prop.name, prop.out)) {
      return;
    }
  }

  if (alt < kGCMinAlt) {
    alt = UnspecifiedNaN<double>();
  }
  if (speed < 0) {
    speed = UnspecifiedNaN<double>();
  }
  if (heading < 0 || std::isnan(speed) || speed == 0) {
    heading = UnspecifiedNaN<double>();
  }

  GCL_LOG(Info, "New location: %f %f +-%fm @ %gm; hdg %f spd %fm/s\n", lat, lon,
          hError, alt, heading, speed);

  self->mLastPosition =
      new nsGeoPosition(lat, lon, alt, hError, vError, heading, speed,
                        PR_Now() / PR_USEC_PER_MSEC);
  self->UpdateLastPosition();
}

void GCLocProviderPriv::StartLastPositionTimer() {
  MOZ_DIAGNOSTIC_ASSERT(mLastPosition, "no last position to report");

  StopPositionTimer();

  RefPtr timerCallback = new GCLocWeakCallback(
      this, "UpdateLastPosition", &GCLocProviderPriv::UpdateLastPosition);
  NS_NewTimerWithCallback(getter_AddRefs(mLastPositionTimer), timerCallback,
                          1000, nsITimer::TYPE_ONE_SHOT);
}

void GCLocProviderPriv::StopPositionTimer() {
  if (!mLastPositionTimer) {
    return;
  }

  mLastPositionTimer->Cancel();
  mLastPositionTimer = nullptr;
}

void GCLocProviderPriv::StartMLSFallbackTimerIfNeeded() {
  StopMLSFallbackTimer();
  if (mLastPosition) {
    // If we already have a location we're good.
    return;
  }

  uint32_t delay = StaticPrefs::geo_provider_geoclue_mls_fallback_timeout_ms();
  if (!delay) {
    return;
  }

  RefPtr timerCallback = new GCLocWeakCallback(
      this, "MLSFallbackTimerFired", &GCLocProviderPriv::MLSFallbackTimerFired);
  NS_NewTimerWithCallback(getter_AddRefs(mMLSFallbackTimer), timerCallback,
                          delay, nsITimer::TYPE_ONE_SHOT);
}

void GCLocProviderPriv::StopMLSFallbackTimer() {
  if (!mMLSFallbackTimer) {
    return;
  }
  mMLSFallbackTimer->Cancel();
  mMLSFallbackTimer = nullptr;
}

void GCLocProviderPriv::MLSFallbackTimerFired() {
  mMLSFallbackTimer = nullptr;

  if (mMLSFallback || mLastPosition || mClientState != ClientState::Started) {
    return;
  }

  GCL_LOG(Info,
          "Didn't get a location in a reasonable amount of time, trying to "
          "fall back to MLS");
  FallbackToMLS();
}

// Did we made some D-Bus call and are still waiting for its response?
bool GCLocProviderPriv::InDBusCall() {
  return mClientState == ClientState::Initing ||
         mClientState == ClientState::SettingAccuracy ||
         mClientState == ClientState::SettingAccuracyForStart ||
         mClientState == ClientState::Starting ||
         mClientState == ClientState::Stopping ||
         mClientState == ClientState::StoppingForRestart;
}

bool GCLocProviderPriv::InDBusStoppingCall() {
  return mClientState == ClientState::Stopping ||
         mClientState == ClientState::StoppingForRestart;
}

/*
 * Did we made some D-Bus call while stopped and
 * are still waiting for its response?
 */
bool GCLocProviderPriv::InDBusStoppedCall() {
  return mClientState == ClientState::SettingAccuracy ||
         mClientState == ClientState::SettingAccuracyForStart;
}

void GCLocProviderPriv::DeleteManager() {
  if (!mProxyManager) {
    return;
  }

  g_signal_handlers_disconnect_matched(mProxyManager, G_SIGNAL_MATCH_DATA, 0, 0,
                                       nullptr, nullptr, this);
  mProxyManager = nullptr;
}

void GCLocProviderPriv::DoShutdown(bool aDeleteClient, bool aDeleteManager) {
  MOZ_DIAGNOSTIC_ASSERT(
      !aDeleteManager || aDeleteClient,
      "deleting manager proxy requires deleting client one, too");

  // Invalidate the cached last position
  StopPositionTimer();
  StopMLSFallbackTimer();
  mLastPosition = nullptr;

  /*
   * Do we need to delete the D-Bus proxy (or proxies)?
   * Either because that's what our caller wanted, or because we are set to
   * never reuse them, or because we are in a middle of some D-Bus call while
   * having the service running (and so not being able to issue an immediate
   * Stop call).
   */
  if (aDeleteClient || !kGCReuseDBusProxy ||
      (InDBusCall() && !InDBusStoppingCall() && !InDBusStoppedCall())) {
    if (mClientState == ClientState::Started) {
      StopClientNoWait();
      GCLP_SETSTATE(this, Idle);
    }
    if (mProxyClient) {
      g_signal_handlers_disconnect_matched(mProxyClient, G_SIGNAL_MATCH_DATA, 0,
                                           0, nullptr, nullptr, this);
    }
    if (mCancellable) {
      g_cancellable_cancel(mCancellable);
      mCancellable = nullptr;
    }
    mProxyClient = nullptr;

    if (aDeleteManager || !kGCReuseDBusProxy) {
      DeleteManager();
    }

    GCLP_SETSTATE(this, Uninit);
  } else if (mClientState == ClientState::Started) {
    StopClient(false);
  } else if (mClientState == ClientState::SettingAccuracyForStart) {
    GCLP_SETSTATE(this, SettingAccuracy);
  } else if (mClientState == ClientState::StoppingForRestart) {
    GCLP_SETSTATE(this, Stopping);
  }
}

void GCLocProviderPriv::DoShutdownClearCallback(bool aDestroying) {
  mCallback = nullptr;
  StopMLSFallback();
  DoShutdown(aDestroying, aDestroying);
}

NS_IMPL_ISUPPORTS(GCLocProviderPriv, nsIGeolocationProvider)

// nsIGeolocationProvider
//

/*
 * The Startup() method should only succeed if Geoclue is available on D-Bus
 * so it can be used for determining whether to continue with this geolocation
 * provider in Geolocation.cpp
 */
NS_IMETHODIMP
GCLocProviderPriv::Startup() {
  if (mProxyManager) {
    return NS_OK;
  }

  MOZ_DIAGNOSTIC_ASSERT(mClientState == ClientState::Uninit,
                        "Client in a initialized state but no manager");

  GUniquePtr<GError> error;
  mProxyManager = dont_AddRef(g_dbus_proxy_new_for_bus_sync(
      G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr, kGeoclueBusName,
      kGCManagerPath, kGCManagerInterface, nullptr, getter_Transfers(error)));
  if (!mProxyManager) {
    GCL_LOG(Info, "Cannot connect to the Manager interface: %s\n",
            error->message);
    return NS_ERROR_FAILURE;
  }

  g_signal_connect(mProxyManager, "notify::g-name-owner",
                   G_CALLBACK(GCManagerOwnerNotify), this);

  GUniquePtr<gchar> managerOwner(g_dbus_proxy_get_name_owner(mProxyManager));
  if (!managerOwner) {
    GCL_LOG(Info, "The Manager interface has no owner\n");
    DeleteManager();
    return NS_ERROR_FAILURE;
  }

  GCL_LOG(Info, "Manager interface connected successfully\n");

  return NS_OK;
}

void GCLocProviderPriv::WatchStart() {
  if (mClientState == ClientState::Idle) {
    StartClient();
  } else if (mClientState == ClientState::Started) {
    if (mLastPosition && !mLastPositionTimer) {
      GCL_LOG(Verbose,
              "Will report the existing position if new one doesn't come up\n");
      StartLastPositionTimer();
    }
  } else if (mClientState == ClientState::SettingAccuracy) {
    GCLP_SETSTATE(this, SettingAccuracyForStart);
  } else if (mClientState == ClientState::Stopping) {
    GCLP_SETSTATE(this, StoppingForRestart);
  }
}

NS_IMETHODIMP
GCLocProviderPriv::Watch(nsIGeolocationUpdate* aCallback) {
  mCallback = aCallback;

  if (!mCancellable) {
    mCancellable = dont_AddRef(g_cancellable_new());
  }

  if (mClientState != ClientState::Uninit) {
    WatchStart();
    return NS_OK;
  }

  if (!mProxyManager) {
    GCL_LOG(Debug, "watch request falling back to MLS");
    return FallbackToMLS();
  }

  StopMLSFallback();

  GCLP_SETSTATE(this, Initing);
  g_dbus_proxy_call(mProxyManager, "GetClient", nullptr, G_DBUS_CALL_FLAGS_NONE,
                    -1, mCancellable,
                    reinterpret_cast<GAsyncReadyCallback>(GetClientResponse),
                    this);

  return NS_OK;
}

NS_IMETHODIMP
GCLocProviderPriv::Shutdown() {
  DoShutdownClearCallback(false);
  return NS_OK;
}

NS_IMETHODIMP
GCLocProviderPriv::SetHighAccuracy(bool aHigh) {
  GCL_LOG(Verbose, "Want %s accuracy\n", aHigh ? "high" : "low");
  if (!aHigh && AlwaysHighAccuracy()) {
    GCL_LOG(Verbose, "Forcing high accuracy due to pref\n");
    aHigh = true;
  }

  mAccuracyWanted = aHigh ? Accuracy::High : Accuracy::Low;
  MaybeRestartForAccuracy();

  return NS_OK;
}

GeoclueLocationProvider::GeoclueLocationProvider() {
  mPriv = new GCLocProviderPriv;
}

// nsISupports
//

NS_IMPL_ISUPPORTS(GeoclueLocationProvider, nsIGeolocationProvider)

// nsIGeolocationProvider
//

NS_IMETHODIMP
GeoclueLocationProvider::Startup() { return mPriv->Startup(); }

NS_IMETHODIMP
GeoclueLocationProvider::Watch(nsIGeolocationUpdate* aCallback) {
  return mPriv->Watch(aCallback);
}

NS_IMETHODIMP
GeoclueLocationProvider::Shutdown() { return mPriv->Shutdown(); }

NS_IMETHODIMP
GeoclueLocationProvider::SetHighAccuracy(bool aHigh) {
  return mPriv->SetHighAccuracy(aHigh);
}

}  // namespace mozilla::dom
