/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "TRRLoadInfo.h"
#include "mozilla/dom/ClientSource.h"
#include "mozilla/dom/FeaturePolicy.h"
#include "mozilla/dom/DOMTypes.h"
#include "nsContentUtils.h"
#include "nsIRedirectHistoryEntry.h"

using namespace mozilla::dom;

namespace mozilla {
namespace net {

NS_IMPL_ISUPPORTS(TRRLoadInfo, nsILoadInfo)

TRRLoadInfo::TRRLoadInfo(nsIURI* aResultPrincipalURI,
                         nsContentPolicyType aContentPolicyType)
    : mResultPrincipalURI(aResultPrincipalURI),
      mInternalContentPolicyType(aContentPolicyType) {}

already_AddRefed<nsILoadInfo> TRRLoadInfo::Clone() const {
  nsCOMPtr<nsILoadInfo> loadInfo =
      new TRRLoadInfo(mResultPrincipalURI, mInternalContentPolicyType);

  return loadInfo.forget();
}

NS_IMETHODIMP
TRRLoadInfo::GetLoadingPrincipal(nsIPrincipal** aLoadingPrincipal) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

nsIPrincipal* TRRLoadInfo::VirtualGetLoadingPrincipal() { return nullptr; }

NS_IMETHODIMP
TRRLoadInfo::GetTriggeringPrincipal(nsIPrincipal** aTriggeringPrincipal) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

nsIPrincipal* TRRLoadInfo::TriggeringPrincipal() { return nullptr; }

NS_IMETHODIMP
TRRLoadInfo::GetPrincipalToInherit(nsIPrincipal** aPrincipalToInherit) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetPrincipalToInherit(nsIPrincipal* aPrincipalToInherit) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

nsIPrincipal* TRRLoadInfo::PrincipalToInherit() { return nullptr; }

nsIPrincipal* TRRLoadInfo::FindPrincipalToInherit(nsIChannel* aChannel) {
  return nullptr;
}

const nsID& TRRLoadInfo::GetSandboxedNullPrincipalID() {
  return mSandboxedNullPrincipalID;
}

void TRRLoadInfo::ResetSandboxedNullPrincipalID() {}

nsIPrincipal* TRRLoadInfo::GetTopLevelPrincipal() { return nullptr; }

NS_IMETHODIMP
TRRLoadInfo::GetTriggeringRemoteType(nsACString& aTriggeringRemoteType) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetTriggeringRemoteType(const nsACString& aTriggeringRemoteType) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetLoadingDocument(Document** aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

nsINode* TRRLoadInfo::LoadingNode() { return nullptr; }

already_AddRefed<nsISupports> TRRLoadInfo::ContextForTopLevelLoad() {
  return nullptr;
}

already_AddRefed<nsISupports> TRRLoadInfo::GetLoadingContext() {
  return nullptr;
}

NS_IMETHODIMP
TRRLoadInfo::GetLoadingContextXPCOM(nsISupports** aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetSecurityFlags(nsSecurityFlags* aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetSandboxFlags(uint32_t* aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}
NS_IMETHODIMP
TRRLoadInfo::GetTriggeringSandboxFlags(uint32_t* aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}
NS_IMETHODIMP
TRRLoadInfo::SetTriggeringSandboxFlags(uint32_t aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetSecurityMode(uint32_t* aFlags) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetIsInThirdPartyContext(bool* aIsInThirdPartyContext) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetIsInThirdPartyContext(bool aIsInThirdPartyContext) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetIsThirdPartyContextToTopWindow(
    bool* aIsThirdPartyContextToTopWindow) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetIsThirdPartyContextToTopWindow(
    bool aIsThirdPartyContextToTopWindow) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetCookiePolicy(uint32_t* aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetCookieJarSettings(nsICookieJarSettings** aCookieJarSettings) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetCookieJarSettings(nsICookieJarSettings* aCookieJarSettings) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetStoragePermission(
    nsILoadInfo::StoragePermissionState* aHasStoragePermission) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetStoragePermission(
    nsILoadInfo::StoragePermissionState aHasStoragePermission) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetIsMetaRefresh(bool* aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetIsMetaRefresh(bool aResult) { return NS_ERROR_NOT_IMPLEMENTED; }

NS_IMETHODIMP
TRRLoadInfo::GetForceInheritPrincipal(bool* aInheritPrincipal) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetForceInheritPrincipalOverruleOwner(bool* aInheritPrincipal) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetLoadingSandboxed(bool* aLoadingSandboxed) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetAboutBlankInherits(bool* aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetAllowChrome(bool* aResult) { return NS_ERROR_NOT_IMPLEMENTED; }

NS_IMETHODIMP
TRRLoadInfo::GetDisallowScript(bool* aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetDontFollowRedirects(bool* aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetLoadErrorPage(bool* aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetIsFormSubmission(bool* aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetIsFormSubmission(bool aValue) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetSendCSPViolationEvents(bool* aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetSendCSPViolationEvents(bool aValue) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetExternalContentPolicyType(nsContentPolicyType* aResult) {
  // We have to use nsContentPolicyType because ExtContentPolicyType is not
  // visible from xpidl.
  *aResult = static_cast<nsContentPolicyType>(
      nsContentUtils::InternalContentPolicyTypeToExternal(
          mInternalContentPolicyType));
  return NS_OK;
}

nsContentPolicyType TRRLoadInfo::InternalContentPolicyType() {
  return mInternalContentPolicyType;
}

NS_IMETHODIMP
TRRLoadInfo::GetBlockAllMixedContent(bool* aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetUpgradeInsecureRequests(bool* aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetBrowserUpgradeInsecureRequests(bool* aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetBrowserDidUpgradeInsecureRequests(bool* aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetBrowserDidUpgradeInsecureRequests(
    bool aBrowserDidUpgradeInsecureRequests) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetBrowserWouldUpgradeInsecureRequests(bool* aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetForceAllowDataURI(bool aForceAllowDataURI) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetForceAllowDataURI(bool* aForceAllowDataURI) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetAllowInsecureRedirectToDataURI(
    bool aAllowInsecureRedirectToDataURI) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetAllowInsecureRedirectToDataURI(
    bool* aAllowInsecureRedirectToDataURI) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetSkipContentPolicyCheckForWebRequest(bool aSkip) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetSkipContentPolicyCheckForWebRequest(bool* aSkip) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetOriginalFrameSrcLoad(bool aOriginalFrameSrcLoad) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetOriginalFrameSrcLoad(bool* aOriginalFrameSrcLoad) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetForceInheritPrincipalDropped(bool* aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetInnerWindowID(uint64_t* aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetBrowsingContextID(uint64_t* aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetWorkerAssociatedBrowsingContextID(uint64_t* aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetWorkerAssociatedBrowsingContextID(uint64_t aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetFrameBrowsingContextID(uint64_t* aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetTargetBrowsingContextID(uint64_t* aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetBrowsingContext(dom::BrowsingContext** aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetWorkerAssociatedBrowsingContext(
    dom::BrowsingContext** aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetFrameBrowsingContext(dom::BrowsingContext** aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetTargetBrowsingContext(dom::BrowsingContext** aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetScriptableOriginAttributes(
    JSContext* aCx, JS::MutableHandle<JS::Value> aOriginAttributes) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::ResetPrincipalToInheritToNullPrincipal() {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetScriptableOriginAttributes(
    JSContext* aCx, JS::Handle<JS::Value> aOriginAttributes) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

nsresult TRRLoadInfo::GetOriginAttributes(
    mozilla::OriginAttributes* aOriginAttributes) {
  NS_ENSURE_ARG(aOriginAttributes);
  *aOriginAttributes = mOriginAttributes;
  return NS_OK;
}

nsresult TRRLoadInfo::SetOriginAttributes(
    const mozilla::OriginAttributes& aOriginAttributes) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetInitialSecurityCheckDone(bool aInitialSecurityCheckDone) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetInitialSecurityCheckDone(bool* aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::AppendRedirectHistoryEntry(nsIChannel* aChannelToDeriveFrom,
                                        bool aIsInternalRedirect) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetRedirectChainIncludingInternalRedirects(
    JSContext* aCx, JS::MutableHandle<JS::Value> aChain) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

const nsTArray<nsCOMPtr<nsIRedirectHistoryEntry>>&
TRRLoadInfo::RedirectChainIncludingInternalRedirects() {
  return mEmptyRedirectChain;
}

NS_IMETHODIMP
TRRLoadInfo::GetRedirectChain(JSContext* aCx,
                              JS::MutableHandle<JS::Value> aChain) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

const nsTArray<nsCOMPtr<nsIRedirectHistoryEntry>>&
TRRLoadInfo::RedirectChain() {
  return mEmptyRedirectChain;
}

const nsTArray<nsCOMPtr<nsIPrincipal>>& TRRLoadInfo::AncestorPrincipals() {
  return mEmptyPrincipals;
}

const nsTArray<uint64_t>& TRRLoadInfo::AncestorBrowsingContextIDs() {
  return mEmptyBrowsingContextIDs;
}

void TRRLoadInfo::SetCorsPreflightInfo(const nsTArray<nsCString>& aHeaders,
                                       bool aForcePreflight) {}

const nsTArray<nsCString>& TRRLoadInfo::CorsUnsafeHeaders() {
  return mCorsUnsafeHeaders;
}

NS_IMETHODIMP
TRRLoadInfo::GetForcePreflight(bool* aForcePreflight) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetIsPreflight(bool* aIsPreflight) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetLoadTriggeredFromExternal(bool aLoadTriggeredFromExternal) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetLoadTriggeredFromExternal(bool* aLoadTriggeredFromExternal) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetServiceWorkerTaintingSynthesized(
    bool* aServiceWorkerTaintingSynthesized) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetTainting(uint32_t* aTaintingOut) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::MaybeIncreaseTainting(uint32_t aTainting) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

void TRRLoadInfo::SynthesizeServiceWorkerTainting(LoadTainting aTainting) {}

NS_IMETHODIMP
TRRLoadInfo::GetDocumentHasUserInteracted(bool* aDocumentHasUserInteracted) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetDocumentHasUserInteracted(bool aDocumentHasUserInteracted) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetAllowListFutureDocumentsCreatedFromThisRedirectChain(
    bool* aValue) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetAllowListFutureDocumentsCreatedFromThisRedirectChain(
    bool aValue) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetNeedForCheckingAntiTrackingHeuristic(bool* aValue) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetNeedForCheckingAntiTrackingHeuristic(bool aValue) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetCspNonce(nsAString& aCspNonce) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetCspNonce(const nsAString& aCspNonce) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetSkipContentSniffing(bool* aSkipContentSniffing) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetSkipContentSniffing(bool aSkipContentSniffing) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetIsTopLevelLoad(bool* aResult) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetIsFromProcessingFrameAttributes(
    bool* aIsFromProcessingFrameAttributes) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetIsMediaRequest(bool aIsMediaRequest) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetIsMediaRequest(bool* aIsMediaRequest) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetIsMediaInitialRequest(bool aIsMediaInitialRequest) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetIsMediaInitialRequest(bool* aIsMediaInitialRequest) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetIsFromObjectOrEmbed(bool aIsFromObjectOrEmbed) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetIsFromObjectOrEmbed(bool* aIsFromObjectOrEmbed) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetShouldSkipCheckForBrokenURLOrZeroSized(
    bool* aShouldSkipCheckForBrokenURLOrZeroSized) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetResultPrincipalURI(nsIURI** aURI) {
  nsCOMPtr<nsIURI> uri = mResultPrincipalURI;
  uri.forget(aURI);
  return NS_OK;
}

NS_IMETHODIMP
TRRLoadInfo::SetResultPrincipalURI(nsIURI* aURI) {
  mResultPrincipalURI = aURI;
  return NS_OK;
}

NS_IMETHODIMP
TRRLoadInfo::GetChannelCreationOriginalURI(nsIURI** aURI) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetChannelCreationOriginalURI(nsIURI* aURI) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetRequestBlockingReason(uint32_t aReason) {
  return NS_ERROR_NOT_IMPLEMENTED;
}
NS_IMETHODIMP
TRRLoadInfo::GetRequestBlockingReason(uint32_t* aReason) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

void TRRLoadInfo::SetClientInfo(const ClientInfo& aClientInfo) {}

const Maybe<ClientInfo>& TRRLoadInfo::GetClientInfo() { return mClientInfo; }

void TRRLoadInfo::GiveReservedClientSource(
    UniquePtr<ClientSource>&& aClientSource) {}

UniquePtr<ClientSource> TRRLoadInfo::TakeReservedClientSource() {
  return nullptr;
}

void TRRLoadInfo::SetReservedClientInfo(const ClientInfo& aClientInfo) {}

void TRRLoadInfo::OverrideReservedClientInfoInParent(
    const ClientInfo& aClientInfo) {}

const Maybe<ClientInfo>& TRRLoadInfo::GetReservedClientInfo() {
  return mReservedClientInfo;
}

void TRRLoadInfo::SetInitialClientInfo(const ClientInfo& aClientInfo) {}

const Maybe<ClientInfo>& TRRLoadInfo::GetInitialClientInfo() {
  return mInitialClientInfo;
}

void TRRLoadInfo::SetController(const ServiceWorkerDescriptor& aServiceWorker) {
}

void TRRLoadInfo::ClearController() {}

const Maybe<ServiceWorkerDescriptor>& TRRLoadInfo::GetController() {
  return mController;
}

void TRRLoadInfo::SetPerformanceStorage(
    PerformanceStorage* aPerformanceStorage) {}

PerformanceStorage* TRRLoadInfo::GetPerformanceStorage() { return nullptr; }

NS_IMETHODIMP
TRRLoadInfo::GetCspEventListener(nsICSPEventListener** aCSPEventListener) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetCspEventListener(nsICSPEventListener* aCSPEventListener) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

already_AddRefed<nsIContentSecurityPolicy> TRRLoadInfo::GetCsp() {
  return nullptr;
}

already_AddRefed<nsIContentSecurityPolicy> TRRLoadInfo::GetPreloadCsp() {
  return nullptr;
}

already_AddRefed<nsIContentSecurityPolicy> TRRLoadInfo::GetCspToInherit() {
  return nullptr;
}

Maybe<FeaturePolicyInfo> TRRLoadInfo::GetContainerFeaturePolicyInfo() {
  return Nothing();
}

void TRRLoadInfo::SetContainerFeaturePolicyInfo(
    const FeaturePolicyInfo& aContainerFeaturePolicyInfo) {}

NS_IMETHODIMP
TRRLoadInfo::GetHttpsOnlyStatus(uint32_t* aHttpsOnlyStatus) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetHttpsOnlyStatus(uint32_t aHttpsOnlyStatus) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetHstsStatus(bool* aHstsStatus) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetHstsStatus(bool aHstsStatus) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetHasValidUserGestureActivation(
    bool* aHasValidUserGestureActivation) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetHasValidUserGestureActivation(
    bool aHasValidUserGestureActivation) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetInternalContentPolicyType(nsContentPolicyType* aResult) {
  *aResult = mInternalContentPolicyType;
  return NS_OK;
}

NS_IMETHODIMP
TRRLoadInfo::GetAllowDeprecatedSystemRequests(
    bool* aAllowDeprecatedSystemRequests) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetAllowDeprecatedSystemRequests(
    bool aAllowDeprecatedSystemRequests) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetIsUserTriggeredSave(bool* aIsUserTriggeredSave) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetIsUserTriggeredSave(bool aIsUserTriggeredSave) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetIsInDevToolsContext(bool* aIsInDevToolsContext) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetIsInDevToolsContext(bool aIsInDevToolsContext) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetParserCreatedScript(bool* aParserCreatedScript) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetParserCreatedScript(bool aParserCreatedScript) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetLoadingEmbedderPolicy(
    nsILoadInfo::CrossOriginEmbedderPolicy* aOutPolicy) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetLoadingEmbedderPolicy(
    nsILoadInfo::CrossOriginEmbedderPolicy aPolicy) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetIsOriginTrialCoepCredentiallessEnabledForTopLevel(
    bool* aIsOriginTrialCoepCredentiallessEnabledForTopLevel) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetIsOriginTrialCoepCredentiallessEnabledForTopLevel(
    bool aIsOriginTrialCoepCredentiallessEnabledForTopLevel) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::GetUnstrippedURI(nsIURI** aURI) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetUnstrippedURI(nsIURI* aURI) { return NS_ERROR_NOT_IMPLEMENTED; }

nsIInterceptionInfo* TRRLoadInfo::InterceptionInfo() { return nullptr; }
void TRRLoadInfo::SetInterceptionInfo(nsIInterceptionInfo* aPrincipla) {}

NS_IMETHODIMP
TRRLoadInfo::GetHasInjectedCookieForCookieBannerHandling(
    bool* aHasInjectedCookieForCookieBannerHandling) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
TRRLoadInfo::SetHasInjectedCookieForCookieBannerHandling(
    bool aHasInjectedCookieForCookieBannerHandling) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

}  // namespace net
}  // namespace mozilla
