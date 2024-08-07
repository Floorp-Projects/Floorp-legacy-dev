/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_CanonicalBrowsingContext_h
#define mozilla_dom_CanonicalBrowsingContext_h

#include "mozilla/dom/BrowsingContext.h"
#include "mozilla/dom/MediaControlKeySource.h"
#include "mozilla/dom/BrowsingContextWebProgress.h"
#include "mozilla/dom/FeaturePolicy.h"
#include "mozilla/dom/ProcessIsolation.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/SessionHistoryEntry.h"
#include "mozilla/dom/SessionStoreRestoreData.h"
#include "mozilla/dom/SessionStoreUtils.h"
#include "mozilla/dom/ipc/IdType.h"
#include "mozilla/RefPtr.h"
#include "mozilla/MozPromise.h"
#include "nsCycleCollectionParticipant.h"
#include "nsWrapperCache.h"
#include "nsTArray.h"
#include "nsTHashtable.h"
#include "nsHashKeys.h"
#include "nsISecureBrowserUI.h"

class nsIBrowserDOMWindow;
class nsISHistory;
class nsIWidget;
class nsIPrintSettings;
class nsSHistory;
class nsBrowserStatusFilter;
class nsSecureBrowserUI;
class CallerWillNotifyHistoryIndexAndLengthChanges;
class nsITimer;

namespace mozilla {
enum class CallState;

namespace embedding {
class PrintData;
}

namespace net {
class DocumentLoadListener;
}

namespace dom {

class BrowserParent;
class BrowserBridgeParent;
class FeaturePolicy;
struct LoadURIOptions;
class MediaController;
struct LoadingSessionHistoryInfo;
class SSCacheCopy;
class WindowGlobalParent;
class SessionStoreFormData;
class SessionStoreScrollData;

// CanonicalBrowsingContext is a BrowsingContext living in the parent
// process, with whatever extra data that a BrowsingContext in the
// parent needs.
class CanonicalBrowsingContext final : public BrowsingContext {
 public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS_INHERITED(
      CanonicalBrowsingContext, BrowsingContext)

  static already_AddRefed<CanonicalBrowsingContext> Get(uint64_t aId);
  static CanonicalBrowsingContext* Cast(BrowsingContext* aContext);
  static const CanonicalBrowsingContext* Cast(const BrowsingContext* aContext);
  static already_AddRefed<CanonicalBrowsingContext> Cast(
      already_AddRefed<BrowsingContext>&& aContext);

  bool IsOwnedByProcess(uint64_t aProcessId) const {
    return mProcessId == aProcessId;
  }
  bool IsEmbeddedInProcess(uint64_t aProcessId) const {
    return mEmbedderProcessId == aProcessId;
  }
  uint64_t OwnerProcessId() const { return mProcessId; }
  uint64_t EmbedderProcessId() const { return mEmbedderProcessId; }
  ContentParent* GetContentParent() const;

  void GetCurrentRemoteType(nsACString& aRemoteType, ErrorResult& aRv) const;

  void SetOwnerProcessId(uint64_t aProcessId);

  // The ID of the BrowsingContext which caused this BrowsingContext to be
  // opened, or `0` if this is unknown.
  // Only set for toplevel content BrowsingContexts, and may be from a different
  // BrowsingContextGroup.
  uint64_t GetCrossGroupOpenerId() const { return mCrossGroupOpenerId; }
  void SetCrossGroupOpenerId(uint64_t aOpenerId);
  void SetCrossGroupOpener(CanonicalBrowsingContext& aCrossGroupOpener,
                           ErrorResult& aRv);

  void GetWindowGlobals(nsTArray<RefPtr<WindowGlobalParent>>& aWindows);

  // The current active WindowGlobal.
  WindowGlobalParent* GetCurrentWindowGlobal() const;

  // Same as the methods on `BrowsingContext`, but with the types already cast
  // to the parent process type.
  CanonicalBrowsingContext* GetParent() {
    return Cast(BrowsingContext::GetParent());
  }
  CanonicalBrowsingContext* Top() { return Cast(BrowsingContext::Top()); }
  WindowGlobalParent* GetParentWindowContext();
  WindowGlobalParent* GetTopWindowContext();

  already_AddRefed<nsIWidget> GetParentProcessWidgetContaining();
  already_AddRefed<nsIBrowserDOMWindow> GetBrowserDOMWindow();

  // Same as `GetParentWindowContext`, but will also cross <browser> and
  // content/chrome boundaries.
  already_AddRefed<WindowGlobalParent> GetEmbedderWindowGlobal() const;

  already_AddRefed<CanonicalBrowsingContext> GetParentCrossChromeBoundary();

  already_AddRefed<CanonicalBrowsingContext> TopCrossChromeBoundary();
  Nullable<WindowProxyHolder> GetTopChromeWindow();

  nsISHistory* GetSessionHistory();
  SessionHistoryEntry* GetActiveSessionHistoryEntry();
  void SetActiveSessionHistoryEntry(SessionHistoryEntry* aEntry);

  UniquePtr<LoadingSessionHistoryInfo> CreateLoadingSessionHistoryEntryForLoad(
      nsDocShellLoadState* aLoadState, SessionHistoryEntry* aExistingEntry,
      nsIChannel* aChannel);

  UniquePtr<LoadingSessionHistoryInfo> ReplaceLoadingSessionHistoryEntryForLoad(
      LoadingSessionHistoryInfo* aInfo, nsIChannel* aNewChannel);

  using PrintPromise = MozPromise</* unused */ bool, nsresult, false>;
  MOZ_CAN_RUN_SCRIPT RefPtr<PrintPromise> Print(nsIPrintSettings*);
  MOZ_CAN_RUN_SCRIPT already_AddRefed<Promise> PrintJS(nsIPrintSettings*,
                                                       ErrorResult&);

  // Call the given callback on all top-level descendant BrowsingContexts.
  // Return Callstate::Stop from the callback to stop calling
  // further children.
  void CallOnAllTopDescendants(
      const std::function<mozilla::CallState(CanonicalBrowsingContext*)>&
          aCallback);

  void SessionHistoryCommit(uint64_t aLoadId, const nsID& aChangeID,
                            uint32_t aLoadType, bool aPersist,
                            bool aCloneEntryChildren, bool aChannelExpired,
                            uint32_t aCacheKey);

  // Calls the session history listeners' OnHistoryReload, storing the result in
  // aCanReload. If aCanReload is set to true and we have an active or a loading
  // entry then aLoadState will be initialized from that entry, and
  // aReloadActiveEntry will be true if we have an active entry. If aCanReload
  // is true and aLoadState and aReloadActiveEntry are not set then we should
  // attempt to reload based on the current document in the docshell.
  void NotifyOnHistoryReload(
      bool aForceReload, bool& aCanReload,
      Maybe<NotNull<RefPtr<nsDocShellLoadState>>>& aLoadState,
      Maybe<bool>& aReloadActiveEntry);

  // See BrowsingContext::SetActiveSessionHistoryEntry.
  void SetActiveSessionHistoryEntry(const Maybe<nsPoint>& aPreviousScrollPos,
                                    SessionHistoryInfo* aInfo,
                                    uint32_t aLoadType,
                                    uint32_t aUpdatedCacheKey,
                                    const nsID& aChangeID);

  void ReplaceActiveSessionHistoryEntry(SessionHistoryInfo* aInfo);

  void RemoveDynEntriesFromActiveSessionHistoryEntry();

  void RemoveFromSessionHistory(const nsID& aChangeID);

  Maybe<int32_t> HistoryGo(int32_t aOffset, uint64_t aHistoryEpoch,
                           bool aRequireUserInteraction, bool aUserActivation,
                           Maybe<ContentParentId> aContentId);

  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;

  // Dispatches a wheel zoom change to the embedder element.
  void DispatchWheelZoomChange(bool aIncrease);

  // This function is used to start the autoplay media which are delayed to
  // start. If needed, it would also notify the content browsing context which
  // are related with the canonical browsing content tree to start delayed
  // autoplay media.
  void NotifyStartDelayedAutoplayMedia();

  // This function is used to mute or unmute all media within a tab. It would
  // set the media mute property for the top level window and propagate it to
  // other top level windows in other processes.
  void NotifyMediaMutedChanged(bool aMuted, ErrorResult& aRv);

  // Return the number of unique site origins by iterating all given BCs,
  // including their subtrees.
  static uint32_t CountSiteOrigins(
      GlobalObject& aGlobal,
      const Sequence<mozilla::OwningNonNull<BrowsingContext>>& aRoots);

  // Return true if a private browsing session is active.
  static bool IsPrivateBrowsingActive();

  // This function would propogate the action to its all child browsing contexts
  // in content processes.
  void UpdateMediaControlAction(const MediaControlAction& aAction);

  // Triggers a load in the process
  using BrowsingContext::LoadURI;
  void FixupAndLoadURIString(const nsAString& aURI,
                             const LoadURIOptions& aOptions,
                             ErrorResult& aError);
  void LoadURI(nsIURI* aURI, const LoadURIOptions& aOptions,
               ErrorResult& aError);

  void GoBack(const Optional<int32_t>& aCancelContentJSEpoch,
              bool aRequireUserInteraction, bool aUserActivation);
  void GoForward(const Optional<int32_t>& aCancelContentJSEpoch,
                 bool aRequireUserInteraction, bool aUserActivation);
  void GoToIndex(int32_t aIndex, const Optional<int32_t>& aCancelContentJSEpoch,
                 bool aUserActivation);
  void Reload(uint32_t aReloadFlags);
  void Stop(uint32_t aStopFlags);

  // Get the publicly exposed current URI loaded in this BrowsingContext.
  already_AddRefed<nsIURI> GetCurrentURI() const;
  void SetCurrentRemoteURI(nsIURI* aCurrentRemoteURI);

  BrowserParent* GetBrowserParent() const;
  void SetCurrentBrowserParent(BrowserParent* aBrowserParent);

  // Internal method to change which process a BrowsingContext is being loaded
  // in. The returned promise will resolve when the process switch is completed.
  //
  // A NOT_REMOTE_TYPE aRemoteType argument will perform a process switch into
  // the parent process, and the method will resolve with a null BrowserParent.
  using RemotenessPromise = MozPromise<RefPtr<BrowserParent>, nsresult, false>;
  RefPtr<RemotenessPromise> ChangeRemoteness(
      const NavigationIsolationOptions& aOptions, uint64_t aPendingSwitchId);

  // Return a media controller from the top-level browsing context that can
  // control all media belonging to this browsing context tree. Return nullptr
  // if the top-level browsing context has been discarded.
  MediaController* GetMediaController();
  bool HasCreatedMediaController() const;

  // Attempts to start loading the given load state in this BrowsingContext,
  // without requiring any communication from a docshell. This will handle
  // computing the right process to load in, and organising handoff to
  // the right docshell when we get a response.
  bool LoadInParent(nsDocShellLoadState* aLoadState, bool aSetNavigating);

  // Attempts to start loading the given load state in this BrowsingContext,
  // in parallel with a DocumentChannelChild being created in the docshell.
  // Requires the DocumentChannel to connect with this load for it to
  // complete successfully.
  bool AttemptSpeculativeLoadInParent(nsDocShellLoadState* aLoadState);

  // Get or create a secure browser UI for this BrowsingContext
  nsISecureBrowserUI* GetSecureBrowserUI();

  BrowsingContextWebProgress* GetWebProgress() { return mWebProgress; }

  // Called when the current URI changes (from an
  // nsIWebProgressListener::OnLocationChange event, so that we
  // can update our security UI for the new location, or when the
  // mixed content/https-only state for our current window is changed.
  void UpdateSecurityState();

  void MaybeAddAsProgressListener(nsIWebProgress* aWebProgress);

  // Called when a navigation forces us to recreate our browsing
  // context (for example, when switching in or out of the parent
  // process).
  // aNewContext is the newly created BrowsingContext that is replacing
  // us.
  void ReplacedBy(CanonicalBrowsingContext* aNewContext,
                  const NavigationIsolationOptions& aRemotenessOptions);

  bool HasHistoryEntry(nsISHEntry* aEntry);
  bool HasLoadingHistoryEntry(nsISHEntry* aEntry) {
    for (const LoadingSessionHistoryEntry& loading : mLoadingEntries) {
      if (loading.mEntry == aEntry) {
        return true;
      }
    }
    return false;
  }

  void SwapHistoryEntries(nsISHEntry* aOldEntry, nsISHEntry* aNewEntry);

  void AddLoadingSessionHistoryEntry(uint64_t aLoadId,
                                     SessionHistoryEntry* aEntry);

  void GetLoadingSessionHistoryInfoFromParent(
      Maybe<LoadingSessionHistoryInfo>& aLoadingInfo);

  void HistoryCommitIndexAndLength();

  void SynchronizeLayoutHistoryState();

  void ResetScalingZoom();

  void SetContainerFeaturePolicy(
      Maybe<FeaturePolicyInfo>&& aContainerFeaturePolicyInfo);
  const Maybe<FeaturePolicyInfo>& GetContainerFeaturePolicy() const {
    return mContainerFeaturePolicyInfo;
  }

  void SetRestoreData(SessionStoreRestoreData* aData, ErrorResult& aError);
  void ClearRestoreState();
  MOZ_CAN_RUN_SCRIPT_BOUNDARY void RequestRestoreTabContent(
      WindowGlobalParent* aWindow);
  already_AddRefed<Promise> GetRestorePromise();

  nsresult WriteSessionStorageToSessionStore(
      const nsTArray<SSCacheCopy>& aSesssionStorage, uint32_t aEpoch);

  void UpdateSessionStoreSessionStorage(const std::function<void()>& aDone);

  static void UpdateSessionStoreForStorage(uint64_t aBrowsingContextId);

  // Called when a BrowserParent for this BrowsingContext has been fully
  // destroyed (i.e. `ActorDestroy` was called).
  void BrowserParentDestroyed(BrowserParent* aBrowserParent,
                              bool aAbnormalShutdown);

  void StartUnloadingHost(uint64_t aChildID);
  void ClearUnloadingHost(uint64_t aChildID);

  bool AllowedInBFCache(const Maybe<uint64_t>& aChannelId, nsIURI* aNewURI);

  // Methods for getting and setting the active state for top level
  // browsing contexts, for the process priority manager.
  bool IsPriorityActive() const {
    MOZ_RELEASE_ASSERT(IsTop());
    return mPriorityActive;
  }
  void SetPriorityActive(bool aIsActive) {
    MOZ_RELEASE_ASSERT(IsTop());
    mPriorityActive = aIsActive;
  }

  void SetTouchEventsOverride(dom::TouchEventsOverride, ErrorResult& aRv);
  void SetTargetTopLevelLinkClicksToBlank(bool aTargetTopLevelLinkClicksToBlank,
                                          ErrorResult& aRv);

  bool IsReplaced() const { return mIsReplaced; }

  const JS::Heap<JS::Value>& PermanentKey() { return mPermanentKey; }
  void ClearPermanentKey() { mPermanentKey.setNull(); }
  void MaybeSetPermanentKey(Element* aEmbedder);

  // When request for page awake, it would increase a count that is used to
  // prevent whole browsing context tree from being suspended. The request can
  // be called multiple times. When calling the revoke, it would decrease the
  // count and once the count reaches to zero, the browsing context tree could
  // be suspended when the tree is inactive.
  void AddPageAwakeRequest();
  void RemovePageAwakeRequest();

  void CloneDocumentTreeInto(CanonicalBrowsingContext* aSource,
                             const nsACString& aRemoteType,
                             embedding::PrintData&& aPrintData);

  // Returns a Promise which resolves when cloning documents for printing
  // finished if this browsing context is cloning document tree.
  RefPtr<GenericNonExclusivePromise> GetClonePromise() const {
    return mClonePromise;
  }

  bool StartApzAutoscroll(float aAnchorX, float aAnchorY, nsViewID aScrollId,
                          uint32_t aPresShellId);
  void StopApzAutoscroll(nsViewID aScrollId, uint32_t aPresShellId);

  void AddFinalDiscardListener(std::function<void(uint64_t)>&& aListener);

  bool ForceAppWindowActive() const { return mForceAppWindowActive; }
  void SetForceAppWindowActive(bool, ErrorResult&);
  void RecomputeAppWindowVisibility();

  already_AddRefed<nsISHEntry> GetMostRecentLoadingSessionHistoryEntry();

 protected:
  // Called when the browsing context is being discarded.
  void CanonicalDiscard();

  // Called when the browsing context is being attached.
  void CanonicalAttach();

  // Called when the browsing context private mode is changed after
  // being attached, but before being discarded.
  void AdjustPrivateBrowsingCount(bool aPrivateBrowsing);

  using Type = BrowsingContext::Type;
  CanonicalBrowsingContext(WindowContext* aParentWindow,
                           BrowsingContextGroup* aGroup,
                           uint64_t aBrowsingContextId,
                           uint64_t aOwnerProcessId,
                           uint64_t aEmbedderProcessId, Type aType,
                           FieldValues&& aInit);

 private:
  friend class BrowsingContext;

  virtual ~CanonicalBrowsingContext();

  class PendingRemotenessChange {
   public:
    NS_INLINE_DECL_REFCOUNTING(PendingRemotenessChange)

    PendingRemotenessChange(CanonicalBrowsingContext* aTarget,
                            RemotenessPromise::Private* aPromise,
                            uint64_t aPendingSwitchId,
                            const NavigationIsolationOptions& aOptions);

    void Cancel(nsresult aRv);

   private:
    friend class CanonicalBrowsingContext;

    ~PendingRemotenessChange();
    void ProcessLaunched();
    void ProcessReady();
    void MaybeFinish();
    void Clear();

    nsresult FinishTopContent();
    nsresult FinishSubframe();

    RefPtr<CanonicalBrowsingContext> mTarget;
    RefPtr<RemotenessPromise::Private> mPromise;
    RefPtr<ContentParent> mContentParent;
    RefPtr<BrowsingContextGroup> mSpecificGroup;

    bool mProcessReady = false;
    bool mWaitingForPrepareToChange = false;

    uint64_t mPendingSwitchId;
    NavigationIsolationOptions mOptions;
  };

  struct RestoreState {
    NS_INLINE_DECL_REFCOUNTING(RestoreState)

    void ClearData() { mData = nullptr; }
    void Resolve();

    RefPtr<SessionStoreRestoreData> mData;
    RefPtr<Promise> mPromise;
    uint32_t mRequests = 0;
    uint32_t mResolves = 0;

   private:
    ~RestoreState() = default;
  };

  friend class net::DocumentLoadListener;
  // Called when a DocumentLoadListener is created to start a load for
  // this browsing context. Returns false if a higher priority load is
  // already in-progress and the new one has been rejected.
  bool StartDocumentLoad(net::DocumentLoadListener* aLoad);
  // Called once DocumentLoadListener completes handling a load, and it
  // is either complete, or handed off to the final channel to deliver
  // data to the destination docshell.
  // If aContinueNavigating it set, the reference to the DocumentLoadListener
  // will be cleared to prevent it being cancelled, however the current load ID
  // will be preserved until `EndDocumentLoad` is called again.
  void EndDocumentLoad(bool aContinueNavigating);

  bool SupportsLoadingInParent(nsDocShellLoadState* aLoadState,
                               uint64_t* aOuterWindowId);

  void HistoryCommitIndexAndLength(
      const nsID& aChangeID,
      const CallerWillNotifyHistoryIndexAndLengthChanges& aProofOfCaller);

  struct UnloadingHost {
    uint64_t mChildID;
    nsTArray<std::function<void()>> mCallbacks;
  };
  nsTArray<UnloadingHost>::iterator FindUnloadingHost(uint64_t aChildID);

  // Called when we want to show the subframe crashed UI as our previous browser
  // has become unloaded for one reason or another.
  void ShowSubframeCrashedUI(BrowserBridgeParent* aBridge);

  void MaybeScheduleSessionStoreUpdate();

  void CancelSessionStoreUpdate();

  void AddPendingDiscard();

  void RemovePendingDiscard();

  bool ShouldAddEntryForRefresh(const SessionHistoryEntry* aEntry) {
    return ShouldAddEntryForRefresh(aEntry->Info().GetURI(),
                                    aEntry->Info().HasPostData());
  }
  bool ShouldAddEntryForRefresh(nsIURI* aNewURI, bool aHasPostData) {
    nsCOMPtr<nsIURI> currentURI = GetCurrentURI();
    return BrowsingContext::ShouldAddEntryForRefresh(currentURI, aNewURI,
                                                     aHasPostData);
  }

  already_AddRefed<nsDocShellLoadState> CreateLoadInfo(
      SessionHistoryEntry* aEntry);

  // XXX(farre): Store a ContentParent pointer here rather than mProcessId?
  // Indicates which process owns the docshell.
  uint64_t mProcessId;

  // Indicates which process owns the embedder element.
  uint64_t mEmbedderProcessId;

  uint64_t mCrossGroupOpenerId = 0;

  // This function will make the top window context reset its
  // "SHEntryHasUserInteraction" cache that prevents documents from repeatedly
  // setting user interaction on SH entries. Should be called anytime SH
  // entries are added or replaced.
  void ResetSHEntryHasUserInteractionCache();

  RefPtr<BrowserParent> mCurrentBrowserParent;

  nsTArray<UnloadingHost> mUnloadingHosts;

  // The current URI loaded in this BrowsingContext. This value is only set for
  // BrowsingContexts loaded in content processes.
  nsCOMPtr<nsIURI> mCurrentRemoteURI;

  // The current remoteness change which is in a pending state.
  RefPtr<PendingRemotenessChange> mPendingRemotenessChange;

  RefPtr<nsSHistory> mSessionHistory;

  // Tab media controller is used to control all media existing in the same
  // browsing context tree, so it would only exist in the top level browsing
  // context.
  RefPtr<MediaController> mTabMediaController;

  RefPtr<net::DocumentLoadListener> mCurrentLoad;

  struct LoadingSessionHistoryEntry {
    uint64_t mLoadId = 0;
    RefPtr<SessionHistoryEntry> mEntry;
  };
  nsTArray<LoadingSessionHistoryEntry> mLoadingEntries;
  RefPtr<SessionHistoryEntry> mActiveEntry;

  RefPtr<nsSecureBrowserUI> mSecureBrowserUI;
  RefPtr<BrowsingContextWebProgress> mWebProgress;

  nsCOMPtr<nsIWebProgressListener> mDocShellProgressBridge;
  RefPtr<nsBrowserStatusFilter> mStatusFilter;

  Maybe<FeaturePolicyInfo> mContainerFeaturePolicyInfo;

  friend class BrowserSessionStore;
  WeakPtr<SessionStoreFormData>& GetSessionStoreFormDataRef() {
    return mFormdata;
  }
  WeakPtr<SessionStoreScrollData>& GetSessionStoreScrollDataRef() {
    return mScroll;
  }

  WeakPtr<SessionStoreFormData> mFormdata;
  WeakPtr<SessionStoreScrollData> mScroll;

  RefPtr<RestoreState> mRestoreState;

  nsCOMPtr<nsITimer> mSessionStoreSessionStorageUpdateTimer;

  // If this is a top level context, this is true if our browser ID is marked as
  // active in the process priority manager.
  bool mPriorityActive = false;

  // See CanonicalBrowsingContext.forceAppWindowActive.
  bool mForceAppWindowActive = false;

  bool mIsReplaced = false;

  // A Promise created when cloning documents for printing.
  RefPtr<GenericNonExclusivePromise> mClonePromise;

  JS::Heap<JS::Value> mPermanentKey;

  uint32_t mPendingDiscards = 0;

  bool mFullyDiscarded = false;

  nsTArray<std::function<void(uint64_t)>> mFullyDiscardedListeners;
};

}  // namespace dom
}  // namespace mozilla

#endif  // !defined(mozilla_dom_CanonicalBrowsingContext_h)
