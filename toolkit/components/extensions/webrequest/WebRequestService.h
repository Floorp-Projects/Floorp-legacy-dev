/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_WebRequestService_h
#define mozilla_WebRequestService_h

#include "mozilla/LinkedList.h"
#include "mozilla/UniquePtr.h"

#include "mozilla/extensions/ChannelWrapper.h"
#include "mozilla/extensions/WebExtensionPolicy.h"

#include "nsHashKeys.h"
#include "nsTHashMap.h"

class nsIRemoteTab;
class nsITraceableChannel;

namespace mozilla {
namespace dom {
class BrowserParent;
class ContentParent;
}  // namespace dom

namespace extensions {

class ChannelWrapper;

class WebRequestChannelEntry final {
 public:
  ~WebRequestChannelEntry();

 private:
  friend class WebRequestService;

  explicit WebRequestChannelEntry(ChannelWrapper* aChannel);

  uint64_t mChannelId;
  WeakPtr<ChannelWrapper> mChannel;
};

class WebRequestService final {
 public:
  NS_INLINE_DECL_REFCOUNTING(WebRequestService)

  WebRequestService() = default;

  static already_AddRefed<WebRequestService> GetInstance() {
    return do_AddRef(&GetSingleton());
  }

  static WebRequestService& GetSingleton();

  using ChannelEntry = WebRequestChannelEntry;

  UniquePtr<ChannelEntry> RegisterChannel(ChannelWrapper* aChannel);

  void UnregisterTraceableChannel(uint64_t aChannelId);

  already_AddRefed<nsITraceableChannel> GetTraceableChannel(
      uint64_t aChannelId, const WebExtensionPolicy& aAddon,
      dom::ContentParent* aContentParent);

 private:
  ~WebRequestService() = default;

  friend ChannelEntry;

  nsTHashMap<nsUint64HashKey, ChannelEntry*> mChannelEntries;
};

}  // namespace extensions
}  // namespace mozilla

#endif  // mozilla_WebRequestService_h
