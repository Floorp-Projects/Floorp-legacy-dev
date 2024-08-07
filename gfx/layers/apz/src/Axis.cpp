/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "Axis.h"

#include <math.h>     // for fabsf, pow, powf
#include <algorithm>  // for max

#include "APZCTreeManager.h"                // for APZCTreeManager
#include "AsyncPanZoomController.h"         // for AsyncPanZoomController
#include "FrameMetrics.h"                   // for FrameMetrics
#include "SimpleVelocityTracker.h"          // for FrameMetrics
#include "mozilla/Attributes.h"             // for final
#include "mozilla/Preferences.h"            // for Preferences
#include "mozilla/gfx/Rect.h"               // for RoundedIn
#include "mozilla/layers/APZThreadUtils.h"  // for AssertOnControllerThread
#include "mozilla/mozalloc.h"               // for operator new
#include "nsMathUtils.h"                    // for NS_lround
#include "nsPrintfCString.h"                // for nsPrintfCString
#include "nsThreadUtils.h"                  // for NS_DispatchToMainThread, etc
#include "nscore.h"                         // for NS_IMETHOD

static mozilla::LazyLogModule sApzAxsLog("apz.axis");
#define AXIS_LOG(...) MOZ_LOG(sApzAxsLog, LogLevel::Debug, (__VA_ARGS__))

namespace mozilla {
namespace layers {

bool FuzzyEqualsCoordinate(CSSCoord aValue1, CSSCoord aValue2) {
  return FuzzyEqualsAdditive(aValue1, aValue2, COORDINATE_EPSILON) ||
         FuzzyEqualsMultiplicative(aValue1, aValue2);
}

Axis::Axis(AsyncPanZoomController* aAsyncPanZoomController)
    : mPos(0),
      mVelocity(0.0f, "Axis::mVelocity"),
      mAxisLocked(false, "Axis::mAxisLocked"),
      mAsyncPanZoomController(aAsyncPanZoomController),
      mOverscroll(0),
      mMSDModel(0.0, 0.0, 0.0, StaticPrefs::apz_overscroll_spring_stiffness(),
                StaticPrefs::apz_overscroll_damping()),
      mVelocityTracker(mAsyncPanZoomController->GetPlatformSpecificState()
                           ->CreateVelocityTracker(this)) {}

float Axis::ToLocalVelocity(float aVelocityInchesPerMs) const {
  ScreenPoint velocity =
      MakePoint(aVelocityInchesPerMs * mAsyncPanZoomController->GetDPI());
  // Use ToScreenCoordinates() to convert a point rather than a vector by
  // treating the point as a vector, and using (0, 0) as the anchor.
  ScreenPoint panStart = mAsyncPanZoomController->ToScreenCoordinates(
      mAsyncPanZoomController->PanStart(), ParentLayerPoint());
  ParentLayerPoint localVelocity =
      mAsyncPanZoomController->ToParentLayerCoordinates(velocity, panStart);
  return localVelocity.Length();
}

void Axis::UpdateWithTouchAtDevicePoint(ParentLayerCoord aPos,
                                        TimeStamp aTimestamp) {
  // mVelocityTracker is controller-thread only
  APZThreadUtils::AssertOnControllerThread();

  mPos = aPos;

  AXIS_LOG("%p|%s got position %f\n", mAsyncPanZoomController, Name(),
           mPos.value);
  if (Maybe<float> newVelocity =
          mVelocityTracker->AddPosition(aPos, aTimestamp)) {
    bool axisLocked = IsAxisLocked();
    DoSetVelocity(axisLocked ? 0 : *newVelocity);
    AXIS_LOG("%p|%s velocity from tracker is %f%s\n", mAsyncPanZoomController,
             Name(), *newVelocity,
             axisLocked ? ", but we are axis locked" : "");
  }
}

void Axis::StartTouch(ParentLayerCoord aPos, TimeStamp aTimestamp) {
  mStartPos = aPos;
  mPos = aPos;
  mVelocityTracker->StartTracking(aPos, aTimestamp);
  SetAxisLocked(false);
}

bool Axis::AdjustDisplacement(ParentLayerCoord aDisplacement,
                              ParentLayerCoord& aDisplacementOut,
                              ParentLayerCoord& aOverscrollAmountOut,
                              bool aForceOverscroll /* = false */) {
  if (IsAxisLocked()) {
    aOverscrollAmountOut = 0;
    aDisplacementOut = 0;
    return false;
  }
  if (aForceOverscroll) {
    aOverscrollAmountOut = aDisplacement;
    aDisplacementOut = 0;
    return false;
  }

  ParentLayerCoord displacement = aDisplacement;

  // First consume any overscroll in the opposite direction along this axis.
  ParentLayerCoord consumedOverscroll = 0;
  if (mOverscroll > 0 && aDisplacement < 0) {
    consumedOverscroll = std::min(mOverscroll, -aDisplacement);
  } else if (mOverscroll < 0 && aDisplacement > 0) {
    consumedOverscroll = 0.f - std::min(-mOverscroll, aDisplacement);
  }
  mOverscroll -= consumedOverscroll;
  displacement += consumedOverscroll;

  if (consumedOverscroll != 0.0f) {
    AXIS_LOG("%p|%s changed overscroll amount to %f\n", mAsyncPanZoomController,
             Name(), mOverscroll.value);
  }

  // Split the requested displacement into an allowed displacement that does
  // not overscroll, and an overscroll amount.
  aOverscrollAmountOut = DisplacementWillOverscrollAmount(displacement);
  if (aOverscrollAmountOut != 0.0f) {
    // No need to have a velocity along this axis anymore; it won't take us
    // anywhere, so we're just spinning needlessly.
    AXIS_LOG("%p|%s has overscrolled, clearing velocity\n",
             mAsyncPanZoomController, Name());
    DoSetVelocity(0.0f);
    displacement -= aOverscrollAmountOut;
  }
  aDisplacementOut = displacement;
  return fabsf(consumedOverscroll) > EPSILON;
}

ParentLayerCoord Axis::ApplyResistance(
    ParentLayerCoord aRequestedOverscroll) const {
  // 'resistanceFactor' is a value between 0 and 1/16, which:
  //   - tends to 1/16 as the existing overscroll tends to 0
  //   - tends to 0 as the existing overscroll tends to the composition length
  // The actual overscroll is the requested overscroll multiplied by this
  // factor.
  float resistanceFactor =
      (1 - fabsf(GetOverscroll()) / GetCompositionLength()) / 16;
  float result = resistanceFactor < 0 ? ParentLayerCoord(0)
                                      : aRequestedOverscroll * resistanceFactor;
  result = clamped(result, -8.0f, 8.0f);
  return result;
}

void Axis::OverscrollBy(ParentLayerCoord aOverscroll) {
  MOZ_ASSERT(CanScroll());
  // We can get some spurious calls to OverscrollBy() with near-zero values
  // due to rounding error. Ignore those (they might trip the asserts below.)
  if (mAsyncPanZoomController->IsZero(aOverscroll)) {
    return;
  }
  EndOverscrollAnimation();
  aOverscroll = ApplyResistance(aOverscroll);
  if (aOverscroll > 0) {
#ifdef DEBUG
    if (!IsScrolledToEnd()) {
      nsPrintfCString message(
          "composition end (%f) is not equal (within error) to page end (%f)\n",
          GetCompositionEnd().value, GetPageEnd().value);
      NS_ASSERTION(false, message.get());
      MOZ_CRASH("GFX: Overscroll issue > 0");
    }
#endif
    MOZ_ASSERT(mOverscroll >= 0);
  } else if (aOverscroll < 0) {
#ifdef DEBUG
    if (!IsScrolledToStart()) {
      nsPrintfCString message(
          "composition origin (%f) is not equal (within error) to page origin "
          "(%f)\n",
          GetOrigin().value, GetPageStart().value);
      NS_ASSERTION(false, message.get());
      MOZ_CRASH("GFX: Overscroll issue < 0");
    }
#endif
    MOZ_ASSERT(mOverscroll <= 0);
  }
  mOverscroll += aOverscroll;

  AXIS_LOG("%p|%s changed overscroll amount to %f\n", mAsyncPanZoomController,
           Name(), mOverscroll.value);
}

ParentLayerCoord Axis::GetOverscroll() const { return mOverscroll; }

void Axis::RestoreOverscroll(ParentLayerCoord aOverscroll) {
  mOverscroll = aOverscroll;
}

void Axis::StartOverscrollAnimation(float aVelocity) {
  const float maxVelocity = StaticPrefs::apz_overscroll_max_velocity();
  aVelocity = clamped(aVelocity / 2.0f, -maxVelocity, maxVelocity);
  SetVelocity(aVelocity);
  mMSDModel.SetPosition(mOverscroll);
  // Convert velocity from ParentLayerCoords/millisecond to
  // ParentLayerCoords/second.
  mMSDModel.SetVelocity(DoGetVelocity() * 1000.0);

  AXIS_LOG(
      "%p|%s beginning overscroll animation with amount %f and velocity %f\n",
      mAsyncPanZoomController, Name(), mOverscroll.value, DoGetVelocity());
}

void Axis::EndOverscrollAnimation() {
  mMSDModel.SetPosition(0.0);
  mMSDModel.SetVelocity(0.0);
}

bool Axis::SampleOverscrollAnimation(const TimeDuration& aDelta,
                                     SideBits aOverscrollSideBits) {
  mMSDModel.Simulate(aDelta);
  mOverscroll = mMSDModel.GetPosition();

  if (((aOverscrollSideBits & (SideBits::eTop | SideBits::eLeft)) &&
       mOverscroll > 0) ||
      ((aOverscrollSideBits & (SideBits::eBottom | SideBits::eRight)) &&
       mOverscroll < 0)) {
    // Stop the overscroll model immediately if it's going to get across the
    // boundary.
    mMSDModel.SetPosition(0.0);
    mMSDModel.SetVelocity(0.0);
  }

  AXIS_LOG("%p|%s changed overscroll amount to %f\n", mAsyncPanZoomController,
           Name(), mOverscroll.value);

  if (mMSDModel.IsFinished(1.0)) {
    // "Jump" to the at-rest state. The jump shouldn't be noticeable as the
    // velocity and overscroll are already low.
    AXIS_LOG("%p|%s oscillation dropped below threshold, going to rest\n",
             mAsyncPanZoomController, Name());
    ClearOverscroll();
    DoSetVelocity(0);
    return false;
  }

  // Otherwise, continue the animation.
  return true;
}

bool Axis::IsOverscrollAnimationRunning() const {
  return !mMSDModel.IsFinished(1.0);
}

bool Axis::IsOverscrollAnimationAlive() const {
  // Unlike IsOverscrollAnimationRunning, check the position and the velocity to
  // be sure that the animation has started but hasn't yet finished.
  return mMSDModel.GetPosition() != 0.0 || mMSDModel.GetVelocity() != 0.0;
}

bool Axis::IsOverscrolled() const { return mOverscroll != 0.f; }

bool Axis::IsScrolledToStart() const {
  const auto zoom = GetFrameMetrics().GetZoom();

  if (zoom == CSSToParentLayerScale(0)) {
    return true;
  }

  return FuzzyEqualsCoordinate(GetOrigin() / zoom, GetPageStart() / zoom);
}

bool Axis::IsScrolledToEnd() const {
  const auto zoom = GetFrameMetrics().GetZoom();

  if (zoom == CSSToParentLayerScale(0)) {
    return true;
  }

  return FuzzyEqualsCoordinate(GetCompositionEnd() / zoom, GetPageEnd() / zoom);
}

bool Axis::IsInInvalidOverscroll() const {
  if (mOverscroll > 0) {
    return !IsScrolledToEnd();
  } else if (mOverscroll < 0) {
    return !IsScrolledToStart();
  }
  return false;
}

void Axis::ClearOverscroll() {
  EndOverscrollAnimation();
  mOverscroll = 0;
}

ParentLayerCoord Axis::PanStart() const { return mStartPos; }

ParentLayerCoord Axis::PanDistance() const { return fabs(mPos - mStartPos); }

ParentLayerCoord Axis::PanDistance(ParentLayerCoord aPos) const {
  return fabs(aPos - mStartPos);
}

void Axis::EndTouch(TimeStamp aTimestamp, ClearAxisLock aClearAxisLock) {
  // mVelocityQueue is controller-thread only
  APZThreadUtils::AssertOnControllerThread();

  // If the velocity tracker wasn't able to compute a velocity, zero out
  // the velocity to make sure we don't get a fling based on some old and
  // no-longer-relevant value of mVelocity. Also if the axis is locked then
  // just reset the velocity to 0 since we don't need any velocity to carry
  // into the fling.
  auto axisLocked = mAxisLocked.Lock();
  if (axisLocked.ref()) {
    DoSetVelocity(0);
  } else if (Maybe<float> velocity =
                 mVelocityTracker->ComputeVelocity(aTimestamp)) {
    DoSetVelocity(*velocity);
  } else {
    DoSetVelocity(0);
  }
  if (aClearAxisLock == ClearAxisLock::Yes) {
    axisLocked.ref() = false;
  }
  AXIS_LOG("%p|%s ending touch, computed velocity %f\n",
           mAsyncPanZoomController, Name(), DoGetVelocity());
}

void Axis::CancelGesture() {
  // mVelocityQueue is controller-thread only
  APZThreadUtils::AssertOnControllerThread();

  AXIS_LOG("%p|%s cancelling touch, clearing velocity queue\n",
           mAsyncPanZoomController, Name());
  DoSetVelocity(0.0f);
  mVelocityTracker->Clear();
  SetAxisLocked(false);
}

bool Axis::CanScroll() const {
  return mAsyncPanZoomController->FuzzyGreater(GetPageLength(),
                                               GetCompositionLength());
}

bool Axis::CanScroll(CSSCoord aDelta) const {
  return CanScroll(aDelta * GetFrameMetrics().GetZoom());
}

bool Axis::CanScroll(ParentLayerCoord aDelta) const {
  if (!CanScroll()) {
    return false;
  }

  const auto zoom = GetFrameMetrics().GetZoom();
  CSSCoord availableToScroll = 0;

  if (zoom != CSSToParentLayerScale(0)) {
    availableToScroll =
        ParentLayerCoord(
            fabs(DisplacementWillOverscrollAmount(aDelta) - aDelta)) /
        zoom;
  }

  return availableToScroll > COORDINATE_EPSILON;
}

CSSCoord Axis::ClampOriginToScrollableRect(CSSCoord aOrigin) const {
  CSSToParentLayerScale zoom = GetFrameMetrics().GetZoom();
  ParentLayerCoord origin = aOrigin * zoom;
  ParentLayerCoord result;
  if (origin < GetPageStart()) {
    result = GetPageStart();
  } else if (origin + GetCompositionLength() > GetPageEnd()) {
    result = GetPageEnd() - GetCompositionLength();
  } else {
    return aOrigin;
  }
  if (zoom == CSSToParentLayerScale(0)) {
    return aOrigin;
  }
  return result / zoom;
}

bool Axis::CanScrollNow() const { return !IsAxisLocked() && CanScroll(); }

ParentLayerCoord Axis::DisplacementWillOverscrollAmount(
    ParentLayerCoord aDisplacement) const {
  ParentLayerCoord newOrigin = GetOrigin() + aDisplacement;
  ParentLayerCoord newCompositionEnd = GetCompositionEnd() + aDisplacement;
  // If the current pan plus a displacement takes the window to the left of or
  // above the current page rect.
  bool minus = newOrigin < GetPageStart();
  // If the current pan plus a displacement takes the window to the right of or
  // below the current page rect.
  bool plus = newCompositionEnd > GetPageEnd();
  if (minus && plus) {
    // Don't handle overscrolled in both directions; a displacement can't cause
    // this, it must have already been zoomed out too far.
    return 0;
  }
  if (minus) {
    return newOrigin - GetPageStart();
  }
  if (plus) {
    return newCompositionEnd - GetPageEnd();
  }
  return 0;
}

CSSCoord Axis::ScaleWillOverscrollAmount(float aScale, CSSCoord aFocus) const {
  // Internally, do computations in ParentLayer coordinates *before* the scale
  // is applied.
  CSSToParentLayerScale zoom = GetFrameMetrics().GetZoom();
  ParentLayerCoord focus = aFocus * zoom;
  ParentLayerCoord originAfterScale = (GetOrigin() + focus) - (focus / aScale);

  bool both = ScaleWillOverscrollBothSides(aScale);
  bool minus = GetPageStart() - originAfterScale > COORDINATE_EPSILON;
  bool plus =
      (originAfterScale + (GetCompositionLength() / aScale)) - GetPageEnd() >
      COORDINATE_EPSILON;

  if ((minus && plus) || both) {
    // If we ever reach here it's a bug in the client code.
    MOZ_ASSERT(false,
               "In an OVERSCROLL_BOTH condition in ScaleWillOverscrollAmount");
    return 0;
  }
  if (minus && zoom != CSSToParentLayerScale(0)) {
    return (originAfterScale - GetPageStart()) / zoom;
  }
  if (plus && zoom != CSSToParentLayerScale(0)) {
    return (originAfterScale + (GetCompositionLength() / aScale) -
            GetPageEnd()) /
           zoom;
  }
  return 0;
}

bool Axis::IsAxisLocked() const {
  auto axisLocked = mAxisLocked.Lock();
  return axisLocked.ref();
}

void Axis::SetAxisLocked(bool aAxisLocked) {
  auto axisLocked = mAxisLocked.Lock();
  axisLocked.ref() = aAxisLocked;
}

float Axis::GetVelocity() const { return IsAxisLocked() ? 0 : DoGetVelocity(); }

void Axis::SetVelocity(float aVelocity) {
  AXIS_LOG("%p|%s direct-setting velocity to %f\n", mAsyncPanZoomController,
           Name(), aVelocity);
  DoSetVelocity(aVelocity);
}

ParentLayerCoord Axis::GetCompositionEnd() const {
  return GetOrigin() + GetCompositionLength();
}

ParentLayerCoord Axis::GetPageEnd() const {
  return GetPageStart() + GetPageLength();
}

ParentLayerCoord Axis::GetScrollRangeEnd() const {
  return GetPageEnd() - GetCompositionLength();
}

ParentLayerCoord Axis::GetOrigin() const {
  ParentLayerPoint origin =
      GetFrameMetrics().GetVisualScrollOffset() * GetFrameMetrics().GetZoom();
  return GetPointOffset(origin);
}

ParentLayerCoord Axis::GetCompositionLength() const {
  return GetRectLength(GetFrameMetrics().GetCompositionBounds());
}

ParentLayerCoord Axis::GetPageStart() const {
  ParentLayerRect pageRect = GetFrameMetrics().GetExpandedScrollableRect() *
                             GetFrameMetrics().GetZoom();
  return GetRectOffset(pageRect);
}

ParentLayerCoord Axis::GetPageLength() const {
  ParentLayerRect pageRect = GetFrameMetrics().GetExpandedScrollableRect() *
                             GetFrameMetrics().GetZoom();
  return GetRectLength(pageRect);
}

bool Axis::ScaleWillOverscrollBothSides(float aScale) const {
  const FrameMetrics& metrics = GetFrameMetrics();
  ParentLayerRect screenCompositionBounds =
      metrics.GetCompositionBounds() / ParentLayerToParentLayerScale(aScale);
  return GetRectLength(screenCompositionBounds) - GetPageLength() >
         COORDINATE_EPSILON;
}

float Axis::DoGetVelocity() const {
  auto velocity = mVelocity.Lock();
  return velocity.ref();
}
void Axis::DoSetVelocity(float aVelocity) {
  auto velocity = mVelocity.Lock();
  velocity.ref() = aVelocity;
}

const FrameMetrics& Axis::GetFrameMetrics() const {
  return mAsyncPanZoomController->GetFrameMetrics();
}

const ScrollMetadata& Axis::GetScrollMetadata() const {
  return mAsyncPanZoomController->GetScrollMetadata();
}

bool Axis::OverscrollBehaviorAllowsHandoff() const {
  // Scroll handoff is a "non-local" overscroll behavior, so it's allowed
  // with "auto" and disallowed with "contain" and "none".
  return GetOverscrollBehavior() == OverscrollBehavior::Auto;
}

bool Axis::OverscrollBehaviorAllowsOverscrollEffect() const {
  // An overscroll effect is a "local" overscroll behavior, so it's allowed
  // with "auto" and "contain" and disallowed with "none".
  return GetOverscrollBehavior() != OverscrollBehavior::None;
}

AxisX::AxisX(AsyncPanZoomController* aAsyncPanZoomController)
    : Axis(aAsyncPanZoomController) {}

CSSCoord AxisX::GetPointOffset(const CSSPoint& aPoint) const {
  return aPoint.x;
}

OuterCSSCoord AxisX::GetPointOffset(const OuterCSSPoint& aPoint) const {
  return aPoint.x;
}

ParentLayerCoord AxisX::GetPointOffset(const ParentLayerPoint& aPoint) const {
  return aPoint.x;
}

CSSToParentLayerScale AxisX::GetAxisScale(
    const CSSToParentLayerScale2D& aScale) const {
  return CSSToParentLayerScale(aScale.xScale);
}

ParentLayerCoord AxisX::GetRectLength(const ParentLayerRect& aRect) const {
  return aRect.Width();
}

CSSCoord AxisX::GetRectLength(const CSSRect& aRect) const {
  return aRect.Width();
}

ParentLayerCoord AxisX::GetRectOffset(const ParentLayerRect& aRect) const {
  return aRect.X();
}

CSSCoord AxisX::GetRectOffset(const CSSRect& aRect) const { return aRect.X(); }

float AxisX::GetTransformScale(
    const AsyncTransformComponentMatrix& aMatrix) const {
  return aMatrix._11;
}

ParentLayerCoord AxisX::GetTransformTranslation(
    const AsyncTransformComponentMatrix& aMatrix) const {
  return aMatrix._41;
}

void AxisX::PostScale(AsyncTransformComponentMatrix& aMatrix,
                      float aScale) const {
  aMatrix.PostScale(aScale, 1.f, 1.f);
}

void AxisX::PostTranslate(AsyncTransformComponentMatrix& aMatrix,
                          ParentLayerCoord aTranslation) const {
  aMatrix.PostTranslate(aTranslation, 0, 0);
}

ScreenPoint AxisX::MakePoint(ScreenCoord aCoord) const {
  return ScreenPoint(aCoord, 0);
}

const char* AxisX::Name() const { return "X"; }

bool AxisX::CanScrollTo(Side aSide) const {
  switch (aSide) {
    case eSideLeft:
      return CanScroll(CSSCoord(-COORDINATE_EPSILON * 2));
    case eSideRight:
      return CanScroll(CSSCoord(COORDINATE_EPSILON * 2));
    default:
      MOZ_ASSERT_UNREACHABLE("aSide is out of valid values");
      return false;
  }
}

SideBits AxisX::ScrollableDirections() const {
  SideBits directions = SideBits::eNone;

  if (CanScrollTo(eSideLeft)) {
    directions |= SideBits::eLeft;
  }
  if (CanScrollTo(eSideRight)) {
    directions |= SideBits::eRight;
  }

  return directions;
}

OverscrollBehavior AxisX::GetOverscrollBehavior() const {
  return GetScrollMetadata().GetOverscrollBehavior().mBehaviorX;
}

AxisY::AxisY(AsyncPanZoomController* aAsyncPanZoomController)
    : Axis(aAsyncPanZoomController) {}

CSSCoord AxisY::GetPointOffset(const CSSPoint& aPoint) const {
  return aPoint.y;
}

OuterCSSCoord AxisY::GetPointOffset(const OuterCSSPoint& aPoint) const {
  return aPoint.y;
}

ParentLayerCoord AxisY::GetPointOffset(const ParentLayerPoint& aPoint) const {
  return aPoint.y;
}

CSSToParentLayerScale AxisY::GetAxisScale(
    const CSSToParentLayerScale2D& aScale) const {
  return CSSToParentLayerScale(aScale.yScale);
}

ParentLayerCoord AxisY::GetRectLength(const ParentLayerRect& aRect) const {
  return aRect.Height();
}

CSSCoord AxisY::GetRectLength(const CSSRect& aRect) const {
  return aRect.Height();
}

ParentLayerCoord AxisY::GetRectOffset(const ParentLayerRect& aRect) const {
  return aRect.Y();
}

CSSCoord AxisY::GetRectOffset(const CSSRect& aRect) const { return aRect.Y(); }

float AxisY::GetTransformScale(
    const AsyncTransformComponentMatrix& aMatrix) const {
  return aMatrix._22;
}

ParentLayerCoord AxisY::GetTransformTranslation(
    const AsyncTransformComponentMatrix& aMatrix) const {
  return aMatrix._42;
}

void AxisY::PostScale(AsyncTransformComponentMatrix& aMatrix,
                      float aScale) const {
  aMatrix.PostScale(1.f, aScale, 1.f);
}

void AxisY::PostTranslate(AsyncTransformComponentMatrix& aMatrix,
                          ParentLayerCoord aTranslation) const {
  aMatrix.PostTranslate(0, aTranslation, 0);
}

ScreenPoint AxisY::MakePoint(ScreenCoord aCoord) const {
  return ScreenPoint(0, aCoord);
}

const char* AxisY::Name() const { return "Y"; }

bool AxisY::CanScrollTo(Side aSide) const {
  switch (aSide) {
    case eSideTop:
      return CanScroll(CSSCoord(-COORDINATE_EPSILON * 2));
    case eSideBottom:
      return CanScroll(CSSCoord(COORDINATE_EPSILON * 2));
    default:
      MOZ_ASSERT_UNREACHABLE("aSide is out of valid values");
      return false;
  }
}

SideBits AxisY::ScrollableDirections() const {
  SideBits directions = SideBits::eNone;

  if (CanScrollTo(eSideTop)) {
    directions |= SideBits::eTop;
  }
  if (CanScrollTo(eSideBottom)) {
    directions |= SideBits::eBottom;
  }

  return directions;
}

bool AxisY::HasDynamicToolbar() const {
  return GetCompositionLengthWithoutDynamicToolbar() != ParentLayerCoord(0);
}

SideBits AxisY::ScrollableDirectionsWithDynamicToolbar(
    const ScreenMargin& aFixedLayerMargins) const {
  MOZ_ASSERT(mAsyncPanZoomController->IsRootContent());

  SideBits directions = ScrollableDirections();

  if (HasDynamicToolbar()) {
    ParentLayerCoord toolbarHeight =
        GetCompositionLength() - GetCompositionLengthWithoutDynamicToolbar();

    ParentLayerMargin fixedLayerMargins = ViewAs<ParentLayerPixel>(
        aFixedLayerMargins, PixelCastJustification::ScreenIsParentLayerForRoot);

    if (!mAsyncPanZoomController->IsZero(fixedLayerMargins.bottom)) {
      directions |= SideBits::eTop;
    }
    if (mAsyncPanZoomController->FuzzyGreater(
            aFixedLayerMargins.bottom + toolbarHeight, 0)) {
      directions |= SideBits::eBottom;
    }
  }

  return directions;
}

bool AxisY::CanVerticalScrollWithDynamicToolbar() const {
  return !HasDynamicToolbar()
             ? CanScroll()
             : mAsyncPanZoomController->FuzzyGreater(
                   GetPageLength(),
                   GetCompositionLengthWithoutDynamicToolbar());
}

OverscrollBehavior AxisY::GetOverscrollBehavior() const {
  return GetScrollMetadata().GetOverscrollBehavior().mBehaviorY;
}

ParentLayerCoord AxisY::GetCompositionLengthWithoutDynamicToolbar() const {
  return GetFrameMetrics().GetCompositionSizeWithoutDynamicToolbar().Height();
}

}  // namespace layers
}  // namespace mozilla
