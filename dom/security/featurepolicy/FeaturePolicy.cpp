/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FeaturePolicy.h"
#include "mozilla/BasePrincipal.h"
#include "mozilla/dom/BrowsingContext.h"
#include "mozilla/dom/Feature.h"
#include "mozilla/dom/FeaturePolicyBinding.h"
#include "mozilla/dom/FeaturePolicyParser.h"
#include "mozilla/dom/FeaturePolicyUtils.h"
#include "mozilla/dom/HTMLIFrameElement.h"
#include "mozilla/StaticPrefs_dom.h"
#include "nsContentUtils.h"
#include "nsNetUtil.h"

namespace mozilla::dom {

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(FeaturePolicy)
NS_IMPL_CYCLE_COLLECTING_ADDREF(FeaturePolicy)
NS_IMPL_CYCLE_COLLECTING_RELEASE(FeaturePolicy)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(FeaturePolicy)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

FeaturePolicy::FeaturePolicy(nsINode* aNode) : mParentNode(aNode) {}

void FeaturePolicy::InheritPolicy(FeaturePolicy* aParentPolicy) {
  MOZ_ASSERT(aParentPolicy);

  mInheritedDeniedFeatureNames.Clear();

  RefPtr<FeaturePolicy> dest = this;
  RefPtr<FeaturePolicy> src = aParentPolicy;

  // Inherit origins which explicitly declared policy in chain
  for (const Feature& featureInChain :
       aParentPolicy->mDeclaredFeaturesInAncestorChain) {
    dest->AppendToDeclaredAllowInAncestorChain(featureInChain);
  }

  FeaturePolicyUtils::ForEachFeature([dest, src](const char* aFeatureName) {
    nsString featureName;
    featureName.AppendASCII(aFeatureName);
    // Store unsafe allows all (allow=*)
    if (src->HasFeatureUnsafeAllowsAll(featureName)) {
      dest->mParentAllowedAllFeatures.AppendElement(featureName);
    }

    // If the destination has a declared feature (via the HTTP header or 'allow'
    // attribute) we allow the feature if the destination allows it and the
    // parent allows its origin or the destinations' one.
    if (dest->HasDeclaredFeature(featureName) &&
        dest->AllowsFeatureInternal(featureName, dest->mDefaultOrigin)) {
      if (!src->AllowsFeatureInternal(featureName, src->mDefaultOrigin) &&
          !src->AllowsFeatureInternal(featureName, dest->mDefaultOrigin)) {
        dest->SetInheritedDeniedFeature(featureName);
      }
      return;
    }

    // If there was not a declared feature, we allow the feature if the parent
    // FeaturePolicy allows the current origin.
    if (!src->AllowsFeatureInternal(featureName, dest->mDefaultOrigin)) {
      dest->SetInheritedDeniedFeature(featureName);
    }
  });
}

void FeaturePolicy::InheritPolicy(
    const FeaturePolicyInfo& aContainerFeaturePolicyInfo) {
  // We create a temporary FeaturePolicy from the FeaturePolicyInfo to be able
  // to re-use the inheriting functionality from FeaturePolicy.
  RefPtr<dom::FeaturePolicy> featurePolicy = new dom::FeaturePolicy(nullptr);
  featurePolicy->SetDefaultOrigin(aContainerFeaturePolicyInfo.mDefaultOrigin);
  featurePolicy->SetInheritedDeniedFeatureNames(
      aContainerFeaturePolicyInfo.mInheritedDeniedFeatureNames);

  const auto& declaredString = aContainerFeaturePolicyInfo.mDeclaredString;
  if (aContainerFeaturePolicyInfo.mSelfOrigin && !declaredString.IsEmpty()) {
    featurePolicy->SetDeclaredPolicy(nullptr, declaredString,
                                     aContainerFeaturePolicyInfo.mSelfOrigin,
                                     aContainerFeaturePolicyInfo.mSrcOrigin);
  }

  for (const auto& featureName :
       aContainerFeaturePolicyInfo.mAttributeEnabledFeatureNames) {
    featurePolicy->MaybeSetAllowedPolicy(featureName);
  }

  InheritPolicy(featurePolicy);
}

void FeaturePolicy::SetInheritedDeniedFeature(const nsAString& aFeatureName) {
  MOZ_ASSERT(!HasInheritedDeniedFeature(aFeatureName));
  mInheritedDeniedFeatureNames.AppendElement(aFeatureName);
}

bool FeaturePolicy::HasInheritedDeniedFeature(
    const nsAString& aFeatureName) const {
  return mInheritedDeniedFeatureNames.Contains(aFeatureName);
}

bool FeaturePolicy::HasDeclaredFeature(const nsAString& aFeatureName) const {
  for (const Feature& feature : mFeatures) {
    if (feature.Name().Equals(aFeatureName)) {
      return true;
    }
  }

  return false;
}

bool FeaturePolicy::HasFeatureUnsafeAllowsAll(
    const nsAString& aFeatureName) const {
  for (const Feature& feature : mFeatures) {
    if (feature.AllowsAll() && feature.Name().Equals(aFeatureName)) {
      return true;
    }
  }

  // We should look into parent too (for example, document of iframe which
  // allows all, would be unsafe)
  return mParentAllowedAllFeatures.Contains(aFeatureName);
}

void FeaturePolicy::AppendToDeclaredAllowInAncestorChain(
    const Feature& aFeature) {
  for (Feature& featureInChain : mDeclaredFeaturesInAncestorChain) {
    if (featureInChain.Name().Equals(aFeature.Name())) {
      MOZ_ASSERT(featureInChain.HasAllowList());

      nsTArray<nsCOMPtr<nsIPrincipal>> list;
      aFeature.GetAllowList(list);

      for (nsIPrincipal* principal : list) {
        featureInChain.AppendToAllowList(principal);
      }
      continue;
    }
  }

  mDeclaredFeaturesInAncestorChain.AppendElement(aFeature);
}

bool FeaturePolicy::IsSameOriginAsSrc(nsIPrincipal* aPrincipal) const {
  MOZ_ASSERT(aPrincipal);

  if (!mSrcOrigin) {
    return false;
  }

  return BasePrincipal::Cast(mSrcOrigin)
      ->Subsumes(aPrincipal, BasePrincipal::ConsiderDocumentDomain);
}

void FeaturePolicy::SetDeclaredPolicy(Document* aDocument,
                                      const nsAString& aPolicyString,
                                      nsIPrincipal* aSelfOrigin,
                                      nsIPrincipal* aSrcOrigin) {
  ResetDeclaredPolicy();

  mDeclaredString = aPolicyString;
  mSelfOrigin = aSelfOrigin;
  mSrcOrigin = aSrcOrigin;

  Unused << NS_WARN_IF(!FeaturePolicyParser::ParseString(
      aPolicyString, aDocument, aSelfOrigin, aSrcOrigin, mFeatures));

  // Only store explicitly declared allowlist
  for (const Feature& feature : mFeatures) {
    if (feature.HasAllowList()) {
      AppendToDeclaredAllowInAncestorChain(feature);
    }
  }
}

void FeaturePolicy::ResetDeclaredPolicy() {
  mFeatures.Clear();
  mDeclaredString.Truncate();
  mSelfOrigin = nullptr;
  mSrcOrigin = nullptr;
  mDeclaredFeaturesInAncestorChain.Clear();
  mAttributeEnabledFeatureNames.Clear();
}

JSObject* FeaturePolicy::WrapObject(JSContext* aCx,
                                    JS::Handle<JSObject*> aGivenProto) {
  return FeaturePolicy_Binding::Wrap(aCx, this, aGivenProto);
}

bool FeaturePolicy::AllowsFeature(const nsAString& aFeatureName,
                                  const Optional<nsAString>& aOrigin) const {
  nsCOMPtr<nsIPrincipal> origin;
  if (aOrigin.WasPassed()) {
    nsCOMPtr<nsIURI> uri;
    nsresult rv = NS_NewURI(getter_AddRefs(uri), aOrigin.Value());
    if (NS_FAILED(rv)) {
      return false;
    }
    origin = BasePrincipal::CreateContentPrincipal(
        uri, BasePrincipal::Cast(mDefaultOrigin)->OriginAttributesRef());
  } else {
    origin = mDefaultOrigin;
  }

  if (NS_WARN_IF(!origin)) {
    return false;
  }

  return AllowsFeatureInternal(aFeatureName, origin);
}

bool FeaturePolicy::AllowsFeatureExplicitlyInAncestorChain(
    const nsAString& aFeatureName, nsIPrincipal* aOrigin) const {
  MOZ_ASSERT(aOrigin);

  for (const Feature& feature : mDeclaredFeaturesInAncestorChain) {
    if (feature.Name().Equals(aFeatureName)) {
      return feature.AllowListContains(aOrigin);
    }
  }

  return false;
}

bool FeaturePolicy::AllowsFeatureInternal(const nsAString& aFeatureName,
                                          nsIPrincipal* aOrigin) const {
  MOZ_ASSERT(aOrigin);

  // Let's see if have to disable this feature because inherited policy.
  if (HasInheritedDeniedFeature(aFeatureName)) {
    return false;
  }

  for (const Feature& feature : mFeatures) {
    if (feature.Name().Equals(aFeatureName)) {
      return feature.Allows(aOrigin);
    }
  }

  switch (FeaturePolicyUtils::DefaultAllowListFeature(aFeatureName)) {
    case FeaturePolicyUtils::FeaturePolicyValue::eAll:
      return true;

    case FeaturePolicyUtils::FeaturePolicyValue::eSelf:
      return BasePrincipal::Cast(mDefaultOrigin)
          ->Subsumes(aOrigin, BasePrincipal::ConsiderDocumentDomain);

    case FeaturePolicyUtils::FeaturePolicyValue::eNone:
      return false;

    default:
      MOZ_CRASH("Unknown default value");
  }

  return false;
}

void FeaturePolicy::Features(nsTArray<nsString>& aFeatures) {
  RefPtr<FeaturePolicy> self = this;
  FeaturePolicyUtils::ForEachFeature(
      [self, &aFeatures](const char* aFeatureName) {
        nsString featureName;
        featureName.AppendASCII(aFeatureName);
        aFeatures.AppendElement(featureName);
      });
}

void FeaturePolicy::AllowedFeatures(nsTArray<nsString>& aAllowedFeatures) {
  RefPtr<FeaturePolicy> self = this;
  FeaturePolicyUtils::ForEachFeature(
      [self, &aAllowedFeatures](const char* aFeatureName) {
        nsString featureName;
        featureName.AppendASCII(aFeatureName);

        if (self->AllowsFeatureInternal(featureName, self->mDefaultOrigin)) {
          aAllowedFeatures.AppendElement(featureName);
        }
      });
}

void FeaturePolicy::GetAllowlistForFeature(const nsAString& aFeatureName,
                                           nsTArray<nsString>& aList) const {
  if (!AllowsFeatureInternal(aFeatureName, mDefaultOrigin)) {
    return;
  }

  for (const Feature& feature : mFeatures) {
    if (feature.Name().Equals(aFeatureName)) {
      if (feature.AllowsAll()) {
        aList.AppendElement(u"*"_ns);
        return;
      }

      nsTArray<nsCOMPtr<nsIPrincipal>> list;
      feature.GetAllowList(list);

      for (nsIPrincipal* principal : list) {
        nsAutoCString originNoSuffix;
        nsresult rv = principal->GetOriginNoSuffix(originNoSuffix);
        if (NS_WARN_IF(NS_FAILED(rv))) {
          return;
        }

        aList.AppendElement(NS_ConvertUTF8toUTF16(originNoSuffix));
      }
      return;
    }
  }

  switch (FeaturePolicyUtils::DefaultAllowListFeature(aFeatureName)) {
    case FeaturePolicyUtils::FeaturePolicyValue::eAll:
      aList.AppendElement(u"*"_ns);
      return;

    case FeaturePolicyUtils::FeaturePolicyValue::eSelf: {
      nsAutoCString originNoSuffix;
      nsresult rv = mDefaultOrigin->GetOriginNoSuffix(originNoSuffix);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return;
      }

      aList.AppendElement(NS_ConvertUTF8toUTF16(originNoSuffix));
      return;
    }

    case FeaturePolicyUtils::FeaturePolicyValue::eNone:
      return;

    default:
      MOZ_CRASH("Unknown default value");
  }
}

void FeaturePolicy::MaybeSetAllowedPolicy(const nsAString& aFeatureName) {
  MOZ_ASSERT(FeaturePolicyUtils::IsSupportedFeature(aFeatureName) ||
             FeaturePolicyUtils::IsExperimentalFeature(aFeatureName));
  // Skip if feature is in experimental phase
  if (!StaticPrefs::dom_security_featurePolicy_experimental_enabled() &&
      FeaturePolicyUtils::IsExperimentalFeature(aFeatureName)) {
    return;
  }

  if (HasDeclaredFeature(aFeatureName)) {
    return;
  }

  Feature feature(aFeatureName);
  feature.SetAllowsAll();

  mFeatures.AppendElement(feature);
  mAttributeEnabledFeatureNames.AppendElement(aFeatureName);
}

FeaturePolicyInfo FeaturePolicy::ToFeaturePolicyInfo() const {
  return {mInheritedDeniedFeatureNames.Clone(),
          mAttributeEnabledFeatureNames.Clone(),
          mDeclaredString,
          mDefaultOrigin,
          mSelfOrigin,
          mSrcOrigin};
}

}  // namespace mozilla::dom
