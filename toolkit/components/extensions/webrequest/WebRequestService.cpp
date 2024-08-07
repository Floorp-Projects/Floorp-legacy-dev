/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WebRequestService.h"

#include "mozilla/Assertions.h"
#include "mozilla/ClearOnShutdown.h"

using namespace mozilla;
using namespace mozilla::dom;
using namespace mozilla::extensions;

static StaticRefPtr<WebRequestService> sWebRequestService;

/* static */ WebRequestService& WebRequestService::GetSingleton() {
  if (!sWebRequestService) {
    sWebRequestService = new WebRequestService();
    ClearOnShutdown(&sWebRequestService);
  }
  return *sWebRequestService;
}

UniquePtr<WebRequestChannelEntry> WebRequestService::RegisterChannel(
    ChannelWrapper* aChannel) {
  UniquePtr<ChannelEntry> entry(new ChannelEntry(aChannel));

  mChannelEntries.WithEntryHandle(entry->mChannelId, [&](auto&& key) {
    MOZ_DIAGNOSTIC_ASSERT(!key);
    key.Insert(entry.get());
  });

  return entry;
}

already_AddRefed<nsITraceableChannel> WebRequestService::GetTraceableChannel(
    uint64_t aChannelId, const WebExtensionPolicy& aAddon,
    ContentParent* aContentParent) {
  if (auto entry = mChannelEntries.Get(aChannelId)) {
    if (entry->mChannel) {
      return entry->mChannel->GetTraceableChannel(aAddon, aContentParent);
    }
  }
  return nullptr;
}

WebRequestChannelEntry::WebRequestChannelEntry(ChannelWrapper* aChannel)
    : mChannelId(aChannel->Id()), mChannel(aChannel) {}

WebRequestChannelEntry::~WebRequestChannelEntry() {
  if (sWebRequestService) {
    sWebRequestService->mChannelEntries.Remove(mChannelId);
  }
}
