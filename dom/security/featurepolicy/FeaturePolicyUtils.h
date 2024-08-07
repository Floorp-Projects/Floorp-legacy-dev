/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_FeaturePolicyUtils_h
#define mozilla_dom_FeaturePolicyUtils_h

#include <functional>

#include "mozilla/dom/FeaturePolicy.h"

class PickleIterator;

namespace IPC {
class Message;
class MessageReader;
class MessageWriter;
}  // namespace IPC

namespace mozilla {
namespace dom {

class Document;

class FeaturePolicyUtils final {
 public:
  enum FeaturePolicyValue {
    // Feature always allowed.
    eAll,

    // Feature allowed for documents that are same-origin with this one.
    eSelf,

    // Feature denied.
    eNone,
  };

  // This method returns true if aFeatureName is allowed for aDocument.
  // Use this method everywhere you need to check feature-policy directives.
  static bool IsFeatureAllowed(Document* aDocument,
                               const nsAString& aFeatureName);

  // Returns true if aFeatureName is a known feature policy name.
  static bool IsSupportedFeature(const nsAString& aFeatureName);

  // Returns true if aFeatureName is a experimental feature policy name.
  static bool IsExperimentalFeature(const nsAString& aFeatureName);

  // Runs aCallback for each known feature policy, with the feature name as
  // argument.
  static void ForEachFeature(const std::function<void(const char*)>& aCallback);

  // Returns the default policy value for aFeatureName.
  static FeaturePolicyValue DefaultAllowListFeature(
      const nsAString& aFeatureName);

  // This method returns true if aFeatureName is in unsafe allowed "*" case.
  // We are in "unsafe" case when there is 'allow "*"' presents for an origin
  // that's not presented in the ancestor feature policy chain, via src, via
  // explicitly listed in allow, and not being the top-level origin.
  static bool IsFeatureUnsafeAllowedAll(Document* aDocument,
                                        const nsAString& aFeatureName);

 private:
  static void ReportViolation(Document* aDocument,
                              const nsAString& aFeatureName);
};

}  // namespace dom

namespace ipc {

class IProtocol;

template <typename T>
struct IPDLParamTraits;

template <>
struct IPDLParamTraits<dom::FeaturePolicyInfo> {
  static void Write(IPC::MessageWriter* aWriter, IProtocol* aActor,
                    const mozilla::dom::FeaturePolicyInfo& aParam);
  static bool Read(IPC::MessageReader* aReader, IProtocol* aActor,
                   dom::FeaturePolicyInfo* aResult);
};

}  // namespace ipc
}  // namespace mozilla

#endif  // mozilla_dom_FeaturePolicyUtils_h
