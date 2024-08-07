/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2; -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_extensions_ChannelWrapper_h
#define mozilla_extensions_ChannelWrapper_h

#include "mozilla/dom/BindingDeclarations.h"
#include "mozilla/dom/ChannelWrapperBinding.h"

#include "mozilla/WebRequestService.h"
#include "mozilla/extensions/MatchPattern.h"
#include "mozilla/extensions/WebExtensionPolicy.h"

#include "mozilla/Attributes.h"
#include "mozilla/LinkedList.h"
#include "mozilla/Maybe.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/WeakPtr.h"

#include "mozilla/DOMEventTargetHelper.h"
#include "nsCOMPtr.h"
#include "nsCycleCollectionParticipant.h"
#include "nsIChannel.h"
#include "nsIHttpChannel.h"
#include "nsIMultiPartChannel.h"
#include "nsIStreamListener.h"
#include "nsIRemoteTab.h"
#include "nsIThreadRetargetableStreamListener.h"
#include "nsPointerHashKeys.h"
#include "nsInterfaceHashtable.h"
#include "nsIWeakReferenceUtils.h"
#include "nsWrapperCache.h"

#define NS_CHANNELWRAPPER_IID                        \
  {                                                  \
    0xc06162d2, 0xb803, 0x43b4, {                    \
      0xaa, 0x31, 0xcf, 0x69, 0x7f, 0x93, 0x68, 0x1c \
    }                                                \
  }

class nsILoadContext;
class nsITraceableChannel;

namespace mozilla {
namespace dom {
class ContentParent;
class Element;
}  // namespace dom
namespace extensions {

namespace detail {

// We need to store our wrapped channel as a weak reference, since channels
// are not cycle collected, and we're going to be hanging this wrapper
// instance off the channel in order to ensure the same channel always has
// the same wrapper.
//
// But since performance matters here, and we don't want to have to
// QueryInterface the channel every time we touch it, we store separate
// nsIChannel and nsIHttpChannel weak references, and check that the WeakPtr
// is alive before returning it.
//
// This holder class prevents us from accidentally touching the weak pointer
// members directly from our ChannelWrapper class.
struct ChannelHolder {
  explicit ChannelHolder(nsIChannel* aChannel)
      : mChannel(do_GetWeakReference(aChannel)), mWeakChannel(aChannel) {}

  bool HaveChannel() const { return mChannel && mChannel->IsAlive(); }

  void SetChannel(nsIChannel* aChannel) {
    mChannel = do_GetWeakReference(aChannel);
    mWeakChannel = aChannel;
    mWeakHttpChannel.reset();
  }

  already_AddRefed<nsIChannel> MaybeChannel() const {
    if (!HaveChannel()) {
      mWeakChannel = nullptr;
    }
    return do_AddRef(mWeakChannel);
  }

  already_AddRefed<nsIHttpChannel> MaybeHttpChannel() const {
    if (mWeakHttpChannel.isNothing()) {
      nsCOMPtr<nsIHttpChannel> chan = QueryChannel();
      mWeakHttpChannel.emplace(chan.get());
    }

    if (!HaveChannel()) {
      mWeakHttpChannel.ref() = nullptr;
    }
    return do_AddRef(mWeakHttpChannel.value());
  }

  const nsQueryReferent QueryChannel() const {
    return do_QueryReferent(mChannel);
  }

 private:
  nsWeakPtr mChannel;

  mutable nsIChannel* MOZ_NON_OWNING_REF mWeakChannel;
  mutable Maybe<nsIHttpChannel*> MOZ_NON_OWNING_REF mWeakHttpChannel;
};
}  // namespace detail

class WebRequestChannelEntry;

class ChannelWrapper final : public DOMEventTargetHelper,
                             public SupportsWeakPtr,
                             public LinkedListElement<ChannelWrapper>,
                             private detail::ChannelHolder {
 public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(ChannelWrapper, DOMEventTargetHelper)

  NS_DECLARE_STATIC_IID_ACCESSOR(NS_CHANNELWRAPPER_IID)

  void Die();

  static already_AddRefed<extensions::ChannelWrapper> Get(
      const dom::GlobalObject& global, nsIChannel* channel);
  static already_AddRefed<extensions::ChannelWrapper> GetRegisteredChannel(
      const dom::GlobalObject& global, uint64_t aChannelId,
      const WebExtensionPolicy& aAddon, nsIRemoteTab* aBrowserParent);

  uint64_t Id() const { return mId; }

  already_AddRefed<nsIChannel> GetChannel() const { return MaybeChannel(); }

  void SetChannel(nsIChannel* aChannel);

  void Cancel(uint32_t result, uint32_t reason, ErrorResult& aRv);

  void RedirectTo(nsIURI* uri, ErrorResult& aRv);
  void UpgradeToSecure(ErrorResult& aRv);

  bool Suspended() const { return mSuspended; }
  void Suspend(const nsCString& aProfileMarkerText, ErrorResult& aRv);
  void Resume(ErrorResult& aRv);

  void GetContentType(nsCString& aContentType) const;
  void SetContentType(const nsACString& aContentType);

  void RegisterTraceableChannel(const WebExtensionPolicy& aAddon,
                                nsIRemoteTab* aBrowserParent);

  already_AddRefed<nsITraceableChannel> GetTraceableChannel(
      const WebExtensionPolicy& aAddon,
      dom::ContentParent* aContentParent) const;

  void GetMethod(nsCString& aRetVal) const;

  dom::MozContentPolicyType Type() const;

  uint32_t StatusCode() const;

  uint64_t ResponseSize() const;

  uint64_t RequestSize() const;

  void GetStatusLine(nsCString& aRetVal) const;

  void GetErrorString(nsString& aRetVal) const;

  void ErrorCheck();

  IMPL_EVENT_HANDLER(error);
  IMPL_EVENT_HANDLER(start);
  IMPL_EVENT_HANDLER(stop);

  already_AddRefed<nsIURI> FinalURI() const;

  void GetFinalURL(nsString& aRetVal) const;

  bool Matches(const dom::MozRequestFilter& aFilter,
               const WebExtensionPolicy* aExtension,
               const dom::MozRequestMatchOptions& aOptions) const;

  already_AddRefed<nsILoadInfo> GetLoadInfo() const {
    nsCOMPtr<nsIChannel> chan = MaybeChannel();
    if (chan) {
      return chan->LoadInfo();
    }
    return nullptr;
  }

  int64_t FrameId() const;

  int64_t ParentFrameId() const;

  void GetFrameAncestors(
      dom::Nullable<nsTArray<dom::MozFrameAncestorInfo>>& aFrameAncestors,
      ErrorResult& aRv) const;

  bool IsServiceWorkerScript() const;

  static bool IsServiceWorkerScript(const nsCOMPtr<nsIChannel>& aChannel);

  bool IsSystemLoad() const;

  void GetOriginURL(nsCString& aRetVal) const;

  void GetDocumentURL(nsCString& aRetVal) const;

  already_AddRefed<nsIURI> GetOriginURI() const;

  already_AddRefed<nsIURI> GetDocumentURI() const;

  already_AddRefed<nsILoadContext> GetLoadContext() const;

  already_AddRefed<dom::Element> GetBrowserElement() const;

  bool CanModify() const;
  bool GetCanModify(ErrorResult& aRv) const { return CanModify(); }

  void GetProxyInfo(dom::Nullable<dom::MozProxyInfo>& aRetVal,
                    ErrorResult& aRv) const;

  void GetRemoteAddress(nsCString& aRetVal) const;

  void GetRequestHeaders(nsTArray<dom::MozHTTPHeader>& aRetVal,
                         ErrorResult& aRv) const;
  void GetRequestHeader(const nsCString& aHeader, nsCString& aResult,
                        ErrorResult& aRv) const;

  void GetResponseHeaders(nsTArray<dom::MozHTTPHeader>& aRetVal,
                          ErrorResult& aRv) const;

  void SetRequestHeader(const nsCString& header, const nsCString& value,
                        bool merge, ErrorResult& aRv);

  void SetResponseHeader(const nsCString& header, const nsCString& value,
                         bool merge, ErrorResult& aRv);

  void GetUrlClassification(dom::Nullable<dom::MozUrlClassification>& aRetVal,
                            ErrorResult& aRv) const;

  bool ThirdParty() const;

  using EventTarget::EventListenerAdded;
  using EventTarget::EventListenerRemoved;
  virtual void EventListenerAdded(nsAtom* aType) override;
  virtual void EventListenerRemoved(nsAtom* aType) override;

  nsISupports* GetParentObject() const { return mParent; }

  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;

 protected:
  ~ChannelWrapper();

 private:
  ChannelWrapper(nsISupports* aParent, nsIChannel* aChannel);

  void ClearCachedAttributes();

  bool CheckAlive(ErrorResult& aRv) const {
    if (!HaveChannel()) {
      aRv.Throw(NS_ERROR_UNEXPECTED);
      return false;
    }
    return true;
  }

  void FireEvent(const nsAString& aType);

  const URLInfo& FinalURLInfo() const;
  const URLInfo* DocumentURLInfo() const;

  uint64_t BrowsingContextId(nsILoadInfo* aLoadInfo) const;

  nsresult GetFrameAncestors(
      nsILoadInfo* aLoadInfo,
      nsTArray<dom::MozFrameAncestorInfo>& aFrameAncestors) const;

  static uint64_t GetNextId() {
    static uint64_t sNextId = 1;
    return ++sNextId;
  }

  void CheckEventListeners();

  class ChannelWrapperStub final : public nsISupports {
   public:
    NS_DECL_CYCLE_COLLECTING_ISUPPORTS
    NS_DECL_CYCLE_COLLECTION_CLASS(ChannelWrapperStub)

    explicit ChannelWrapperStub(ChannelWrapper* aChannelWrapper)
        : mChannelWrapper(aChannelWrapper) {}

   private:
    friend class ChannelWrapper;

    RefPtr<ChannelWrapper> mChannelWrapper;

   protected:
    ~ChannelWrapperStub() = default;
  };

  RefPtr<ChannelWrapperStub> mStub;

  mutable Maybe<URLInfo> mFinalURLInfo;
  mutable Maybe<URLInfo> mDocumentURLInfo;

  UniquePtr<WebRequestChannelEntry> mChannelEntry;

  // The overridden Content-Type header value.
  nsCString mContentTypeHdr = VoidCString();

  const uint64_t mId = GetNextId();
  nsCOMPtr<nsISupports> mParent;

  bool mAddedStreamListener = false;
  bool mFiredErrorEvent = false;
  bool mSuspended = false;
  bool mResponseStarted = false;

  nsInterfaceHashtable<nsPtrHashKey<const nsAtom>, nsIRemoteTab> mAddonEntries;

  // The text for the "Extension Suspend" marker, set from the Suspend method
  // when called for the first time and then cleared on the Resume method.
  nsCString mSuspendedMarkerText = VoidCString();

  class RequestListener final : public nsIStreamListener,
                                public nsIMultiPartChannelListener,
                                public nsIThreadRetargetableStreamListener {
   public:
    NS_DECL_THREADSAFE_ISUPPORTS
    NS_DECL_NSIREQUESTOBSERVER
    NS_DECL_NSISTREAMLISTENER
    NS_DECL_NSIMULTIPARTCHANNELLISTENER
    NS_DECL_NSITHREADRETARGETABLESTREAMLISTENER

    explicit RequestListener(ChannelWrapper* aWrapper)
        : mChannelWrapper(aWrapper) {}

    nsresult Init();

   protected:
    virtual ~RequestListener();

   private:
    RefPtr<ChannelWrapper> mChannelWrapper;
    nsCOMPtr<nsIStreamListener> mOrigStreamListener;
  };
};

NS_DEFINE_STATIC_IID_ACCESSOR(ChannelWrapper, NS_CHANNELWRAPPER_IID)

}  // namespace extensions
}  // namespace mozilla

#endif  // mozilla_extensions_ChannelWrapper_h
