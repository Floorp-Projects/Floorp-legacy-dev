/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "chrome/common/ipc_channel.h"
#include "mozilla/a11y/DocAccessibleChildBase.h"
#include "mozilla/a11y/CacheConstants.h"
#include "mozilla/a11y/RemoteAccessible.h"
#include "mozilla/ipc/ProcessChild.h"

#include "LocalAccessible-inl.h"
#ifdef A11Y_LOG
#  include "Logging.h"
#endif
#include "TextLeafRange.h"

namespace mozilla {
namespace a11y {

/* static */
void DocAccessibleChildBase::FlattenTree(LocalAccessible* aRoot,
                                         nsTArray<LocalAccessible*>& aTree) {
  MOZ_ASSERT(!aRoot->IsDoc(), "documents shouldn't be serialized");

  aTree.AppendElement(aRoot);
  // OuterDocAccessibles are special because we don't want to serialize the
  // child doc here, we'll call PDocAccessibleConstructor in
  // NotificationController.
  uint32_t childCount = aRoot->IsOuterDoc() ? 0 : aRoot->ChildCount();

  for (uint32_t i = 0; i < childCount; i++) {
    FlattenTree(aRoot->LocalChildAt(i), aTree);
  }
}

/* static */
AccessibleData DocAccessibleChildBase::SerializeAcc(LocalAccessible* aAcc) {
  uint32_t genericTypes = aAcc->mGenericTypes;
  if (aAcc->ARIAHasNumericValue()) {
    // XXX: We need to do this because this requires a state check.
    genericTypes |= eNumericValue;
  }
  if (aAcc->IsTextLeaf() || aAcc->IsImage()) {
    // Ideally, we'd set eActionable for any Accessible with an ancedstor
    // action. However, that requires an ancestor walk which is too expensive
    // here. eActionable is only used by ATK. For now, we only expose ancestor
    // actions on text leaf and image Accessibles. This means that we don't
    // support "clickAncestor" for ATK.
    if (aAcc->ActionCount()) {
      genericTypes |= eActionable;
    }
  } else if (aAcc->HasPrimaryAction()) {
    genericTypes |= eActionable;
  }

  RefPtr<AccAttributes> fields;
  // Even though we send moves as a hide and a show, we don't want to
  // push the cache again for moves.
  if (!aAcc->Document()->IsAccessibleBeingMoved(aAcc)) {
    fields =
        aAcc->BundleFieldsForCache(CacheDomain::All, CacheUpdateType::Initial);
    if (fields->Count() == 0) {
      fields = nullptr;
    }
  }

  return AccessibleData(aAcc->ID(), aAcc->Role(), aAcc->LocalParent()->ID(),
                        static_cast<int32_t>(aAcc->IndexInParent()),
                        static_cast<AccType>(aAcc->mType),
                        static_cast<AccGenericType>(genericTypes),
                        aAcc->mRoleMapEntryIndex, fields);
}

void DocAccessibleChildBase::InsertIntoIpcTree(LocalAccessible* aChild,
                                               bool aSuppressShowEvent) {
  nsTArray<LocalAccessible*> shownTree;
  FlattenTree(aChild, shownTree);
  uint32_t totalAccs = shownTree.Length();
  // Exceeding the IPDL maximum message size will cause a crash. Try to avoid
  // this by only including kMaxAccsPerMessage Accessibels in a single IPDL
  // call. If there are Accessibles beyond this, they will be split across
  // multiple calls.
  constexpr uint32_t kMaxAccsPerMessage =
      IPC::Channel::kMaximumMessageSize / (2 * 1024);
  nsTArray<AccessibleData> data(std::min(kMaxAccsPerMessage, totalAccs));
  for (LocalAccessible* child : shownTree) {
    if (data.Length() == kMaxAccsPerMessage) {
      if (ipc::ProcessChild::ExpectingShutdown()) {
        return;
      }
      SendShowEvent(data, aSuppressShowEvent, false, false);
      data.ClearAndRetainStorage();
    }
    data.AppendElement(SerializeAcc(child));
  }
  if (ipc::ProcessChild::ExpectingShutdown()) {
    return;
  }
  if (!data.IsEmpty()) {
    SendShowEvent(data, aSuppressShowEvent, true, false);
  }
}

void DocAccessibleChildBase::ShowEvent(AccShowEvent* aShowEvent) {
  LocalAccessible* child = aShowEvent->GetAccessible();
  InsertIntoIpcTree(child, false);
}

mozilla::ipc::IPCResult DocAccessibleChildBase::RecvTakeFocus(
    const uint64_t& aID) {
  LocalAccessible* acc = IdToAccessible(aID);
  if (acc) {
    acc->TakeFocus();
  }
  return IPC_OK();
}

mozilla::ipc::IPCResult DocAccessibleChildBase::RecvScrollTo(
    const uint64_t& aID, const uint32_t& aScrollType) {
  LocalAccessible* acc = IdToAccessible(aID);
  if (acc) {
    RefPtr<PresShell> presShell = acc->Document()->PresShellPtr();
    nsCOMPtr<nsIContent> content = acc->GetContent();
    nsCoreUtils::ScrollTo(presShell, content, aScrollType);
  }

  return IPC_OK();
}

mozilla::ipc::IPCResult DocAccessibleChildBase::RecvTakeSelection(
    const uint64_t& aID) {
  LocalAccessible* acc = IdToAccessible(aID);
  if (acc) {
    acc->TakeSelection();
  }

  return IPC_OK();
}

mozilla::ipc::IPCResult DocAccessibleChildBase::RecvSetSelected(
    const uint64_t& aID, const bool& aSelect) {
  LocalAccessible* acc = IdToAccessible(aID);
  if (acc) {
    acc->SetSelected(aSelect);
  }

  return IPC_OK();
}

mozilla::ipc::IPCResult DocAccessibleChildBase::RecvVerifyCache(
    const uint64_t& aID, const uint64_t& aCacheDomain, AccAttributes* aFields) {
#ifdef A11Y_LOG
  LocalAccessible* acc = IdToAccessible(aID);
  if (!acc) {
    return IPC_OK();
  }

  RefPtr<AccAttributes> localFields =
      acc->BundleFieldsForCache(aCacheDomain, CacheUpdateType::Update);
  bool mismatches = false;

  for (auto prop : *localFields) {
    if (prop.Value<DeleteEntry>()) {
      if (aFields->HasAttribute(prop.Name())) {
        if (!mismatches) {
          logging::MsgBegin("Mismatch!", "Local and remote values differ");
          logging::AccessibleInfo("", acc);
          mismatches = true;
        }
        nsAutoCString propName;
        prop.Name()->ToUTF8String(propName);
        nsAutoString val;
        aFields->GetAttribute(prop.Name(), val);
        logging::MsgEntry(
            "Remote value for %s should be empty, but instead it is '%s'",
            propName.get(), NS_ConvertUTF16toUTF8(val).get());
      }
      continue;
    }

    nsAutoString localVal;
    prop.ValueAsString(localVal);
    nsAutoString remoteVal;
    aFields->GetAttribute(prop.Name(), remoteVal);
    if (!localVal.Equals(remoteVal)) {
      if (!mismatches) {
        logging::MsgBegin("Mismatch!", "Local and remote values differ");
        logging::AccessibleInfo("", acc);
        mismatches = true;
      }
      nsAutoCString propName;
      prop.Name()->ToUTF8String(propName);
      logging::MsgEntry("Fields differ: %s '%s' != '%s'", propName.get(),
                        NS_ConvertUTF16toUTF8(remoteVal).get(),
                        NS_ConvertUTF16toUTF8(localVal).get());
    }
  }
  if (mismatches) {
    logging::MsgEnd();
  }
#endif  // A11Y_LOG

  return IPC_OK();
}

mozilla::ipc::IPCResult DocAccessibleChildBase::RecvDoActionAsync(
    const uint64_t& aID, const uint8_t& aIndex) {
  if (LocalAccessible* acc = IdToAccessible(aID)) {
    Unused << acc->DoAction(aIndex);
  }

  return IPC_OK();
}

mozilla::ipc::IPCResult DocAccessibleChildBase::RecvSetCaretOffset(
    const uint64_t& aID, const int32_t& aOffset) {
  HyperTextAccessible* acc = IdToHyperTextAccessible(aID);
  if (acc && acc->IsTextRole() && acc->IsValidOffset(aOffset)) {
    acc->SetCaretOffset(aOffset);
  }
  return IPC_OK();
}

mozilla::ipc::IPCResult DocAccessibleChildBase::RecvSetTextSelection(
    const uint64_t& aStartID, const int32_t& aStartOffset,
    const uint64_t& aEndID, const int32_t& aEndOffset,
    const int32_t& aSelectionNum) {
  TextLeafRange range(TextLeafPoint(IdToAccessible(aStartID), aStartOffset),
                      TextLeafPoint(IdToAccessible(aEndID), aEndOffset));
  if (range) {
    range.SetSelection(aSelectionNum);
  }

  return IPC_OK();
}

mozilla::ipc::IPCResult DocAccessibleChildBase::RecvScrollTextLeafRangeIntoView(
    const uint64_t& aStartID, const int32_t& aStartOffset,
    const uint64_t& aEndID, const int32_t& aEndOffset,
    const uint32_t& aScrollType) {
  TextLeafRange range(TextLeafPoint(IdToAccessible(aStartID), aStartOffset),
                      TextLeafPoint(IdToAccessible(aEndID), aEndOffset));
  if (range) {
    range.ScrollIntoView(aScrollType);
  }

  return IPC_OK();
}

mozilla::ipc::IPCResult DocAccessibleChildBase::RecvRemoveTextSelection(
    const uint64_t& aID, const int32_t& aSelectionNum) {
  HyperTextAccessible* acc = IdToHyperTextAccessible(aID);
  if (acc && acc->IsTextRole()) {
    acc->RemoveFromSelection(aSelectionNum);
  }

  return IPC_OK();
}

mozilla::ipc::IPCResult DocAccessibleChildBase::RecvSetCurValue(
    const uint64_t& aID, const double& aValue) {
  LocalAccessible* acc = IdToAccessible(aID);
  if (acc) {
    acc->SetCurValue(aValue);
  }

  return IPC_OK();
}

mozilla::ipc::IPCResult DocAccessibleChildBase::RecvReplaceText(
    const uint64_t& aID, const nsAString& aText) {
  HyperTextAccessible* acc = IdToHyperTextAccessible(aID);
  if (acc && acc->IsTextRole()) {
    acc->ReplaceText(aText);
  }

  return IPC_OK();
}

mozilla::ipc::IPCResult DocAccessibleChildBase::RecvInsertText(
    const uint64_t& aID, const nsAString& aText, const int32_t& aPosition) {
  HyperTextAccessible* acc = IdToHyperTextAccessible(aID);
  if (acc && acc->IsTextRole()) {
    acc->InsertText(aText, aPosition);
  }

  return IPC_OK();
}

mozilla::ipc::IPCResult DocAccessibleChildBase::RecvCopyText(
    const uint64_t& aID, const int32_t& aStartPos, const int32_t& aEndPos) {
  HyperTextAccessible* acc = IdToHyperTextAccessible(aID);
  if (acc && acc->IsTextRole()) {
    acc->CopyText(aStartPos, aEndPos);
  }

  return IPC_OK();
}

mozilla::ipc::IPCResult DocAccessibleChildBase::RecvCutText(
    const uint64_t& aID, const int32_t& aStartPos, const int32_t& aEndPos) {
  HyperTextAccessible* acc = IdToHyperTextAccessible(aID);
  if (acc && acc->IsTextRole()) {
    acc->CutText(aStartPos, aEndPos);
  }

  return IPC_OK();
}

mozilla::ipc::IPCResult DocAccessibleChildBase::RecvDeleteText(
    const uint64_t& aID, const int32_t& aStartPos, const int32_t& aEndPos) {
  HyperTextAccessible* acc = IdToHyperTextAccessible(aID);
  if (acc && acc->IsTextRole()) {
    acc->DeleteText(aStartPos, aEndPos);
  }

  return IPC_OK();
}

mozilla::ipc::IPCResult DocAccessibleChildBase::RecvPasteText(
    const uint64_t& aID, const int32_t& aPosition) {
  RefPtr<HyperTextAccessible> acc = IdToHyperTextAccessible(aID);
  if (acc && acc->IsTextRole()) {
    acc->PasteText(aPosition);
  }

  return IPC_OK();
}

LocalAccessible* DocAccessibleChildBase::IdToAccessible(
    const uint64_t& aID) const {
  if (!aID) return mDoc;

  if (!mDoc) return nullptr;

  return mDoc->GetAccessibleByUniqueID(reinterpret_cast<void*>(aID));
}

HyperTextAccessible* DocAccessibleChildBase::IdToHyperTextAccessible(
    const uint64_t& aID) const {
  LocalAccessible* acc = IdToAccessible(aID);
  return acc && acc->IsHyperText() ? acc->AsHyperText() : nullptr;
}

}  // namespace a11y
}  // namespace mozilla
