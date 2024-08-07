/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil; tab-width: 8 -*- */
/* vim: set sw=2 ts=8 et tw=80 ft=cpp : */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_WindowGlobalChild_h
#define mozilla_dom_WindowGlobalChild_h

#include "mozilla/RefPtr.h"
#include "mozilla/WeakPtr.h"
#include "mozilla/dom/PWindowGlobalChild.h"
#include "nsRefPtrHashtable.h"
#include "nsWrapperCache.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/WindowGlobalActor.h"

class nsGlobalWindowInner;
class nsDocShell;

namespace mozilla::dom {

class BrowsingContext;
class WindowContext;
class WindowGlobalParent;
class JSWindowActorChild;
class JSActorMessageMeta;
class BrowserChild;

/**
 * Actor for a single nsGlobalWindowInner. This actor is used to communicate
 * information to the parent process asynchronously.
 */
class WindowGlobalChild final : public WindowGlobalActor,
                                public nsWrapperCache,
                                public PWindowGlobalChild,
                                public SupportsWeakPtr {
  friend class PWindowGlobalChild;

 public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_WRAPPERCACHE_CLASS(WindowGlobalChild)

  static already_AddRefed<WindowGlobalChild> GetByInnerWindowId(
      uint64_t aInnerWindowId);

  static already_AddRefed<WindowGlobalChild> GetByInnerWindowId(
      const GlobalObject& aGlobal, uint64_t aInnerWindowId) {
    return GetByInnerWindowId(aInnerWindowId);
  }

  dom::BrowsingContext* BrowsingContext() override;
  dom::WindowContext* WindowContext() const { return mWindowContext; }
  nsGlobalWindowInner* GetWindowGlobal() const { return mWindowGlobal; }

  // Has this actor been shut down
  bool IsClosed() { return !CanSend(); }
  void Destroy();

  // Check if this actor is managed by PInProcess, as-in the document is loaded
  // in the chrome process.
  bool IsInProcess() { return XRE_IsParentProcess(); }

  nsIURI* GetDocumentURI() override { return mDocumentURI; }
  void SetDocumentURI(nsIURI* aDocumentURI);
  // See the corresponding comment for `UpdateDocumentPrincipal` in
  // PWindowGlobal on why and when this is allowed
  void SetDocumentPrincipal(nsIPrincipal* aNewDocumentPrincipal,
                            nsIPrincipal* aNewDocumentStoragePrincipal);

  nsIPrincipal* DocumentPrincipal() { return mDocumentPrincipal; }

  // The Window ID for this WindowGlobal
  uint64_t InnerWindowId();
  uint64_t OuterWindowId();

  uint64_t ContentParentId();

  int64_t BeforeUnloadListeners() { return mBeforeUnloadListeners; }
  void BeforeUnloadAdded();
  void BeforeUnloadRemoved();

  bool IsCurrentGlobal();

  bool IsProcessRoot();

  // Get the other side of this actor if it is an in-process actor. Returns
  // |nullptr| if the actor has been torn down, or is not in-process.
  already_AddRefed<WindowGlobalParent> GetParentActor();

  // Get this actor's manager if it is not an in-process actor. Returns
  // |nullptr| if the actor has been torn down, or is in-process.
  already_AddRefed<BrowserChild> GetBrowserChild();

  // Get a JS actor object by name.
  already_AddRefed<JSWindowActorChild> GetActor(JSContext* aCx,
                                                const nsACString& aName,
                                                ErrorResult& aRv);
  already_AddRefed<JSWindowActorChild> GetExistingActor(
      const nsACString& aName);

  // Create and initialize the WindowGlobalChild object.
  static already_AddRefed<WindowGlobalChild> Create(
      nsGlobalWindowInner* aWindow);
  static already_AddRefed<WindowGlobalChild> CreateDisconnected(
      const WindowGlobalInit& aInit);

  void Init();

  void InitWindowGlobal(nsGlobalWindowInner* aWindow);

  // Called when a new document is loaded in this WindowGlobalChild.
  void OnNewDocument(Document* aNewDocument);

  // Returns true if this WindowGlobal is same-origin with the given
  // WindowContext. Out-of-process WindowContexts are supported, and are assumed
  // to be cross-origin.
  //
  // The given WindowContext must be in the same BrowsingContextGroup as this
  // window global.
  bool IsSameOriginWith(const dom::WindowContext* aOther) const;

  bool SameOriginWithTop();

  // Returns `true` if this WindowGlobal is allowed to navigate the given
  // BrowsingContext. BrowsingContexts which are currently out-of-process are
  // supported, and assumed to be cross-origin.
  //
  // The given BrowsingContext must be in the same BrowsingContextGroup as this
  // WindowGlobal.
  bool CanNavigate(dom::BrowsingContext* aTarget, bool aConsiderOpener = true);

  // Using the rules for choosing a browsing context we try to find
  // the browsing context with the given name in the set of
  // transitively reachable browsing contexts. Performs access control
  // checks with regard to this.
  // See
  // https://html.spec.whatwg.org/multipage/browsers.html#the-rules-for-choosing-a-browsing-context-given-a-browsing-context-name.
  dom::BrowsingContext* FindBrowsingContextWithName(
      const nsAString& aName, bool aUseEntryGlobalForAccessCheck = true);

  nsISupports* GetParentObject();
  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;

  void UnblockBFCacheFor(BFCacheStatus aStatus);
  void BlockBFCacheFor(BFCacheStatus aStatus);

 protected:
  const nsACString& GetRemoteType() override;

  already_AddRefed<JSActor> InitJSActor(JS::Handle<JSObject*> aMaybeActor,
                                        const nsACString& aName,
                                        ErrorResult& aRv) override;
  mozilla::ipc::IProtocol* AsNativeActor() override { return this; }

  // IPC messages
  mozilla::ipc::IPCResult RecvRawMessage(
      const JSActorMessageMeta& aMeta, const Maybe<ClonedMessageData>& aData,
      const Maybe<ClonedMessageData>& aStack);

  mozilla::ipc::IPCResult RecvMakeFrameLocal(
      const MaybeDiscarded<dom::BrowsingContext>& aFrameContext,
      uint64_t aPendingSwitchId);

  mozilla::ipc::IPCResult RecvMakeFrameRemote(
      const MaybeDiscarded<dom::BrowsingContext>& aFrameContext,
      ManagedEndpoint<PBrowserBridgeChild>&& aEndpoint, const TabId& aTabId,
      const LayersId& aLayersId, MakeFrameRemoteResolver&& aResolve);

  mozilla::ipc::IPCResult RecvDrawSnapshot(const Maybe<IntRect>& aRect,
                                           const float& aScale,
                                           const nscolor& aBackgroundColor,
                                           const uint32_t& aFlags,
                                           DrawSnapshotResolver&& aResolve);

  mozilla::ipc::IPCResult RecvDispatchSecurityPolicyViolation(
      const nsString& aViolationEventJSON);

  mozilla::ipc::IPCResult RecvSaveStorageAccessPermissionGranted();

  mozilla::ipc::IPCResult RecvAddBlockedFrameNodeByClassifier(
      const MaybeDiscardedBrowsingContext& aNode);

  mozilla::ipc::IPCResult RecvResetScalingZoom();

  mozilla::ipc::IPCResult RecvRestoreDocShellState(
      const dom::sessionstore::DocShellRestoreState& aState,
      RestoreDocShellStateResolver&& aResolve);

  // TODO: Use MOZ_CAN_RUN_SCRIPT when it gains IPDL support (bug 1539864)
  MOZ_CAN_RUN_SCRIPT_BOUNDARY mozilla::ipc::IPCResult RecvRestoreTabContent(
      dom::SessionStoreRestoreData* aData,
      RestoreTabContentResolver&& aResolve);

  virtual void ActorDestroy(ActorDestroyReason aWhy) override;

 private:
  WindowGlobalChild(dom::WindowContext* aWindowContext,
                    nsIPrincipal* aPrincipal, nsIURI* aURI);

  ~WindowGlobalChild();

  RefPtr<nsGlobalWindowInner> mWindowGlobal;
  RefPtr<dom::WindowContext> mWindowContext;
  nsCOMPtr<nsIPrincipal> mDocumentPrincipal;
  RefPtr<dom::FeaturePolicy> mContainerFeaturePolicy;
  nsCOMPtr<nsIURI> mDocumentURI;
  int64_t mBeforeUnloadListeners = 0;
};

}  // namespace mozilla::dom

#endif  // !defined(mozilla_dom_WindowGlobalChild_h)
