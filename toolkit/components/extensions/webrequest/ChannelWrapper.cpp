/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ChannelWrapper.h"

#include "jsapi.h"
#include "xpcpublic.h"

#include "mozilla/BasePrincipal.h"
#include "mozilla/SystemPrincipal.h"

#include "NSSErrorsService.h"
#include "nsITransportSecurityInfo.h"

#include "mozilla/AddonManagerWebAPI.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/Components.h"
#include "mozilla/ErrorNames.h"
#include "mozilla/ResultExtensions.h"
#include "mozilla/Unused.h"
#include "mozilla/dom/Element.h"
#include "mozilla/dom/Event.h"
#include "mozilla/dom/EventBinding.h"
#include "mozilla/dom/BrowserHost.h"
#include "mozIThirdPartyUtil.h"
#include "nsContentUtils.h"
#include "nsIContentPolicy.h"
#include "nsIClassifiedChannel.h"
#include "nsIHttpChannelInternal.h"
#include "nsIHttpHeaderVisitor.h"
#include "nsIInterfaceRequestor.h"
#include "nsIInterfaceRequestorUtils.h"
#include "nsILoadContext.h"
#include "nsIProxiedChannel.h"
#include "nsIProxyInfo.h"
#include "nsITraceableChannel.h"
#include "nsIWritablePropertyBag.h"
#include "nsIWritablePropertyBag2.h"
#include "nsNetUtil.h"
#include "nsProxyRelease.h"
#include "nsPrintfCString.h"

using namespace mozilla::dom;
using namespace JS;

namespace mozilla {
namespace extensions {

#define CHANNELWRAPPER_PROP_KEY u"ChannelWrapper::CachedInstance"_ns

using CF = nsIClassifiedChannel::ClassificationFlags;
using MUC = MozUrlClassificationFlags;

struct ClassificationStruct {
  uint32_t mFlag;
  MozUrlClassificationFlags mValue;
};
static const ClassificationStruct classificationArray[] = {
    {CF::CLASSIFIED_FINGERPRINTING, MUC::Fingerprinting},
    {CF::CLASSIFIED_FINGERPRINTING_CONTENT, MUC::Fingerprinting_content},
    {CF::CLASSIFIED_CRYPTOMINING, MUC::Cryptomining},
    {CF::CLASSIFIED_CRYPTOMINING_CONTENT, MUC::Cryptomining_content},
    {CF::CLASSIFIED_EMAILTRACKING, MUC::Emailtracking},
    {CF::CLASSIFIED_EMAILTRACKING_CONTENT, MUC::Emailtracking_content},
    {CF::CLASSIFIED_TRACKING, MUC::Tracking},
    {CF::CLASSIFIED_TRACKING_AD, MUC::Tracking_ad},
    {CF::CLASSIFIED_TRACKING_ANALYTICS, MUC::Tracking_analytics},
    {CF::CLASSIFIED_TRACKING_SOCIAL, MUC::Tracking_social},
    {CF::CLASSIFIED_TRACKING_CONTENT, MUC::Tracking_content},
    {CF::CLASSIFIED_SOCIALTRACKING, MUC::Socialtracking},
    {CF::CLASSIFIED_SOCIALTRACKING_FACEBOOK, MUC::Socialtracking_facebook},
    {CF::CLASSIFIED_SOCIALTRACKING_LINKEDIN, MUC::Socialtracking_linkedin},
    {CF::CLASSIFIED_SOCIALTRACKING_TWITTER, MUC::Socialtracking_twitter},
    {CF::CLASSIFIED_ANY_BASIC_TRACKING, MUC::Any_basic_tracking},
    {CF::CLASSIFIED_ANY_STRICT_TRACKING, MUC::Any_strict_tracking},
    {CF::CLASSIFIED_ANY_SOCIAL_TRACKING, MUC::Any_social_tracking}};

/*****************************************************************************
 * Lifetimes
 *****************************************************************************/

namespace {
class ChannelListHolder : public LinkedList<ChannelWrapper> {
 public:
  ChannelListHolder() : LinkedList<ChannelWrapper>() {}

  ~ChannelListHolder();
};

}  // anonymous namespace

ChannelListHolder::~ChannelListHolder() {
  while (ChannelWrapper* wrapper = popFirst()) {
    wrapper->Die();
  }
}

static LinkedList<ChannelWrapper>* GetChannelList() {
  static UniquePtr<ChannelListHolder> sChannelList;
  if (!sChannelList && !PastShutdownPhase(ShutdownPhase::XPCOMShutdown)) {
    sChannelList.reset(new ChannelListHolder());
    ClearOnShutdown(&sChannelList, ShutdownPhase::XPCOMShutdown);
  }
  return sChannelList.get();
}

NS_IMPL_CYCLE_COLLECTING_ADDREF(ChannelWrapper::ChannelWrapperStub)
NS_IMPL_CYCLE_COLLECTING_RELEASE(ChannelWrapper::ChannelWrapperStub)

NS_IMPL_CYCLE_COLLECTION(ChannelWrapper::ChannelWrapperStub, mChannelWrapper)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(ChannelWrapper::ChannelWrapperStub)
  NS_INTERFACE_MAP_ENTRY_TEAROFF_AMBIGUOUS(ChannelWrapper, EventTarget,
                                           mChannelWrapper)
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

/*****************************************************************************
 * Initialization
 *****************************************************************************/

ChannelWrapper::ChannelWrapper(nsISupports* aParent, nsIChannel* aChannel)
    : ChannelHolder(aChannel), mParent(aParent) {
  mStub = new ChannelWrapperStub(this);

  if (auto* list = GetChannelList()) {
    list->insertBack(this);
  }
}

ChannelWrapper::~ChannelWrapper() {
  if (LinkedListElement<ChannelWrapper>::isInList()) {
    LinkedListElement<ChannelWrapper>::remove();
  }
}

void ChannelWrapper::Die() {
  if (mStub) {
    mStub->mChannelWrapper = nullptr;
  }
}

/* static */
already_AddRefed<ChannelWrapper> ChannelWrapper::Get(const GlobalObject& global,
                                                     nsIChannel* channel) {
  RefPtr<ChannelWrapper> wrapper;

  nsCOMPtr<nsIWritablePropertyBag2> props = do_QueryInterface(channel);
  if (props) {
    wrapper = do_GetProperty(props, CHANNELWRAPPER_PROP_KEY);
    if (wrapper) {
      // Assume cached attributes may have changed at this point.
      wrapper->ClearCachedAttributes();
    }
  }

  if (!wrapper) {
    wrapper = new ChannelWrapper(global.GetAsSupports(), channel);
    if (props) {
      Unused << props->SetPropertyAsInterface(CHANNELWRAPPER_PROP_KEY,
                                              wrapper->mStub);
    }
  }

  return wrapper.forget();
}

already_AddRefed<ChannelWrapper> ChannelWrapper::GetRegisteredChannel(
    const GlobalObject& global, uint64_t aChannelId,
    const WebExtensionPolicy& aAddon, nsIRemoteTab* aRemoteTab) {
  ContentParent* contentParent = nullptr;
  if (BrowserHost* host = BrowserHost::GetFrom(aRemoteTab)) {
    contentParent = host->GetActor()->Manager();
  }

  auto& webreq = WebRequestService::GetSingleton();

  nsCOMPtr<nsITraceableChannel> channel =
      webreq.GetTraceableChannel(aChannelId, aAddon, contentParent);
  if (!channel) {
    return nullptr;
  }
  nsCOMPtr<nsIChannel> chan(do_QueryInterface(channel));
  return ChannelWrapper::Get(global, chan);
}

void ChannelWrapper::SetChannel(nsIChannel* aChannel) {
  detail::ChannelHolder::SetChannel(aChannel);
  ClearCachedAttributes();
  ChannelWrapper_Binding::ClearCachedFinalURIValue(this);
  ChannelWrapper_Binding::ClearCachedFinalURLValue(this);
  mFinalURLInfo.reset();
  ChannelWrapper_Binding::ClearCachedProxyInfoValue(this);
}

void ChannelWrapper::ClearCachedAttributes() {
  ChannelWrapper_Binding::ClearCachedRemoteAddressValue(this);
  ChannelWrapper_Binding::ClearCachedStatusCodeValue(this);
  ChannelWrapper_Binding::ClearCachedStatusLineValue(this);
  ChannelWrapper_Binding::ClearCachedUrlClassificationValue(this);
  if (!mFiredErrorEvent) {
    ChannelWrapper_Binding::ClearCachedErrorStringValue(this);
  }

  ChannelWrapper_Binding::ClearCachedRequestSizeValue(this);
  ChannelWrapper_Binding::ClearCachedResponseSizeValue(this);
}

/*****************************************************************************
 * ...
 *****************************************************************************/

void ChannelWrapper::Cancel(uint32_t aResult, uint32_t aReason,
                            ErrorResult& aRv) {
  nsresult rv = NS_ERROR_UNEXPECTED;
  if (nsCOMPtr<nsIChannel> chan = MaybeChannel()) {
    nsCOMPtr<nsILoadInfo> loadInfo = GetLoadInfo();
    if (aReason > 0 && loadInfo) {
      loadInfo->SetRequestBlockingReason(aReason);
    }
    rv = chan->Cancel(nsresult(aResult));
    ErrorCheck();
  }
  if (NS_FAILED(rv)) {
    aRv.Throw(rv);
  }
}

void ChannelWrapper::RedirectTo(nsIURI* aURI, ErrorResult& aRv) {
  nsresult rv = NS_ERROR_UNEXPECTED;
  if (nsCOMPtr<nsIHttpChannel> chan = MaybeHttpChannel()) {
    rv = chan->RedirectTo(aURI);
  }
  if (NS_FAILED(rv)) {
    aRv.Throw(rv);
  }
}

void ChannelWrapper::UpgradeToSecure(ErrorResult& aRv) {
  nsresult rv = NS_ERROR_UNEXPECTED;
  if (nsCOMPtr<nsIHttpChannel> chan = MaybeHttpChannel()) {
    rv = chan->UpgradeToSecure();
  }
  if (NS_FAILED(rv)) {
    aRv.Throw(rv);
  }
}

void ChannelWrapper::Suspend(const nsCString& aProfileMarkerText,
                             ErrorResult& aRv) {
  if (!mSuspended) {
    nsresult rv = NS_ERROR_UNEXPECTED;
    if (nsCOMPtr<nsIChannel> chan = MaybeChannel()) {
      rv = chan->Suspend();
    }
    if (NS_FAILED(rv)) {
      aRv.Throw(rv);
    } else {
      mSuspended = true;
      MOZ_ASSERT(mSuspendedMarkerText.IsVoid());
      mSuspendedMarkerText = aProfileMarkerText;
      PROFILER_MARKER_TEXT("Extension Suspend", NETWORK,
                           MarkerOptions(MarkerTiming::IntervalStart()),
                           mSuspendedMarkerText);
    }
  }
}

void ChannelWrapper::Resume(ErrorResult& aRv) {
  if (mSuspended) {
    nsresult rv = NS_ERROR_UNEXPECTED;
    if (nsCOMPtr<nsIChannel> chan = MaybeChannel()) {
      rv = chan->Resume();
    }
    if (NS_FAILED(rv)) {
      aRv.Throw(rv);
    } else {
      mSuspended = false;
      PROFILER_MARKER_TEXT("Extension Suspend", NETWORK,
                           MarkerOptions(MarkerTiming::IntervalEnd()),
                           mSuspendedMarkerText);
      mSuspendedMarkerText = VoidCString();
    }
  }
}

void ChannelWrapper::GetContentType(nsCString& aContentType) const {
  if (nsCOMPtr<nsIHttpChannel> chan = MaybeHttpChannel()) {
    Unused << chan->GetContentType(aContentType);
  }
}

void ChannelWrapper::SetContentType(const nsACString& aContentType) {
  if (nsCOMPtr<nsIHttpChannel> chan = MaybeHttpChannel()) {
    Unused << chan->SetContentType(aContentType);
  }
}

/*****************************************************************************
 * Headers
 *****************************************************************************/

namespace {

class MOZ_STACK_CLASS HeaderVisitor final : public nsIHttpHeaderVisitor {
 public:
  NS_DECL_NSIHTTPHEADERVISITOR

  explicit HeaderVisitor(nsTArray<dom::MozHTTPHeader>& aHeaders)
      : mHeaders(aHeaders) {}

  HeaderVisitor(nsTArray<dom::MozHTTPHeader>& aHeaders,
                const nsCString& aContentTypeHdr)
      : mHeaders(aHeaders), mContentTypeHdr(aContentTypeHdr) {}

  void VisitRequestHeaders(nsIHttpChannel* aChannel, ErrorResult& aRv) {
    CheckResult(aChannel->VisitRequestHeaders(this), aRv);
  }

  void VisitResponseHeaders(nsIHttpChannel* aChannel, ErrorResult& aRv) {
    CheckResult(aChannel->VisitResponseHeaders(this), aRv);
  }

  NS_IMETHOD QueryInterface(REFNSIID aIID, void** aInstancePtr) override;

  // Stub AddRef/Release since this is a stack class.
  NS_IMETHOD_(MozExternalRefCountType) AddRef(void) override {
    return ++mRefCnt;
  }

  NS_IMETHOD_(MozExternalRefCountType) Release(void) override {
    return --mRefCnt;
  }

  virtual ~HeaderVisitor() { MOZ_DIAGNOSTIC_ASSERT(mRefCnt == 0); }

 private:
  bool CheckResult(nsresult aNSRv, ErrorResult& aRv) {
    if (NS_FAILED(aNSRv)) {
      aRv.Throw(aNSRv);
      return false;
    }
    return true;
  }

  nsTArray<dom::MozHTTPHeader>& mHeaders;
  nsCString mContentTypeHdr = VoidCString();

  nsrefcnt mRefCnt = 0;
};

NS_IMETHODIMP
HeaderVisitor::VisitHeader(const nsACString& aHeader,
                           const nsACString& aValue) {
  auto dict = mHeaders.AppendElement(fallible);
  if (!dict) {
    return NS_ERROR_OUT_OF_MEMORY;
  }
  dict->mName = aHeader;

  if (!mContentTypeHdr.IsVoid() &&
      aHeader.LowerCaseEqualsLiteral("content-type")) {
    dict->mValue = mContentTypeHdr;
  } else {
    dict->mValue = aValue;
  }

  return NS_OK;
}

NS_IMPL_QUERY_INTERFACE(HeaderVisitor, nsIHttpHeaderVisitor)

}  // anonymous namespace

void ChannelWrapper::GetRequestHeaders(nsTArray<dom::MozHTTPHeader>& aRetVal,
                                       ErrorResult& aRv) const {
  if (nsCOMPtr<nsIHttpChannel> chan = MaybeHttpChannel()) {
    HeaderVisitor visitor(aRetVal);
    visitor.VisitRequestHeaders(chan, aRv);
  } else {
    aRv.Throw(NS_ERROR_UNEXPECTED);
  }
}

void ChannelWrapper::GetRequestHeader(const nsCString& aHeader,
                                      nsCString& aResult,
                                      ErrorResult& aRv) const {
  aResult.SetIsVoid(true);
  if (nsCOMPtr<nsIHttpChannel> chan = MaybeHttpChannel()) {
    Unused << chan->GetRequestHeader(aHeader, aResult);
  } else {
    aRv.Throw(NS_ERROR_UNEXPECTED);
  }
}

void ChannelWrapper::GetResponseHeaders(nsTArray<dom::MozHTTPHeader>& aRetVal,
                                        ErrorResult& aRv) const {
  if (nsCOMPtr<nsIHttpChannel> chan = MaybeHttpChannel()) {
    HeaderVisitor visitor(aRetVal, mContentTypeHdr);
    visitor.VisitResponseHeaders(chan, aRv);
  } else {
    aRv.Throw(NS_ERROR_UNEXPECTED);
  }
}

void ChannelWrapper::SetRequestHeader(const nsCString& aHeader,
                                      const nsCString& aValue, bool aMerge,
                                      ErrorResult& aRv) {
  nsresult rv = NS_ERROR_UNEXPECTED;
  if (nsCOMPtr<nsIHttpChannel> chan = MaybeHttpChannel()) {
    rv = chan->SetRequestHeader(aHeader, aValue, aMerge);
  }
  if (NS_FAILED(rv)) {
    aRv.Throw(rv);
  }
}

void ChannelWrapper::SetResponseHeader(const nsCString& aHeader,
                                       const nsCString& aValue, bool aMerge,
                                       ErrorResult& aRv) {
  nsresult rv = NS_ERROR_UNEXPECTED;
  if (nsCOMPtr<nsIHttpChannel> chan = MaybeHttpChannel()) {
    if (aHeader.LowerCaseEqualsLiteral("content-type")) {
      rv = chan->SetContentType(aValue);
      if (NS_SUCCEEDED(rv)) {
        mContentTypeHdr = aValue;
      }
    } else {
      rv = chan->SetResponseHeader(aHeader, aValue, aMerge);
    }
  }
  if (NS_FAILED(rv)) {
    aRv.Throw(rv);
  }
}

/*****************************************************************************
 * LoadInfo
 *****************************************************************************/

already_AddRefed<nsILoadContext> ChannelWrapper::GetLoadContext() const {
  if (nsCOMPtr<nsIChannel> chan = MaybeChannel()) {
    nsCOMPtr<nsILoadContext> ctxt;
    // Fetch() from Workers saves BrowsingContext/LoadContext information in
    // nsILoadInfo.workerAssociatedBrowsingContext. So we can not use
    // NS_QueryNotificationCallbacks to get LoadContext of the channel.
    RefPtr<BrowsingContext> bc;
    nsCOMPtr<nsILoadInfo> loadInfo = chan->LoadInfo();
    loadInfo->GetWorkerAssociatedBrowsingContext(getter_AddRefs(bc));
    if (bc) {
      ctxt = bc.forget();
      return ctxt.forget();
    }
    NS_QueryNotificationCallbacks(chan, ctxt);
    return ctxt.forget();
  }
  return nullptr;
}

already_AddRefed<Element> ChannelWrapper::GetBrowserElement() const {
  if (nsCOMPtr<nsILoadContext> ctxt = GetLoadContext()) {
    RefPtr<Element> elem;
    if (NS_SUCCEEDED(ctxt->GetTopFrameElement(getter_AddRefs(elem)))) {
      return elem.forget();
    }
  }
  return nullptr;
}

bool ChannelWrapper::IsServiceWorkerScript() const {
  nsCOMPtr<nsIChannel> chan = MaybeChannel();
  return IsServiceWorkerScript(chan);
}

// static
bool ChannelWrapper::IsServiceWorkerScript(const nsCOMPtr<nsIChannel>& chan) {
  nsCOMPtr<nsILoadInfo> loadInfo;

  if (chan) {
    chan->GetLoadInfo(getter_AddRefs(loadInfo));
  }

  if (loadInfo) {
    // Not a script.
    if (loadInfo->GetExternalContentPolicyType() !=
        ExtContentPolicy::TYPE_SCRIPT) {
      return false;
    }

    // Service worker main script load.
    if (loadInfo->InternalContentPolicyType() ==
        nsIContentPolicy::TYPE_INTERNAL_SERVICE_WORKER) {
      return true;
    }

    // Service worker import scripts load.
    if (loadInfo->InternalContentPolicyType() ==
            nsIContentPolicy::TYPE_INTERNAL_WORKER_IMPORT_SCRIPTS ||
        loadInfo->InternalContentPolicyType() ==
            nsIContentPolicy::TYPE_INTERNAL_WORKER_STATIC_MODULE) {
      nsLoadFlags loadFlags = 0;
      chan->GetLoadFlags(&loadFlags);
      return loadFlags & nsIChannel::LOAD_BYPASS_SERVICE_WORKER;
    }
  }

  return false;
}

static inline bool IsSystemPrincipal(nsIPrincipal* aPrincipal) {
  return BasePrincipal::Cast(aPrincipal)->Is<SystemPrincipal>();
}

bool ChannelWrapper::IsSystemLoad() const {
  if (nsCOMPtr<nsILoadInfo> loadInfo = GetLoadInfo()) {
    if (nsIPrincipal* prin = loadInfo->GetLoadingPrincipal()) {
      return IsSystemPrincipal(prin);
    }

    if (RefPtr<BrowsingContext> bc = loadInfo->GetBrowsingContext();
        !bc || bc->IsTop()) {
      return false;
    }

    if (nsIPrincipal* prin = loadInfo->PrincipalToInherit()) {
      return IsSystemPrincipal(prin);
    }
    if (nsIPrincipal* prin = loadInfo->TriggeringPrincipal()) {
      return IsSystemPrincipal(prin);
    }
  }
  return false;
}

bool ChannelWrapper::CanModify() const {
  if (WebExtensionPolicy::IsRestrictedURI(FinalURLInfo())) {
    return false;
  }

  if (nsCOMPtr<nsILoadInfo> loadInfo = GetLoadInfo()) {
    if (nsIPrincipal* prin = loadInfo->GetLoadingPrincipal()) {
      if (IsSystemPrincipal(prin)) {
        return false;
      }

      auto* docURI = DocumentURLInfo();
      if (docURI && WebExtensionPolicy::IsRestrictedURI(*docURI)) {
        return false;
      }
    }
  }
  return true;
}

already_AddRefed<nsIURI> ChannelWrapper::GetOriginURI() const {
  nsCOMPtr<nsIURI> uri;
  if (nsCOMPtr<nsILoadInfo> loadInfo = GetLoadInfo()) {
    if (nsIPrincipal* prin = loadInfo->TriggeringPrincipal()) {
      if (prin->GetIsContentPrincipal()) {
        auto* basePrin = BasePrincipal::Cast(prin);
        Unused << basePrin->GetURI(getter_AddRefs(uri));
      }
    }
  }
  return uri.forget();
}

already_AddRefed<nsIURI> ChannelWrapper::GetDocumentURI() const {
  nsCOMPtr<nsIURI> uri;
  if (nsCOMPtr<nsILoadInfo> loadInfo = GetLoadInfo()) {
    if (nsIPrincipal* prin = loadInfo->GetLoadingPrincipal()) {
      if (prin->GetIsContentPrincipal()) {
        auto* basePrin = BasePrincipal::Cast(prin);
        Unused << basePrin->GetURI(getter_AddRefs(uri));
      }
    }
  }
  return uri.forget();
}

void ChannelWrapper::GetOriginURL(nsCString& aRetVal) const {
  if (nsCOMPtr<nsIURI> uri = GetOriginURI()) {
    Unused << uri->GetSpec(aRetVal);
  }
}

void ChannelWrapper::GetDocumentURL(nsCString& aRetVal) const {
  if (nsCOMPtr<nsIURI> uri = GetDocumentURI()) {
    Unused << uri->GetSpec(aRetVal);
  }
}

const URLInfo& ChannelWrapper::FinalURLInfo() const {
  if (mFinalURLInfo.isNothing()) {
    ErrorResult rv;
    nsCOMPtr<nsIURI> uri = FinalURI();
    MOZ_ASSERT(uri);

    // If this is a view-source scheme, get the nested uri.
    while (uri && uri->SchemeIs("view-source")) {
      nsCOMPtr<nsINestedURI> nested = do_QueryInterface(uri);
      if (!nested) {
        break;
      }
      nested->GetInnerURI(getter_AddRefs(uri));
    }
    mFinalURLInfo.emplace(uri.get(), true);

    // If this is a WebSocket request, mangle the URL so that the scheme is
    // ws: or wss:, as appropriate.
    auto& url = mFinalURLInfo.ref();
    if (Type() == MozContentPolicyType::Websocket &&
        (url.Scheme() == nsGkAtoms::http || url.Scheme() == nsGkAtoms::https)) {
      nsAutoCString spec(url.CSpec());
      spec.Replace(0, 4, "ws"_ns);

      Unused << NS_NewURI(getter_AddRefs(uri), spec);
      MOZ_RELEASE_ASSERT(uri);
      mFinalURLInfo.reset();
      mFinalURLInfo.emplace(uri.get(), true);
    }
  }
  return mFinalURLInfo.ref();
}

const URLInfo* ChannelWrapper::DocumentURLInfo() const {
  if (mDocumentURLInfo.isNothing()) {
    nsCOMPtr<nsIURI> uri = GetDocumentURI();
    if (!uri) {
      return nullptr;
    }
    mDocumentURLInfo.emplace(uri.get(), true);
  }
  return &mDocumentURLInfo.ref();
}

bool ChannelWrapper::Matches(
    const dom::MozRequestFilter& aFilter, const WebExtensionPolicy* aExtension,
    const dom::MozRequestMatchOptions& aOptions) const {
  if (!HaveChannel()) {
    return false;
  }

  if (!aFilter.mTypes.IsNull() && !aFilter.mTypes.Value().Contains(Type())) {
    return false;
  }

  auto& urlInfo = FinalURLInfo();
  if (aFilter.mUrls && !aFilter.mUrls->Matches(urlInfo)) {
    return false;
  }

  nsCOMPtr<nsILoadInfo> loadInfo = GetLoadInfo();
  bool isPrivate =
      loadInfo && loadInfo->GetOriginAttributes().mPrivateBrowsingId > 0;
  if (!aFilter.mIncognito.IsNull() && aFilter.mIncognito.Value() != isPrivate) {
    return false;
  }

  if (aExtension) {
    // Verify extension access to private requests
    if (isPrivate && !aExtension->PrivateBrowsingAllowed()) {
      return false;
    }

    bool isProxy =
        aOptions.mIsProxy && aExtension->HasPermission(nsGkAtoms::proxy);
    // Proxies are allowed access to all urls, including restricted urls.
    if (!aExtension->CanAccessURI(urlInfo, false, !isProxy, true)) {
      return false;
    }

    // If this isn't the proxy phase of the request, check that the extension
    // has origin permissions for origin that originated the request.
    if (!isProxy) {
      if (IsSystemLoad()) {
        return false;
      }

      auto origin = DocumentURLInfo();
      // Extensions with the file:-permission may observe requests from file:
      // origins, because such documents can already be modified by content
      // scripts anyway.
      if (origin && !aExtension->CanAccessURI(*origin, false, true, true)) {
        return false;
      }
    }
  }

  return true;
}

int64_t NormalizeFrameID(nsILoadInfo* aLoadInfo, uint64_t bcID) {
  RefPtr<BrowsingContext> bc = aLoadInfo->GetWorkerAssociatedBrowsingContext();
  if (!bc) {
    bc = aLoadInfo->GetBrowsingContext();
  }

  if (!bc || bcID == bc->Top()->Id()) {
    return 0;
  }
  return bcID;
}

uint64_t ChannelWrapper::BrowsingContextId(nsILoadInfo* aLoadInfo) const {
  auto frameID = aLoadInfo->GetFrameBrowsingContextID();
  if (!frameID) {
    frameID = aLoadInfo->GetWorkerAssociatedBrowsingContextID();
  }
  if (!frameID) {
    frameID = aLoadInfo->GetBrowsingContextID();
  }
  return frameID;
}

int64_t ChannelWrapper::FrameId() const {
  if (nsCOMPtr<nsILoadInfo> loadInfo = GetLoadInfo()) {
    return NormalizeFrameID(loadInfo, BrowsingContextId(loadInfo));
  }
  return 0;
}

int64_t ChannelWrapper::ParentFrameId() const {
  if (nsCOMPtr<nsILoadInfo> loadInfo = GetLoadInfo()) {
    RefPtr<BrowsingContext> bc = loadInfo->GetWorkerAssociatedBrowsingContext();
    if (!bc) {
      bc = loadInfo->GetBrowsingContext();
    }
    if (bc) {
      if (BrowsingContextId(loadInfo) == bc->Top()->Id()) {
        return -1;
      }

      uint64_t parentID = -1;
      if (loadInfo->GetFrameBrowsingContextID()) {
        parentID = loadInfo->GetBrowsingContextID();
      } else if (bc->GetParent()) {
        parentID = bc->GetParent()->Id();
      }
      return NormalizeFrameID(loadInfo, parentID);
    }
  }
  return -1;
}

void ChannelWrapper::GetFrameAncestors(
    dom::Nullable<nsTArray<dom::MozFrameAncestorInfo>>& aFrameAncestors,
    ErrorResult& aRv) const {
  nsCOMPtr<nsILoadInfo> loadInfo = GetLoadInfo();
  if (!loadInfo || BrowsingContextId(loadInfo) == 0) {
    aFrameAncestors.SetNull();
    return;
  }

  nsresult rv = GetFrameAncestors(loadInfo, aFrameAncestors.SetValue());
  if (NS_FAILED(rv)) {
    aRv.Throw(rv);
  }
}

nsresult ChannelWrapper::GetFrameAncestors(
    nsILoadInfo* aLoadInfo,
    nsTArray<dom::MozFrameAncestorInfo>& aFrameAncestors) const {
  const nsTArray<nsCOMPtr<nsIPrincipal>>& ancestorPrincipals =
      aLoadInfo->AncestorPrincipals();
  const nsTArray<uint64_t>& ancestorBrowsingContextIDs =
      aLoadInfo->AncestorBrowsingContextIDs();
  uint32_t size = ancestorPrincipals.Length();
  MOZ_DIAGNOSTIC_ASSERT(size == ancestorBrowsingContextIDs.Length());
  if (size != ancestorBrowsingContextIDs.Length()) {
    return NS_ERROR_UNEXPECTED;
  }

  bool subFrame = aLoadInfo->GetExternalContentPolicyType() ==
                  ExtContentPolicy::TYPE_SUBDOCUMENT;
  if (!aFrameAncestors.SetCapacity(subFrame ? size : size + 1, fallible)) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  // The immediate parent is always the first element in the ancestor arrays,
  // however SUBDOCUMENTs do not have their immediate parent included, so we
  // inject it here. This will force wrapper.parentBrowsingContextId ==
  // wrapper.frameAncestors[0].frameId to always be true.  All ather requests
  // already match this way.
  if (subFrame) {
    auto ancestor = aFrameAncestors.AppendElement();
    GetDocumentURL(ancestor->mUrl);
    ancestor->mFrameId = ParentFrameId();
  }

  for (uint32_t i = 0; i < size; ++i) {
    auto ancestor = aFrameAncestors.AppendElement();
    MOZ_TRY(ancestorPrincipals[i]->GetAsciiSpec(ancestor->mUrl));
    ancestor->mFrameId =
        NormalizeFrameID(aLoadInfo, ancestorBrowsingContextIDs[i]);
  }
  return NS_OK;
}

/*****************************************************************************
 * Response filtering
 *****************************************************************************/

void ChannelWrapper::RegisterTraceableChannel(const WebExtensionPolicy& aAddon,
                                              nsIRemoteTab* aBrowserParent) {
  // We can't attach new listeners after the response has started, so don't
  // bother registering anything.
  if (mResponseStarted || !CanModify()) {
    return;
  }

  mAddonEntries.InsertOrUpdate(aAddon.Id(), aBrowserParent);
  if (!mChannelEntry) {
    mChannelEntry = WebRequestService::GetSingleton().RegisterChannel(this);
    CheckEventListeners();
  }
}

already_AddRefed<nsITraceableChannel> ChannelWrapper::GetTraceableChannel(
    const WebExtensionPolicy& aAddon,
    dom::ContentParent* aContentParent) const {
  nsCOMPtr<nsIRemoteTab> remoteTab;
  if (mAddonEntries.Get(aAddon.Id(), getter_AddRefs(remoteTab))) {
    if (FinalURLInfo().URI() &&
        !aAddon.CanAccessURI(FinalURLInfo(), false, true, true)) {
      return nullptr;
    }

    ContentParent* contentParent = nullptr;
    if (remoteTab) {
      contentParent =
          BrowserHost::GetFrom(remoteTab.get())->GetActor()->Manager();
    }

    if (contentParent == aContentParent) {
      nsCOMPtr<nsITraceableChannel> chan = QueryChannel();
      return chan.forget();
    }
  }
  return nullptr;
}

/*****************************************************************************
 * ...
 *****************************************************************************/

MozContentPolicyType GetContentPolicyType(ExtContentPolicyType aType) {
  // Note: Please keep this function in sync with the external types in
  // nsIContentPolicy.idl
  switch (aType) {
    case ExtContentPolicy::TYPE_DOCUMENT:
      return MozContentPolicyType::Main_frame;
    case ExtContentPolicy::TYPE_SUBDOCUMENT:
      return MozContentPolicyType::Sub_frame;
    case ExtContentPolicy::TYPE_STYLESHEET:
      return MozContentPolicyType::Stylesheet;
    case ExtContentPolicy::TYPE_SCRIPT:
      return MozContentPolicyType::Script;
    case ExtContentPolicy::TYPE_IMAGE:
      return MozContentPolicyType::Image;
    case ExtContentPolicy::TYPE_OBJECT:
      return MozContentPolicyType::Object;
    case ExtContentPolicy::TYPE_OBJECT_SUBREQUEST:
      return MozContentPolicyType::Object_subrequest;
    case ExtContentPolicy::TYPE_XMLHTTPREQUEST:
      return MozContentPolicyType::Xmlhttprequest;
    // TYPE_FETCH returns xmlhttprequest for cross-browser compatibility.
    case ExtContentPolicy::TYPE_FETCH:
      return MozContentPolicyType::Xmlhttprequest;
    case ExtContentPolicy::TYPE_XSLT:
      return MozContentPolicyType::Xslt;
    case ExtContentPolicy::TYPE_PING:
      return MozContentPolicyType::Ping;
    case ExtContentPolicy::TYPE_BEACON:
      return MozContentPolicyType::Beacon;
    case ExtContentPolicy::TYPE_DTD:
      return MozContentPolicyType::Xml_dtd;
    case ExtContentPolicy::TYPE_FONT:
    case ExtContentPolicy::TYPE_UA_FONT:
      return MozContentPolicyType::Font;
    case ExtContentPolicy::TYPE_MEDIA:
      return MozContentPolicyType::Media;
    case ExtContentPolicy::TYPE_WEBSOCKET:
      return MozContentPolicyType::Websocket;
    case ExtContentPolicy::TYPE_CSP_REPORT:
      return MozContentPolicyType::Csp_report;
    case ExtContentPolicy::TYPE_IMAGESET:
      return MozContentPolicyType::Imageset;
    case ExtContentPolicy::TYPE_WEB_MANIFEST:
      return MozContentPolicyType::Web_manifest;
    case ExtContentPolicy::TYPE_SPECULATIVE:
      return MozContentPolicyType::Speculative;
    case ExtContentPolicy::TYPE_PROXIED_WEBRTC_MEDIA:
    case ExtContentPolicy::TYPE_INVALID:
    case ExtContentPolicy::TYPE_OTHER:
    case ExtContentPolicy::TYPE_SAVEAS_DOWNLOAD:
      break;
      // Do not add default: so that compilers can catch the missing case.
  }
  return MozContentPolicyType::Other;
}

MozContentPolicyType ChannelWrapper::Type() const {
  if (nsCOMPtr<nsILoadInfo> loadInfo = GetLoadInfo()) {
    return GetContentPolicyType(loadInfo->GetExternalContentPolicyType());
  }
  return MozContentPolicyType::Other;
}

void ChannelWrapper::GetMethod(nsCString& aMethod) const {
  if (nsCOMPtr<nsIHttpChannel> chan = MaybeHttpChannel()) {
    Unused << chan->GetRequestMethod(aMethod);
  }
}

/*****************************************************************************
 * ...
 *****************************************************************************/

uint32_t ChannelWrapper::StatusCode() const {
  uint32_t result = 0;
  if (nsCOMPtr<nsIHttpChannel> chan = MaybeHttpChannel()) {
    Unused << chan->GetResponseStatus(&result);
  }
  return result;
}

void ChannelWrapper::GetStatusLine(nsCString& aRetVal) const {
  nsCOMPtr<nsIHttpChannel> chan = MaybeHttpChannel();
  nsCOMPtr<nsIHttpChannelInternal> internal = do_QueryInterface(chan);

  if (internal) {
    nsAutoCString statusText;
    uint32_t major, minor, status;
    if (NS_FAILED(chan->GetResponseStatus(&status)) ||
        NS_FAILED(chan->GetResponseStatusText(statusText)) ||
        NS_FAILED(internal->GetResponseVersion(&major, &minor))) {
      return;
    }

    aRetVal = nsPrintfCString("HTTP/%u.%u %u %s", major, minor, status,
                              statusText.get());
  }
}

uint64_t ChannelWrapper::ResponseSize() const {
  uint64_t result = 0;
  if (nsCOMPtr<nsIHttpChannel> chan = MaybeHttpChannel()) {
    Unused << chan->GetTransferSize(&result);
  }
  return result;
}

uint64_t ChannelWrapper::RequestSize() const {
  uint64_t result = 0;
  if (nsCOMPtr<nsIHttpChannel> chan = MaybeHttpChannel()) {
    Unused << chan->GetRequestSize(&result);
  }
  return result;
}

/*****************************************************************************
 * ...
 *****************************************************************************/

already_AddRefed<nsIURI> ChannelWrapper::FinalURI() const {
  nsCOMPtr<nsIURI> uri;
  if (nsCOMPtr<nsIChannel> chan = MaybeChannel()) {
    NS_GetFinalChannelURI(chan, getter_AddRefs(uri));
  }
  return uri.forget();
}

void ChannelWrapper::GetFinalURL(nsString& aRetVal) const {
  if (HaveChannel()) {
    aRetVal = FinalURLInfo().Spec();
  }
}

/*****************************************************************************
 * ...
 *****************************************************************************/

nsresult FillProxyInfo(MozProxyInfo& aDict, nsIProxyInfo* aProxyInfo) {
  MOZ_TRY(aProxyInfo->GetHost(aDict.mHost));
  MOZ_TRY(aProxyInfo->GetPort(&aDict.mPort));
  MOZ_TRY(aProxyInfo->GetType(aDict.mType));
  MOZ_TRY(aProxyInfo->GetUsername(aDict.mUsername));
  MOZ_TRY(
      aProxyInfo->GetProxyAuthorizationHeader(aDict.mProxyAuthorizationHeader));
  MOZ_TRY(aProxyInfo->GetConnectionIsolationKey(aDict.mConnectionIsolationKey));
  MOZ_TRY(aProxyInfo->GetFailoverTimeout(&aDict.mFailoverTimeout.Construct()));

  uint32_t flags;
  MOZ_TRY(aProxyInfo->GetFlags(&flags));
  aDict.mProxyDNS = flags & nsIProxyInfo::TRANSPARENT_PROXY_RESOLVES_HOST;

  return NS_OK;
}

void ChannelWrapper::GetProxyInfo(dom::Nullable<MozProxyInfo>& aRetVal,
                                  ErrorResult& aRv) const {
  nsCOMPtr<nsIProxyInfo> proxyInfo;
  if (nsCOMPtr<nsIProxiedChannel> proxied = QueryChannel()) {
    Unused << proxied->GetProxyInfo(getter_AddRefs(proxyInfo));
  }
  if (proxyInfo) {
    MozProxyInfo result;

    nsresult rv = FillProxyInfo(result, proxyInfo);
    if (NS_FAILED(rv)) {
      aRv.Throw(rv);
    } else {
      aRetVal.SetValue(std::move(result));
    }
  }
}

void ChannelWrapper::GetRemoteAddress(nsCString& aRetVal) const {
  aRetVal.SetIsVoid(true);
  if (nsCOMPtr<nsIHttpChannelInternal> internal = QueryChannel()) {
    Unused << internal->GetRemoteAddress(aRetVal);
  }
}

void FillClassification(
    Sequence<mozilla::dom::MozUrlClassificationFlags>& classifications,
    uint32_t classificationFlags, ErrorResult& aRv) {
  if (classificationFlags == 0) {
    return;
  }
  for (const auto& entry : classificationArray) {
    if (classificationFlags & entry.mFlag) {
      if (!classifications.AppendElement(entry.mValue, mozilla::fallible)) {
        aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
        return;
      }
    }
  }
}

void ChannelWrapper::GetUrlClassification(
    dom::Nullable<dom::MozUrlClassification>& aRetVal, ErrorResult& aRv) const {
  MozUrlClassification classification;
  if (nsCOMPtr<nsIHttpChannel> chan = MaybeHttpChannel()) {
    nsCOMPtr<nsIClassifiedChannel> classified = do_QueryInterface(chan);
    MOZ_DIAGNOSTIC_ASSERT(
        classified,
        "Must be an object inheriting from both nsIHttpChannel and "
        "nsIClassifiedChannel");
    uint32_t classificationFlags;
    classified->GetFirstPartyClassificationFlags(&classificationFlags);
    FillClassification(classification.mFirstParty, classificationFlags, aRv);
    if (aRv.Failed()) {
      return;
    }
    classified->GetThirdPartyClassificationFlags(&classificationFlags);
    FillClassification(classification.mThirdParty, classificationFlags, aRv);
  }
  aRetVal.SetValue(std::move(classification));
}

bool ChannelWrapper::ThirdParty() const {
  nsCOMPtr<mozIThirdPartyUtil> thirdPartyUtil =
      components::ThirdPartyUtil::Service();
  if (NS_WARN_IF(!thirdPartyUtil)) {
    return true;
  }

  nsCOMPtr<nsIHttpChannel> chan = MaybeHttpChannel();
  if (!chan) {
    return false;
  }

  bool thirdParty = false;
  nsresult rv = thirdPartyUtil->IsThirdPartyChannel(chan, nullptr, &thirdParty);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return true;
  }

  return thirdParty;
}

/*****************************************************************************
 * Error handling
 *****************************************************************************/

void ChannelWrapper::GetErrorString(nsString& aRetVal) const {
  if (nsCOMPtr<nsIChannel> chan = MaybeChannel()) {
    nsCOMPtr<nsITransportSecurityInfo> securityInfo;
    Unused << chan->GetSecurityInfo(getter_AddRefs(securityInfo));
    if (securityInfo) {
      int32_t errorCode = 0;
      securityInfo->GetErrorCode(&errorCode);
      if (psm::IsNSSErrorCode(errorCode)) {
        nsCOMPtr<nsINSSErrorsService> nsserr =
            do_GetService(NS_NSS_ERRORS_SERVICE_CONTRACTID);

        nsresult rv = psm::GetXPCOMFromNSSError(errorCode);
        if (nsserr && NS_SUCCEEDED(nsserr->GetErrorMessage(rv, aRetVal))) {
          return;
        }
      }
    }

    nsresult status;
    if (NS_SUCCEEDED(chan->GetStatus(&status)) && NS_FAILED(status)) {
      nsAutoCString name;
      GetErrorName(status, name);
      AppendUTF8toUTF16(name, aRetVal);
    } else {
      aRetVal.SetIsVoid(true);
    }
  } else {
    aRetVal.AssignLiteral("NS_ERROR_UNEXPECTED");
  }
}

void ChannelWrapper::ErrorCheck() {
  if (!mFiredErrorEvent) {
    nsAutoString error;
    GetErrorString(error);
    if (error.Length()) {
      mChannelEntry = nullptr;
      mFiredErrorEvent = true;
      ChannelWrapper_Binding::ClearCachedErrorStringValue(this);
      FireEvent(u"error"_ns);
    }
  }
}

/*****************************************************************************
 * nsIWebRequestListener
 *****************************************************************************/

NS_IMPL_ISUPPORTS(ChannelWrapper::RequestListener, nsIStreamListener,
                  nsIMultiPartChannelListener, nsIRequestObserver,
                  nsIThreadRetargetableStreamListener)

ChannelWrapper::RequestListener::~RequestListener() {
  NS_ReleaseOnMainThread("RequestListener::mChannelWrapper",
                         mChannelWrapper.forget());
}

nsresult ChannelWrapper::RequestListener::Init() {
  if (nsCOMPtr<nsITraceableChannel> chan = mChannelWrapper->QueryChannel()) {
    return chan->SetNewListener(this, false,
                                getter_AddRefs(mOrigStreamListener));
  }
  return NS_ERROR_UNEXPECTED;
}

NS_IMETHODIMP
ChannelWrapper::RequestListener::OnStartRequest(nsIRequest* request) {
  MOZ_ASSERT(mOrigStreamListener, "Should have mOrigStreamListener");

  mChannelWrapper->mChannelEntry = nullptr;
  mChannelWrapper->mResponseStarted = true;
  mChannelWrapper->ErrorCheck();
  mChannelWrapper->FireEvent(u"start"_ns);

  return mOrigStreamListener->OnStartRequest(request);
}

NS_IMETHODIMP
ChannelWrapper::RequestListener::OnStopRequest(nsIRequest* request,
                                               nsresult aStatus) {
  MOZ_ASSERT(mOrigStreamListener, "Should have mOrigStreamListener");

  mChannelWrapper->mChannelEntry = nullptr;
  mChannelWrapper->ErrorCheck();
  mChannelWrapper->FireEvent(u"stop"_ns);

  return mOrigStreamListener->OnStopRequest(request, aStatus);
}

NS_IMETHODIMP
ChannelWrapper::RequestListener::OnDataAvailable(nsIRequest* request,
                                                 nsIInputStream* inStr,
                                                 uint64_t sourceOffset,
                                                 uint32_t count) {
  MOZ_ASSERT(mOrigStreamListener, "Should have mOrigStreamListener");
  return mOrigStreamListener->OnDataAvailable(request, inStr, sourceOffset,
                                              count);
}

NS_IMETHODIMP
ChannelWrapper::RequestListener::OnAfterLastPart(nsresult aStatus) {
  MOZ_ASSERT(mOrigStreamListener, "Should have mOrigStreamListener");
  if (nsCOMPtr<nsIMultiPartChannelListener> listener =
          do_QueryInterface(mOrigStreamListener)) {
    return listener->OnAfterLastPart(aStatus);
  }
  return NS_OK;
}

NS_IMETHODIMP
ChannelWrapper::RequestListener::CheckListenerChain() {
  MOZ_ASSERT(NS_IsMainThread(), "Should be on main thread!");
  nsresult rv;
  nsCOMPtr<nsIThreadRetargetableStreamListener> retargetableListener =
      do_QueryInterface(mOrigStreamListener, &rv);
  if (retargetableListener) {
    return retargetableListener->CheckListenerChain();
  }
  return rv;
}

/*****************************************************************************
 * Event dispatching
 *****************************************************************************/

void ChannelWrapper::FireEvent(const nsAString& aType) {
  EventInit init;
  init.mBubbles = false;
  init.mCancelable = false;

  RefPtr<Event> event = Event::Constructor(this, aType, init);
  event->SetTrusted(true);

  DispatchEvent(*event);
}

void ChannelWrapper::CheckEventListeners() {
  if (!mAddedStreamListener &&
      (HasListenersFor(nsGkAtoms::onerror) ||
       HasListenersFor(nsGkAtoms::onstart) ||
       HasListenersFor(nsGkAtoms::onstop) || mChannelEntry)) {
    auto listener = MakeRefPtr<RequestListener>(this);
    if (!NS_WARN_IF(NS_FAILED(listener->Init()))) {
      mAddedStreamListener = true;
    }
  }
}

void ChannelWrapper::EventListenerAdded(nsAtom* aType) {
  CheckEventListeners();
}

void ChannelWrapper::EventListenerRemoved(nsAtom* aType) {
  CheckEventListeners();
}

/*****************************************************************************
 * Glue
 *****************************************************************************/

JSObject* ChannelWrapper::WrapObject(JSContext* aCx,
                                     JS::Handle<JSObject*> aGivenProto) {
  return ChannelWrapper_Binding::Wrap(aCx, this, aGivenProto);
}

NS_IMPL_CYCLE_COLLECTION_CLASS(ChannelWrapper)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(ChannelWrapper)
  NS_INTERFACE_MAP_ENTRY_CONCRETE(ChannelWrapper)
NS_INTERFACE_MAP_END_INHERITING(DOMEventTargetHelper)

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN_INHERITED(ChannelWrapper,
                                                DOMEventTargetHelper)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mParent)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mStub)
  NS_IMPL_CYCLE_COLLECTION_UNLINK_WEAK_PTR
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(ChannelWrapper,
                                                  DOMEventTargetHelper)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mParent)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mStub)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_ADDREF_INHERITED(ChannelWrapper, DOMEventTargetHelper)
NS_IMPL_RELEASE_INHERITED(ChannelWrapper, DOMEventTargetHelper)

}  // namespace extensions
}  // namespace mozilla
