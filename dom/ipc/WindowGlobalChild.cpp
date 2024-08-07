/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil; tab-width: 8 -*- */
/* vim: set sw=2 ts=8 et tw=80 ft=cpp : */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/WindowGlobalChild.h"

#include "mozilla/AntiTrackingUtils.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/dom/WindowGlobalParent.h"
#include "mozilla/dom/BrowsingContext.h"
#include "mozilla/dom/BrowsingContextGroup.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/dom/MozFrameLoaderOwnerBinding.h"
#include "mozilla/dom/BrowserChild.h"
#include "mozilla/dom/BrowserBridgeChild.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/dom/SecurityPolicyViolationEvent.h"
#include "mozilla/dom/SessionStoreRestoreData.h"
#include "mozilla/dom/WindowGlobalActorsBinding.h"
#include "mozilla/dom/WindowContext.h"
#include "mozilla/dom/InProcessChild.h"
#include "mozilla/dom/InProcessParent.h"
#include "mozilla/ipc/Endpoint.h"
#include "mozilla/PresShell.h"
#include "mozilla/ScopeExit.h"
#include "GeckoProfiler.h"
#include "nsContentUtils.h"
#include "nsDocShell.h"
#include "nsFocusManager.h"
#include "nsFrameLoaderOwner.h"
#include "nsGlobalWindowInner.h"
#include "nsNetUtil.h"
#include "nsQueryObject.h"
#include "nsSerializationHelper.h"
#include "nsFrameLoader.h"

#include "mozilla/dom/JSWindowActorBinding.h"
#include "mozilla/dom/JSWindowActorChild.h"
#include "mozilla/dom/JSActorService.h"
#include "nsIHttpChannelInternal.h"
#include "nsIURIMutator.h"
#include "nsURLHelper.h"

using namespace mozilla::ipc;
using namespace mozilla::dom::ipc;

namespace mozilla::dom {

WindowGlobalChild::WindowGlobalChild(dom::WindowContext* aWindowContext,
                                     nsIPrincipal* aPrincipal,
                                     nsIURI* aDocumentURI)
    : mWindowContext(aWindowContext),
      mDocumentPrincipal(aPrincipal),
      mDocumentURI(aDocumentURI) {
  MOZ_DIAGNOSTIC_ASSERT(mWindowContext);
  MOZ_DIAGNOSTIC_ASSERT(mDocumentPrincipal);

  if (!mDocumentURI) {
    NS_NewURI(getter_AddRefs(mDocumentURI), "about:blank");
  }

  // Registers a DOM Window with the profiler. It re-registers the same Inner
  // Window ID with different URIs because when a Browsing context is first
  // loaded, the first url loaded in it will be about:blank. This call keeps the
  // first non-about:blank registration of window and discards the previous one.
  uint64_t embedderInnerWindowID = 0;
  if (BrowsingContext()->GetParent()) {
    embedderInnerWindowID = BrowsingContext()->GetEmbedderInnerWindowId();
  }
  profiler_register_page(
      BrowsingContext()->BrowserId(), InnerWindowId(),
      nsContentUtils::TruncatedURLForDisplay(aDocumentURI, 1024),
      embedderInnerWindowID, BrowsingContext()->UsePrivateBrowsing());
}

already_AddRefed<WindowGlobalChild> WindowGlobalChild::Create(
    nsGlobalWindowInner* aWindow) {
#ifdef MOZ_DIAGNOSTIC_ASSERT_ENABLED
  // Opener policy is set when we start to load a document. Here, we ensure we
  // have set the correct Opener policy so that it will be available in the
  // parent process through window global child.
  nsCOMPtr<nsIChannel> chan = aWindow->GetDocument()->GetChannel();
  nsCOMPtr<nsILoadInfo> loadInfo = chan ? chan->LoadInfo() : nullptr;
  nsCOMPtr<nsIHttpChannelInternal> httpChan = do_QueryInterface(chan);
  nsILoadInfo::CrossOriginOpenerPolicy policy;
  if (httpChan &&
      loadInfo->GetExternalContentPolicyType() ==
          ExtContentPolicy::TYPE_DOCUMENT &&
      NS_SUCCEEDED(httpChan->GetCrossOriginOpenerPolicy(&policy))) {
    MOZ_DIAGNOSTIC_ASSERT(policy ==
                          aWindow->GetBrowsingContext()->GetOpenerPolicy());
  }
#endif

  WindowGlobalInit init = WindowGlobalActor::WindowInitializer(aWindow);
  RefPtr<WindowGlobalChild> wgc = CreateDisconnected(init);

  // Send the link constructor over PBrowser, or link over PInProcess.
  if (XRE_IsParentProcess()) {
    InProcessChild* ipChild = InProcessChild::Singleton();
    InProcessParent* ipParent = InProcessParent::Singleton();
    if (!ipChild || !ipParent) {
      return nullptr;
    }

    ManagedEndpoint<PWindowGlobalParent> endpoint =
        ipChild->OpenPWindowGlobalEndpoint(wgc);
    ipParent->BindPWindowGlobalEndpoint(std::move(endpoint),
                                        wgc->WindowContext()->Canonical());
  } else {
    RefPtr<BrowserChild> browserChild =
        BrowserChild::GetFrom(static_cast<mozIDOMWindow*>(aWindow));
    MOZ_ASSERT(browserChild);

#ifdef MOZ_DIAGNOSTIC_ASSERT_ENABLED
    dom::BrowsingContext* bc = aWindow->GetBrowsingContext();
#endif

    MOZ_DIAGNOSTIC_ASSERT(bc->AncestorsAreCurrent());
    MOZ_DIAGNOSTIC_ASSERT(bc->IsInProcess());

    ManagedEndpoint<PWindowGlobalParent> endpoint =
        browserChild->OpenPWindowGlobalEndpoint(wgc);
    browserChild->SendNewWindowGlobal(std::move(endpoint), init);
  }

  wgc->Init();
  wgc->InitWindowGlobal(aWindow);
  return wgc.forget();
}

already_AddRefed<WindowGlobalChild> WindowGlobalChild::CreateDisconnected(
    const WindowGlobalInit& aInit) {
  RefPtr<dom::BrowsingContext> browsingContext =
      dom::BrowsingContext::Get(aInit.context().mBrowsingContextId);

  RefPtr<dom::WindowContext> windowContext =
      dom::WindowContext::GetById(aInit.context().mInnerWindowId);
  MOZ_RELEASE_ASSERT(!windowContext, "Creating duplicate WindowContext");

  // Create our new WindowContext
  if (XRE_IsParentProcess()) {
    windowContext = WindowGlobalParent::CreateDisconnected(aInit);
  } else {
    dom::WindowContext::FieldValues fields = aInit.context().mFields;
    windowContext = new dom::WindowContext(
        browsingContext, aInit.context().mInnerWindowId,
        aInit.context().mOuterWindowId, std::move(fields));
  }

  RefPtr<WindowGlobalChild> windowChild = new WindowGlobalChild(
      windowContext, aInit.principal(), aInit.documentURI());
  windowContext->mIsInProcess = true;
  windowContext->mWindowGlobalChild = windowChild;
  return windowChild.forget();
}

void WindowGlobalChild::Init() {
  MOZ_ASSERT(mWindowContext->mWindowGlobalChild == this);
  mWindowContext->Init();
}

void WindowGlobalChild::InitWindowGlobal(nsGlobalWindowInner* aWindow) {
  mWindowGlobal = aWindow;
}

void WindowGlobalChild::OnNewDocument(Document* aDocument) {
  MOZ_RELEASE_ASSERT(mWindowGlobal);
  MOZ_RELEASE_ASSERT(aDocument);

  // Send a series of messages to update document-specific state on
  // WindowGlobalParent, when we change documents on an existing WindowGlobal.
  // This data is also all sent when we construct a WindowGlobal, so anything
  // added here should also be added to WindowGlobalActor::WindowInitializer.

  // FIXME: Perhaps these should be combined into a smaller number of messages?
  SendSetIsInitialDocument(aDocument->IsInitialDocument());
  SetDocumentURI(aDocument->GetDocumentURI());
  SetDocumentPrincipal(aDocument->NodePrincipal(),
                       aDocument->EffectiveStoragePrincipal());

  nsCOMPtr<nsITransportSecurityInfo> securityInfo;
  if (nsCOMPtr<nsIChannel> channel = aDocument->GetChannel()) {
    channel->GetSecurityInfo(getter_AddRefs(securityInfo));
  }
  SendUpdateDocumentSecurityInfo(securityInfo);

  SendUpdateDocumentCspSettings(aDocument->GetBlockAllMixedContent(false),
                                aDocument->GetUpgradeInsecureRequests(false));
  SendUpdateSandboxFlags(aDocument->GetSandboxFlags());

  net::CookieJarSettingsArgs csArgs;
  net::CookieJarSettings::Cast(aDocument->CookieJarSettings())
      ->Serialize(csArgs);
  if (!SendUpdateCookieJarSettings(csArgs)) {
    NS_WARNING(
        "Failed to update document's cookie jar settings on the "
        "WindowGlobalParent");
  }

  SendUpdateHttpsOnlyStatus(aDocument->HttpsOnlyStatus());

  // Update window context fields for the newly loaded Document.
  WindowContext::Transaction txn;
  txn.SetCookieBehavior(
      Some(aDocument->CookieJarSettings()->GetCookieBehavior()));
  txn.SetIsOnContentBlockingAllowList(
      aDocument->CookieJarSettings()->GetIsOnContentBlockingAllowList());
  txn.SetIsThirdPartyWindow(aDocument->HasThirdPartyChannel());
  txn.SetIsThirdPartyTrackingResourceWindow(
      nsContentUtils::IsThirdPartyTrackingResourceWindow(mWindowGlobal));
  txn.SetIsSecureContext(mWindowGlobal->IsSecureContext());
  if (auto policy = aDocument->GetEmbedderPolicy()) {
    txn.SetEmbedderPolicy(*policy);
  }

  if (nsCOMPtr<nsIChannel> channel = aDocument->GetChannel()) {
    nsCOMPtr<nsILoadInfo> loadInfo(channel->LoadInfo());
    txn.SetIsOriginalFrameSource(loadInfo->GetOriginalFrameSrcLoad());
  } else {
    txn.SetIsOriginalFrameSource(false);
  }

  // Init Mixed Content Fields
  nsCOMPtr<nsIURI> innerDocURI =
      NS_GetInnermostURI(aDocument->GetDocumentURI());
  if (innerDocURI) {
    txn.SetIsSecure(innerDocURI->SchemeIs("https"));
  }

  MOZ_DIAGNOSTIC_ASSERT(mDocumentPrincipal->GetIsLocalIpAddress() ==
                        mWindowContext->IsLocalIP());

  MOZ_ALWAYS_SUCCEEDS(txn.Commit(mWindowContext));
}

/* static */
already_AddRefed<WindowGlobalChild> WindowGlobalChild::GetByInnerWindowId(
    uint64_t aInnerWindowId) {
  if (RefPtr<dom::WindowContext> context =
          dom::WindowContext::GetById(aInnerWindowId)) {
    return do_AddRef(context->GetWindowGlobalChild());
  }
  return nullptr;
}

dom::BrowsingContext* WindowGlobalChild::BrowsingContext() {
  return mWindowContext->GetBrowsingContext();
}

uint64_t WindowGlobalChild::InnerWindowId() {
  return mWindowContext->InnerWindowId();
}

uint64_t WindowGlobalChild::OuterWindowId() {
  return mWindowContext->OuterWindowId();
}

bool WindowGlobalChild::IsCurrentGlobal() {
  return CanSend() && mWindowGlobal->IsCurrentInnerWindow();
}

already_AddRefed<WindowGlobalParent> WindowGlobalChild::GetParentActor() {
  if (!CanSend()) {
    return nullptr;
  }
  IProtocol* otherSide = InProcessChild::ParentActorFor(this);
  return do_AddRef(static_cast<WindowGlobalParent*>(otherSide));
}

already_AddRefed<BrowserChild> WindowGlobalChild::GetBrowserChild() {
  if (IsInProcess() || !CanSend()) {
    return nullptr;
  }
  return do_AddRef(static_cast<BrowserChild*>(Manager()));
}

uint64_t WindowGlobalChild::ContentParentId() {
  if (XRE_IsParentProcess()) {
    return 0;
  }
  return ContentChild::GetSingleton()->GetID();
}

// A WindowGlobalChild is the root in its process if it has no parent, or its
// embedder is in a different process.
bool WindowGlobalChild::IsProcessRoot() {
  if (!BrowsingContext()->GetParent()) {
    return true;
  }

  return !BrowsingContext()->GetEmbedderElement();
}

void WindowGlobalChild::BeforeUnloadAdded() {
  // Don't bother notifying the parent if we don't have an IPC link open.
  if (mBeforeUnloadListeners == 0 && CanSend()) {
    Unused << mWindowContext->SetHasBeforeUnload(true);
  }

  mBeforeUnloadListeners++;
  MOZ_ASSERT(mBeforeUnloadListeners > 0);
}

void WindowGlobalChild::BeforeUnloadRemoved() {
  mBeforeUnloadListeners--;
  MOZ_ASSERT(mBeforeUnloadListeners >= 0);

  if (mBeforeUnloadListeners == 0) {
    Unused << mWindowContext->SetHasBeforeUnload(false);
  }
}

void WindowGlobalChild::Destroy() {
  JSActorWillDestroy();

  mWindowContext->Discard();

  // Perform async IPC shutdown unless we're not in-process, and our
  // BrowserChild is in the process of being destroyed, which will destroy
  // us as well.
  RefPtr<BrowserChild> browserChild = GetBrowserChild();
  if (!browserChild || !browserChild->IsDestroyed()) {
    SendDestroy();
  }
}

mozilla::ipc::IPCResult WindowGlobalChild::RecvMakeFrameLocal(
    const MaybeDiscarded<dom::BrowsingContext>& aFrameContext,
    uint64_t aPendingSwitchId) {
  MOZ_DIAGNOSTIC_ASSERT(XRE_IsContentProcess());

  MOZ_LOG(BrowsingContext::GetLog(), LogLevel::Debug,
          ("RecvMakeFrameLocal ID=%" PRIx64, aFrameContext.ContextId()));

  if (NS_WARN_IF(aFrameContext.IsNullOrDiscarded())) {
    return IPC_OK();
  }
  dom::BrowsingContext* frameContext = aFrameContext.get();

  RefPtr<Element> embedderElt = frameContext->GetEmbedderElement();
  if (NS_WARN_IF(!embedderElt)) {
    return IPC_OK();
  }

  if (NS_WARN_IF(embedderElt->GetOwnerGlobal() != GetWindowGlobal())) {
    return IPC_OK();
  }

  RefPtr<nsFrameLoaderOwner> flo = do_QueryObject(embedderElt);
  MOZ_DIAGNOSTIC_ASSERT(flo, "Embedder must be a nsFrameLoaderOwner");

  // Trigger a process switch into the current process.
  RemotenessOptions options;
  options.mRemoteType = NOT_REMOTE_TYPE;
  options.mPendingSwitchID.Construct(aPendingSwitchId);
  options.mSwitchingInProgressLoad = true;
  flo->ChangeRemoteness(options, IgnoreErrors());
  return IPC_OK();
}

mozilla::ipc::IPCResult WindowGlobalChild::RecvMakeFrameRemote(
    const MaybeDiscarded<dom::BrowsingContext>& aFrameContext,
    ManagedEndpoint<PBrowserBridgeChild>&& aEndpoint, const TabId& aTabId,
    const LayersId& aLayersId, MakeFrameRemoteResolver&& aResolve) {
  MOZ_DIAGNOSTIC_ASSERT(XRE_IsContentProcess());

  MOZ_LOG(BrowsingContext::GetLog(), LogLevel::Debug,
          ("RecvMakeFrameRemote ID=%" PRIx64, aFrameContext.ContextId()));

  if (!aLayersId.IsValid()) {
    return IPC_FAIL(this, "Received an invalid LayersId");
  }

  // Resolve the promise when this function exits, as we'll have fully unloaded
  // at that point.
  auto scopeExit = MakeScopeExit([&] { aResolve(true); });

  // Get a BrowsingContext if we're not null or discarded. We don't want to
  // early-return before we connect the BrowserBridgeChild, as otherwise we'll
  // never break the channel in the parent.
  RefPtr<dom::BrowsingContext> frameContext;
  if (!aFrameContext.IsDiscarded()) {
    frameContext = aFrameContext.get();
  }

  // Immediately construct the BrowserBridgeChild so we can destroy it cleanly
  // if the process switch fails.
  RefPtr<BrowserBridgeChild> bridge =
      new BrowserBridgeChild(frameContext, aTabId, aLayersId);
  RefPtr<BrowserChild> manager = GetBrowserChild();
  if (NS_WARN_IF(
          !manager->BindPBrowserBridgeEndpoint(std::move(aEndpoint), bridge))) {
    return IPC_OK();
  }

  // Synchronously delete de actor here rather than using SendBeginDestroy(), as
  // we haven't initialized it yet.
  auto deleteBridge =
      MakeScopeExit([&] { BrowserBridgeChild::Send__delete__(bridge); });

  // Immediately tear down the actor if we don't have a valid FrameContext.
  if (NS_WARN_IF(aFrameContext.IsNullOrDiscarded())) {
    return IPC_OK();
  }

  RefPtr<Element> embedderElt = frameContext->GetEmbedderElement();
  if (NS_WARN_IF(!embedderElt)) {
    return IPC_OK();
  }

  if (NS_WARN_IF(embedderElt->GetOwnerGlobal() != GetWindowGlobal())) {
    return IPC_OK();
  }

  RefPtr<nsFrameLoaderOwner> flo = do_QueryObject(embedderElt);
  MOZ_DIAGNOSTIC_ASSERT(flo, "Embedder must be a nsFrameLoaderOwner");

  // Trgger a process switch into the specified process.
  IgnoredErrorResult rv;
  flo->ChangeRemotenessWithBridge(bridge, rv);
  if (NS_WARN_IF(rv.Failed())) {
    return IPC_OK();
  }

  // Everything succeeded, so don't delete the bridge.
  deleteBridge.release();

  return IPC_OK();
}

mozilla::ipc::IPCResult WindowGlobalChild::RecvDrawSnapshot(
    const Maybe<IntRect>& aRect, const float& aScale,
    const nscolor& aBackgroundColor, const uint32_t& aFlags,
    DrawSnapshotResolver&& aResolve) {
  aResolve(gfx::PaintFragment::Record(BrowsingContext(), aRect, aScale,
                                      aBackgroundColor,
                                      (gfx::CrossProcessPaintFlags)aFlags));
  return IPC_OK();
}

mozilla::ipc::IPCResult
WindowGlobalChild::RecvSaveStorageAccessPermissionGranted() {
  nsCOMPtr<nsPIDOMWindowInner> inner = GetWindowGlobal();
  if (inner) {
    inner->SaveStorageAccessPermissionGranted();
  }

  return IPC_OK();
}

mozilla::ipc::IPCResult WindowGlobalChild::RecvDispatchSecurityPolicyViolation(
    const nsString& aViolationEventJSON) {
  nsGlobalWindowInner* window = GetWindowGlobal();
  if (!window) {
    return IPC_OK();
  }

  Document* doc = window->GetDocument();
  if (!doc) {
    return IPC_OK();
  }

  SecurityPolicyViolationEventInit violationEvent;
  if (!violationEvent.Init(aViolationEventJSON)) {
    return IPC_OK();
  }

  RefPtr<Event> event = SecurityPolicyViolationEvent::Constructor(
      doc, u"securitypolicyviolation"_ns, violationEvent);
  event->SetTrusted(true);
  doc->DispatchEvent(*event, IgnoreErrors());
  return IPC_OK();
}

mozilla::ipc::IPCResult WindowGlobalChild::RecvAddBlockedFrameNodeByClassifier(
    const MaybeDiscardedBrowsingContext& aNode) {
  if (aNode.IsNullOrDiscarded()) {
    return IPC_OK();
  }

  nsGlobalWindowInner* window = GetWindowGlobal();
  if (!window) {
    return IPC_OK();
  }

  Document* doc = window->GetDocument();
  if (!doc) {
    return IPC_OK();
  }

  MOZ_ASSERT(aNode.get()->GetEmbedderElement()->OwnerDoc() == doc);
  doc->AddBlockedNodeByClassifier(aNode.get()->GetEmbedderElement());
  return IPC_OK();
}

mozilla::ipc::IPCResult WindowGlobalChild::RecvResetScalingZoom() {
  if (Document* doc = mWindowGlobal->GetExtantDoc()) {
    if (PresShell* ps = doc->GetPresShell()) {
      ps->SetResolutionAndScaleTo(1.0,
                                  ResolutionChangeOrigin::MainThreadAdjustment);
    }
  }
  return IPC_OK();
}

mozilla::ipc::IPCResult WindowGlobalChild::RecvRestoreDocShellState(
    const dom::sessionstore::DocShellRestoreState& aState,
    RestoreDocShellStateResolver&& aResolve) {
  if (mWindowGlobal) {
    SessionStoreUtils::RestoreDocShellState(mWindowGlobal->GetDocShell(),
                                            aState);
  }
  aResolve(true);
  return IPC_OK();
}

mozilla::ipc::IPCResult WindowGlobalChild::RecvRestoreTabContent(
    dom::SessionStoreRestoreData* aData, RestoreTabContentResolver&& aResolve) {
  aData->RestoreInto(BrowsingContext());
  aResolve(true);
  return IPC_OK();
}

IPCResult WindowGlobalChild::RecvRawMessage(
    const JSActorMessageMeta& aMeta, const Maybe<ClonedMessageData>& aData,
    const Maybe<ClonedMessageData>& aStack) {
  Maybe<StructuredCloneData> data;
  if (aData) {
    data.emplace();
    data->BorrowFromClonedMessageData(*aData);
  }
  Maybe<StructuredCloneData> stack;
  if (aStack) {
    stack.emplace();
    stack->BorrowFromClonedMessageData(*aStack);
  }
  ReceiveRawMessage(aMeta, std::move(data), std::move(stack));
  return IPC_OK();
}

void WindowGlobalChild::SetDocumentURI(nsIURI* aDocumentURI) {
  // Registers a DOM Window with the profiler. It re-registers the same Inner
  // Window ID with different URIs because when a Browsing context is first
  // loaded, the first url loaded in it will be about:blank. This call keeps the
  // first non-about:blank registration of window and discards the previous one.
  uint64_t embedderInnerWindowID = 0;
  if (BrowsingContext()->GetParent()) {
    embedderInnerWindowID = BrowsingContext()->GetEmbedderInnerWindowId();
  }
  profiler_register_page(
      BrowsingContext()->BrowserId(), InnerWindowId(),
      nsContentUtils::TruncatedURLForDisplay(aDocumentURI, 1024),
      embedderInnerWindowID, BrowsingContext()->UsePrivateBrowsing());
  mDocumentURI = aDocumentURI;
  SendUpdateDocumentURI(aDocumentURI);
}

void WindowGlobalChild::SetDocumentPrincipal(
    nsIPrincipal* aNewDocumentPrincipal,
    nsIPrincipal* aNewDocumentStoragePrincipal) {
  MOZ_ASSERT(mDocumentPrincipal->Equals(aNewDocumentPrincipal));
  mDocumentPrincipal = aNewDocumentPrincipal;
  SendUpdateDocumentPrincipal(aNewDocumentPrincipal,
                              aNewDocumentStoragePrincipal);
}

const nsACString& WindowGlobalChild::GetRemoteType() {
  if (XRE_IsContentProcess()) {
    return ContentChild::GetSingleton()->GetRemoteType();
  }

  return NOT_REMOTE_TYPE;
}

already_AddRefed<JSWindowActorChild> WindowGlobalChild::GetActor(
    JSContext* aCx, const nsACString& aName, ErrorResult& aRv) {
  return JSActorManager::GetActor(aCx, aName, aRv)
      .downcast<JSWindowActorChild>();
}

already_AddRefed<JSWindowActorChild> WindowGlobalChild::GetExistingActor(
    const nsACString& aName) {
  return JSActorManager::GetExistingActor(aName).downcast<JSWindowActorChild>();
}

already_AddRefed<JSActor> WindowGlobalChild::InitJSActor(
    JS::Handle<JSObject*> aMaybeActor, const nsACString& aName,
    ErrorResult& aRv) {
  RefPtr<JSWindowActorChild> actor;
  if (aMaybeActor.get()) {
    aRv = UNWRAP_OBJECT(JSWindowActorChild, aMaybeActor.get(), actor);
    if (aRv.Failed()) {
      return nullptr;
    }
  } else {
    actor = new JSWindowActorChild();
  }

  MOZ_RELEASE_ASSERT(!actor->GetManager(),
                     "mManager was already initialized once!");
  actor->Init(aName, this);
  return actor.forget();
}

void WindowGlobalChild::ActorDestroy(ActorDestroyReason aWhy) {
  MOZ_ASSERT(nsContentUtils::IsSafeToRunScript(),
             "Destroying WindowGlobalChild can run script");

  // If our WindowContext hasn't been marked as discarded yet, ensure it's
  // marked as discarded at this point.
  mWindowContext->Discard();

  profiler_unregister_page(InnerWindowId());

  // Destroy our JSActors, and reject any pending queries.
  JSActorDidDestroy();
}

bool WindowGlobalChild::IsSameOriginWith(
    const dom::WindowContext* aOther) const {
  if (aOther == WindowContext()) {
    return true;
  }

  MOZ_DIAGNOSTIC_ASSERT(WindowContext()->Group() == aOther->Group());
  if (nsGlobalWindowInner* otherWin = aOther->GetInnerWindow()) {
    return mDocumentPrincipal->Equals(otherWin->GetPrincipal());
  }
  return false;
}

bool WindowGlobalChild::SameOriginWithTop() {
  return IsSameOriginWith(WindowContext()->TopWindowContext());
}

// For historical context, see:
//
// Bug 13871:   Prevent frameset spoofing
// Bug 103638:  Targets with same name in different windows open in wrong
//              window with javascript
// Bug 408052:  Adopt "ancestor" frame navigation policy
// Bug 1570207: Refactor logic to rely on BrowsingContextGroups to enforce
//              origin attribute isolation
// Bug 1810619: Crash at null in nsDocShell::ValidateOrigin
bool WindowGlobalChild::CanNavigate(dom::BrowsingContext* aTarget,
                                    bool aConsiderOpener) {
  MOZ_DIAGNOSTIC_ASSERT(WindowContext()->Group() == aTarget->Group(),
                        "A WindowGlobalChild should never try to navigate a "
                        "BrowsingContext from another group");

  auto isFileScheme = [](nsIPrincipal* aPrincipal) -> bool {
    // NOTE: This code previously checked for a file scheme using
    // `nsIPrincipal::GetURI()` combined with `NS_GetInnermostURI`. We no longer
    // use GetURI, as it has been deprecated, and it makes more sense to take
    // advantage of the pre-computed origin, which will already use the
    // innermost URI (bug 1810619)
    nsAutoCString origin, scheme;
    return NS_SUCCEEDED(aPrincipal->GetOriginNoSuffix(origin)) &&
           NS_SUCCEEDED(net_ExtractURLScheme(origin, scheme)) &&
           scheme == "file"_ns;
  };

  // A frame can navigate itself and its own root.
  if (aTarget == BrowsingContext() || aTarget == BrowsingContext()->Top()) {
    return true;
  }

  // If the target frame doesn't yet have a WindowContext, start checking
  // principals from its direct ancestor instead. It would inherit its principal
  // from this document upon creation.
  dom::WindowContext* initialWc = aTarget->GetCurrentWindowContext();
  if (!initialWc) {
    initialWc = aTarget->GetParentWindowContext();
  }

  // A frame can navigate any frame with a same-origin ancestor.
  bool isFileDocument = isFileScheme(DocumentPrincipal());
  for (dom::WindowContext* wc = initialWc; wc;
       wc = wc->GetParentWindowContext()) {
    dom::WindowGlobalChild* wgc = wc->GetWindowGlobalChild();
    if (!wgc) {
      continue;  // out-of process, so not same-origin.
    }

    if (DocumentPrincipal()->Equals(wgc->DocumentPrincipal())) {
      return true;
    }

    // Not strictly equal, special case if both are file: URIs.
    //
    // file: URIs are considered the same domain for the purpose of frame
    // navigation, regardless of script accessibility (bug 420425).
    if (isFileDocument && isFileScheme(wgc->DocumentPrincipal())) {
      return true;
    }
  }

  // If the target is a top-level document, a frame can navigate it if it can
  // navigate its opener.
  if (aConsiderOpener && !aTarget->GetParent()) {
    if (RefPtr<dom::BrowsingContext> opener = aTarget->GetOpener()) {
      return CanNavigate(opener, false);
    }
  }

  return false;
}

// FindWithName follows the rules for choosing a browsing context,
// with the exception of sandboxing for iframes. The implementation
// for arbitrarily choosing between two browsing contexts with the
// same name is as follows:
//
// 1) The start browsing context, i.e. 'this'
// 2) Descendants in insertion order
// 3) The parent
// 4) Siblings and their children, both in insertion order
// 5) After this we iteratively follow the parent chain, repeating 3
//    and 4 until
// 6) If there is no parent, consider all other top level browsing
//    contexts and their children, both in insertion order
//
// See
// https://html.spec.whatwg.org/multipage/browsers.html#the-rules-for-choosing-a-browsing-context-given-a-browsing-context-name
dom::BrowsingContext* WindowGlobalChild::FindBrowsingContextWithName(
    const nsAString& aName, bool aUseEntryGlobalForAccessCheck) {
  RefPtr<WindowGlobalChild> requestingContext = this;
  if (aUseEntryGlobalForAccessCheck) {
    if (nsGlobalWindowInner* caller = nsContentUtils::EntryInnerWindow()) {
      if (caller->GetBrowsingContextGroup() == WindowContext()->Group()) {
        requestingContext = caller->GetWindowGlobalChild();
      } else {
        MOZ_RELEASE_ASSERT(caller->GetPrincipal()->IsSystemPrincipal(),
                           "caller must be either same-group or system");
      }
    }
  }
  MOZ_ASSERT(requestingContext, "must have a requestingContext");

  dom::BrowsingContext* found = nullptr;
  if (aName.IsEmpty()) {
    // You can't find a browsing context with an empty name.
    found = nullptr;
  } else if (aName.LowerCaseEqualsLiteral("_blank")) {
    // Just return null. Caller must handle creating a new window with
    // a blank name.
    found = nullptr;
  } else if (nsContentUtils::IsSpecialName(aName)) {
    found = BrowsingContext()->FindWithSpecialName(aName, *requestingContext);
  } else if (dom::BrowsingContext* child =
                 BrowsingContext()->FindWithNameInSubtree(aName,
                                                          requestingContext)) {
    found = child;
  } else {
    dom::WindowContext* current = WindowContext();

    do {
      Span<RefPtr<dom::BrowsingContext>> siblings;
      dom::WindowContext* parent = current->GetParentWindowContext();

      if (!parent) {
        // We've reached the root of the tree, consider browsing
        // contexts in the same browsing context group.
        siblings = WindowContext()->Group()->Toplevels();
      } else if (dom::BrowsingContext* bc = parent->GetBrowsingContext();
                 bc && bc->NameEquals(aName) &&
                 requestingContext->CanNavigate(bc) && bc->IsTargetable()) {
        found = bc;
        break;
      } else {
        siblings = parent->NonSyntheticChildren();
      }

      for (dom::BrowsingContext* sibling : siblings) {
        if (sibling == current->GetBrowsingContext()) {
          continue;
        }

        if (dom::BrowsingContext* relative =
                sibling->FindWithNameInSubtree(aName, requestingContext)) {
          found = relative;
          // Breaks the outer loop
          parent = nullptr;
          break;
        }
      }

      current = parent;
    } while (current);
  }

  // Helpers should perform access control checks, which means that we
  // only need to assert that we can access found.
  MOZ_DIAGNOSTIC_ASSERT(!found || requestingContext->CanNavigate(found));

  return found;
}

void WindowGlobalChild::UnblockBFCacheFor(BFCacheStatus aStatus) {
  SendUpdateBFCacheStatus(0, aStatus);
}

void WindowGlobalChild::BlockBFCacheFor(BFCacheStatus aStatus) {
  SendUpdateBFCacheStatus(aStatus, 0);
}

WindowGlobalChild::~WindowGlobalChild() = default;

JSObject* WindowGlobalChild::WrapObject(JSContext* aCx,
                                        JS::Handle<JSObject*> aGivenProto) {
  return WindowGlobalChild_Binding::Wrap(aCx, this, aGivenProto);
}

nsISupports* WindowGlobalChild::GetParentObject() {
  return xpc::NativeGlobal(xpc::PrivilegedJunkScope());
}

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE_WEAK_PTR(WindowGlobalChild, mWindowGlobal,
                                               mContainerFeaturePolicy,
                                               mWindowContext)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(WindowGlobalChild)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

NS_IMPL_CYCLE_COLLECTING_ADDREF(WindowGlobalChild)
NS_IMPL_CYCLE_COLLECTING_RELEASE(WindowGlobalChild)

}  // namespace mozilla::dom
