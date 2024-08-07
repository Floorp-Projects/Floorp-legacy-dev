/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// HttpLog.h should generally be included first
#include "HttpLog.h"

#include "SpeculativeTransaction.h"
#include "HTTPSRecordResolver.h"
#include "nsICachingChannel.h"
#include "nsHttpHandler.h"

namespace mozilla {
namespace net {

SpeculativeTransaction::SpeculativeTransaction(
    nsHttpConnectionInfo* aConnInfo, nsIInterfaceRequestor* aCallbacks,
    uint32_t aCaps, std::function<void(bool)>&& aCallback)
    : NullHttpTransaction(aConnInfo, aCallbacks, aCaps),
      mCloseCallback(std::move(aCallback)) {}

already_AddRefed<SpeculativeTransaction>
SpeculativeTransaction::CreateWithNewConnInfo(nsHttpConnectionInfo* aConnInfo) {
  RefPtr<SpeculativeTransaction> trans =
      new SpeculativeTransaction(aConnInfo, mCallbacks, mCaps);
  trans->mParallelSpeculativeConnectLimit = mParallelSpeculativeConnectLimit;
  trans->mIgnoreIdle = mIgnoreIdle;
  trans->mIsFromPredictor = mIsFromPredictor;
  trans->mAllow1918 = mAllow1918;
  return trans.forget();
}

nsresult SpeculativeTransaction::FetchHTTPSRR() {
  LOG(("SpeculativeTransaction::FetchHTTPSRR [this=%p]", this));
  MOZ_ASSERT(OnSocketThread(), "not on socket thread");

  RefPtr<HTTPSRecordResolver> resolver = new HTTPSRecordResolver(this);
  nsCOMPtr<nsICancelable> dnsRequest;
  return resolver->FetchHTTPSRRInternal(GetCurrentSerialEventTarget(),
                                        getter_AddRefs(dnsRequest));
}

nsresult SpeculativeTransaction::OnHTTPSRRAvailable(
    nsIDNSHTTPSSVCRecord* aHTTPSSVCRecord,
    nsISVCBRecord* aHighestPriorityRecord) {
  MOZ_ASSERT(OnSocketThread(), "not on socket thread");
  LOG(("SpeculativeTransaction::OnHTTPSRRAvailable [this=%p]", this));

  if (!aHTTPSSVCRecord || !aHighestPriorityRecord) {
    gHttpHandler->ConnMgr()->DoSpeculativeConnection(this, false);
    return NS_OK;
  }

  RefPtr<nsHttpConnectionInfo> connInfo = ConnectionInfo();
  RefPtr<nsHttpConnectionInfo> newInfo =
      connInfo->CloneAndAdoptHTTPSSVCRecord(aHighestPriorityRecord);
  RefPtr<SpeculativeTransaction> newTrans = CreateWithNewConnInfo(newInfo);
  gHttpHandler->ConnMgr()->DoSpeculativeConnection(newTrans, false);
  return NS_OK;
}

nsresult SpeculativeTransaction::ReadSegments(nsAHttpSegmentReader* aReader,
                                              uint32_t aCount,
                                              uint32_t* aCountRead) {
  MOZ_ASSERT(OnSocketThread(), "not on socket thread");
  mTriedToWrite = true;
  return NullHttpTransaction::ReadSegments(aReader, aCount, aCountRead);
}

void SpeculativeTransaction::Close(nsresult aReason) {
  MOZ_ASSERT(OnSocketThread(), "not on socket thread");
  LOG(("SpeculativeTransaction::Close %p aReason=%" PRIx32, this,
       static_cast<uint32_t>(aReason)));
  NullHttpTransaction::Close(aReason);

  if (aReason == NS_BASE_STREAM_CLOSED) {
    aReason = NS_OK;
  }
  if (mCloseCallback) {
    mCloseCallback(mTriedToWrite && NS_SUCCEEDED(aReason));
    mCloseCallback = nullptr;
  }
}

void SpeculativeTransaction::InvokeCallback() {
  if (mCloseCallback) {
    mCloseCallback(true);
    mCloseCallback = nullptr;
  }
}

}  // namespace net
}  // namespace mozilla
