/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ISurfaceAllocator.h"

#include "mozilla/layers/ImageBridgeParent.h"  // for ImageBridgeParent
#include "mozilla/layers/TextureHost.h"        // for TextureHost
#include "mozilla/layers/TextureForwarder.h"
#include "mozilla/layers/CompositableForwarder.h"

namespace mozilla {
namespace layers {

NS_IMPL_ISUPPORTS(GfxMemoryImageReporter, nsIMemoryReporter)

mozilla::Atomic<ptrdiff_t> GfxMemoryImageReporter::sAmount(0);

void HostIPCAllocator::SendPendingAsyncMessages() {
  if (mPendingAsyncMessage.empty()) {
    return;
  }

  // Some type of AsyncParentMessageData message could have
  // one file descriptor (e.g. OpDeliverFence).
  // A number of file descriptors per gecko ipc message have a limitation
  // on OS_POSIX (MACOSX or LINUX).
  static const uint32_t kMaxMessageNumber =
      IPC::Message::MAX_DESCRIPTORS_PER_MESSAGE;

  nsTArray<AsyncParentMessageData> messages;
  messages.SetCapacity(mPendingAsyncMessage.size());
  for (size_t i = 0; i < mPendingAsyncMessage.size(); i++) {
    messages.AppendElement(mPendingAsyncMessage[i]);
    // Limit maximum number of messages.
    if (messages.Length() >= kMaxMessageNumber) {
      SendAsyncMessage(messages);
      // Initialize Messages.
      messages.Clear();
    }
  }

  if (messages.Length() > 0) {
    SendAsyncMessage(messages);
  }
  mPendingAsyncMessage.clear();
}

// XXX - We should actually figure out the minimum shmem allocation size on
// a certain platform and use that.
const uint32_t sShmemPageSize = 4096;

#ifdef DEBUG
const uint32_t sSupportedBlockSize = 4;
#endif

FixedSizeSmallShmemSectionAllocator::FixedSizeSmallShmemSectionAllocator(
    LayersIPCChannel* aShmProvider)
    : mShmProvider(aShmProvider) {
  MOZ_ASSERT(mShmProvider);
}

FixedSizeSmallShmemSectionAllocator::~FixedSizeSmallShmemSectionAllocator() {
  ShrinkShmemSectionHeap();
}

bool FixedSizeSmallShmemSectionAllocator::IPCOpen() const {
  return mShmProvider->IPCOpen();
}

bool FixedSizeSmallShmemSectionAllocator::AllocShmemSection(
    uint32_t aSize, ShmemSection* aShmemSection) {
  // For now we only support sizes of 4. If we want to support different sizes
  // some more complicated bookkeeping should be added.
  MOZ_ASSERT(aSize == sSupportedBlockSize);
  MOZ_ASSERT(aShmemSection);

  if (!IPCOpen()) {
    gfxCriticalError() << "Attempt to allocate a ShmemSection after shutdown.";
    return false;
  }

  uint32_t allocationSize = (aSize + sizeof(ShmemSectionHeapAllocation));

  ipc::Shmem shmem;

  for (size_t i = 0; i < mUsedShmems.size(); i++) {
    ShmemSectionHeapHeader* header =
        mUsedShmems[i].get<ShmemSectionHeapHeader>();
    if ((header->mAllocatedBlocks + 1) * allocationSize +
            sizeof(ShmemSectionHeapHeader) <
        sShmemPageSize) {
      shmem = mUsedShmems[i];
      MOZ_ASSERT(mUsedShmems[i].IsWritable());
      break;
    }
  }

  if (!shmem.IsWritable()) {
    ipc::Shmem tmp;
    if (!mShmProvider->AllocUnsafeShmem(sShmemPageSize, &tmp)) {
      return false;
    }

    ShmemSectionHeapHeader* header = tmp.get<ShmemSectionHeapHeader>();
    header->mTotalBlocks = 0;
    header->mAllocatedBlocks = 0;

    mUsedShmems.push_back(tmp);
    shmem = tmp;
  }

  MOZ_ASSERT(shmem.IsWritable());

  ShmemSectionHeapHeader* header = shmem.get<ShmemSectionHeapHeader>();
  uint8_t* heap = shmem.get<uint8_t>() + sizeof(ShmemSectionHeapHeader);

  ShmemSectionHeapAllocation* allocHeader = nullptr;

  if (header->mTotalBlocks > header->mAllocatedBlocks) {
    // Search for the first available block.
    for (size_t i = 0; i < header->mTotalBlocks; i++) {
      allocHeader = reinterpret_cast<ShmemSectionHeapAllocation*>(heap);

      if (allocHeader->mStatus == STATUS_FREED) {
        break;
      }
      heap += allocationSize;
    }
    MOZ_ASSERT(allocHeader && allocHeader->mStatus == STATUS_FREED);
    MOZ_ASSERT(allocHeader->mSize == sSupportedBlockSize);
  } else {
    heap += header->mTotalBlocks * allocationSize;

    header->mTotalBlocks++;
    allocHeader = reinterpret_cast<ShmemSectionHeapAllocation*>(heap);
    allocHeader->mSize = aSize;
  }

  MOZ_ASSERT(allocHeader);
  header->mAllocatedBlocks++;
  allocHeader->mStatus = STATUS_ALLOCATED;

  size_t offset =
      (heap + sizeof(ShmemSectionHeapAllocation)) - shmem.get<uint8_t>();
  if (offset > (size_t)std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  if (!aShmemSection->Init(shmem, offset, aSize)) {
    return false;
  }

  ShrinkShmemSectionHeap();
  return true;
}

void FixedSizeSmallShmemSectionAllocator::FreeShmemSection(
    mozilla::layers::ShmemSection& aShmemSection) {
  MOZ_ASSERT(aShmemSection.size() == sSupportedBlockSize);
  MOZ_ASSERT(aShmemSection.offset() < sShmemPageSize - sSupportedBlockSize);

  if (!aShmemSection.shmem().IsWritable()) {
    return;
  }

  ShmemSectionHeapAllocation* allocHeader =
      reinterpret_cast<ShmemSectionHeapAllocation*>(
          aShmemSection.shmem().get<char>() + aShmemSection.offset() -
          sizeof(ShmemSectionHeapAllocation));

  MOZ_ASSERT(allocHeader->mSize == aShmemSection.size());

  DebugOnly<bool> success =
      allocHeader->mStatus.compareExchange(STATUS_ALLOCATED, STATUS_FREED);
  // If this fails something really weird is going on.
  MOZ_ASSERT(success);

  ShmemSectionHeapHeader* header =
      aShmemSection.shmem().get<ShmemSectionHeapHeader>();
  header->mAllocatedBlocks--;
}

void FixedSizeSmallShmemSectionAllocator::DeallocShmemSection(
    mozilla::layers::ShmemSection& aShmemSection) {
  if (!IPCOpen()) {
    gfxCriticalNote << "Attempt to dealloc a ShmemSections after shutdown.";
    return;
  }

  FreeShmemSection(aShmemSection);
  ShrinkShmemSectionHeap();
}

void FixedSizeSmallShmemSectionAllocator::ShrinkShmemSectionHeap() {
  if (!IPCOpen()) {
    mUsedShmems.clear();
    return;
  }

  // The loop will terminate as we either increase i, or decrease size
  // every time through.
  size_t i = 0;
  while (i < mUsedShmems.size()) {
    ShmemSectionHeapHeader* header =
        mUsedShmems[i].get<ShmemSectionHeapHeader>();
    if (header->mAllocatedBlocks == 0) {
      mShmProvider->DeallocShmem(mUsedShmems[i]);
      // We don't particularly care about order, move the last one in the array
      // to this position.
      if (i < mUsedShmems.size() - 1) {
        mUsedShmems[i] = mUsedShmems[mUsedShmems.size() - 1];
      }
      mUsedShmems.pop_back();
    } else {
      i++;
    }
  }
}

Maybe<ShmemSection> ShmemSection::FromUntrusted(
    const UntrustedShmemSection& aUntrusted) {
  ShmemSection section;
  if (!section.Init(aUntrusted.shmem(), aUntrusted.offset(),
                    aUntrusted.size())) {
    return Nothing();
  }

  return Some(section);
}

bool ShmemSection::Init(const mozilla::ipc::Shmem& aShmem, uint32_t aOffset,
                        uint32_t aSize) {
  if (!aShmem.IsReadable()) {
    return false;
  }

  size_t shmSize = aShmem.Size<uint8_t>();
  CheckedInt<size_t> end = CheckedInt<size_t>(aOffset) + aSize;

  if (!end.isValid() || end.value() > shmSize) {
    return false;
  }

  mShmem = aShmem;
  mOffset = aOffset;
  mSize = aSize;

  return true;
}

UntrustedShmemSection ShmemSection::AsUntrusted() {
  return UntrustedShmemSection(mShmem, mOffset, mSize);
}

}  // namespace layers
}  // namespace mozilla
