/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_Axis_h
#define mozilla_layers_Axis_h

#include <sys/types.h>  // for int32_t

#include "APZUtils.h"
#include "AxisPhysicsMSDModel.h"
#include "mozilla/DataMutex.h"  // for DataMutex
#include "mozilla/gfx/Types.h"  // for Side
#include "mozilla/TimeStamp.h"  // for TimeDuration
#include "nsTArray.h"           // for nsTArray
#include "Units.h"

namespace mozilla {
namespace layers {

const float EPSILON = 0.0001f;

/**
 * Compare two coordinates for equality, accounting for rounding error.
 * Use both FuzzyEqualsAdditive() with COORDINATE_EPISLON, which accounts for
 * things like the error introduced by rounding during a round-trip to app
 * units, and FuzzyEqualsMultiplicative(), which accounts for accumulated error
 * due to floating-point operations (which can be larger than COORDINATE_EPISLON
 * for sufficiently large coordinate values).
 */
bool FuzzyEqualsCoordinate(CSSCoord aValue1, CSSCoord aValue2);

struct FrameMetrics;
class AsyncPanZoomController;

/**
 * Interface for computing velocities along the axis based on
 * position samples.
 */
class VelocityTracker {
 public:
  virtual ~VelocityTracker() = default;

  /**
   * Start tracking velocity along this axis, starting with the given
   * initial position and corresponding timestamp.
   */
  virtual void StartTracking(ParentLayerCoord aPos, TimeStamp aTimestamp) = 0;
  /**
   * Record a new position along this axis, at the given timestamp.
   * Returns the average velocity between the last sample and this one, or
   * or Nothing() if a reasonable average cannot be computed.
   */
  virtual Maybe<float> AddPosition(ParentLayerCoord aPos,
                                   TimeStamp aTimestamp) = 0;
  /**
   * Compute an estimate of the axis's current velocity, based on recent
   * position samples. It's up to implementation how many samples to consider
   * and how to perform the computation.
   * If the tracker doesn't have enough samples to compute a result, it
   * may return Nothing{}.
   */
  virtual Maybe<float> ComputeVelocity(TimeStamp aTimestamp) = 0;
  /**
   * Clear all state in the velocity tracker.
   */
  virtual void Clear() = 0;
};

/**
 * Helper class to maintain each axis of movement (X,Y) for panning and zooming.
 * Note that everything here is specific to one axis; that is, the X axis knows
 * nothing about the Y axis and vice versa.
 */
class Axis {
 public:
  explicit Axis(AsyncPanZoomController* aAsyncPanZoomController);

  /**
   * Notify this Axis that a new touch has been received, including a timestamp
   * for when the touch was received. This triggers a recalculation of velocity.
   * This can also used for pan gesture events. For those events, |aPos| is
   * an invented position corresponding to the mouse position plus any
   * accumulated displacements over the course of the pan gesture.
   */
  void UpdateWithTouchAtDevicePoint(ParentLayerCoord aPos,
                                    TimeStamp aTimestamp);

 public:
  /**
   * Notify this Axis that a touch has begun, i.e. the user has put their finger
   * on the screen but has not yet tried to pan.
   */
  void StartTouch(ParentLayerCoord aPos, TimeStamp aTimestamp);

  /**
   * Helper enum class for specifying if EndTouch() should clear the axis lock.
   */
  enum class ClearAxisLock { Yes, No };

  /**
   * Notify this Axis that a touch has ended gracefully. This may perform
   * recalculations of the axis velocity.
   */
  void EndTouch(TimeStamp aTimestamp, ClearAxisLock aClearAxisLock);

  /**
   * Notify this Axis that the gesture has ended forcefully. Useful for stopping
   * flings when a user puts their finger down in the middle of one (i.e. to
   * stop a previous touch including its fling so that a new one can take its
   * place).
   */
  void CancelGesture();

  /**
   * Takes a requested displacement to the position of this axis, and adjusts it
   * to account for overscroll (which might decrease the displacement; this is
   * to prevent the viewport from overscrolling the page rect), and axis locking
   * (which might prevent any displacement from happening). If overscroll
   * ocurred, its amount is written to |aOverscrollAmountOut|.
   * The |aDisplacementOut| parameter is set to the adjusted displacement, and
   * the function returns true if and only if internal overscroll amounts were
   * changed.
   */
  bool AdjustDisplacement(ParentLayerCoord aDisplacement,
                          ParentLayerCoord& aDisplacementOut,
                          ParentLayerCoord& aOverscrollAmountOut,
                          bool aForceOverscroll = false);

  /**
   * Overscrolls this axis by the requested amount in the requested direction.
   * The axis must be at the end of its scroll range in this direction.
   */
  void OverscrollBy(ParentLayerCoord aOverscroll);

  /**
   * Return the amount of overscroll on this axis, in ParentLayer pixels.
   *
   * If this amount is nonzero, the relevant component of
   * mAsyncPanZoomController->Metrics().mScrollOffset must be at its
   * extreme allowed value in the relevant direction (that is, it must be at
   * its maximum value if we are overscrolled at our composition length, and
   * at its minimum value if we are overscrolled at the origin).
   */
  ParentLayerCoord GetOverscroll() const;

  /**
   * Restore the amount by which this axis is overscrolled to the specified
   * amount. This is for test-related use; overscrolling as a result of user
   * input should happen via OverscrollBy().
   */
  void RestoreOverscroll(ParentLayerCoord aOverscroll);

  /**
   * Start an overscroll animation with the given initial velocity.
   */
  void StartOverscrollAnimation(float aVelocity);

  /**
   * Sample the snap-back animation to relieve overscroll.
   * |aDelta| is the time since the last sample, |aOverscrollSideBits| is
   * the direction where the overscroll happens on this axis.
   */
  bool SampleOverscrollAnimation(const TimeDuration& aDelta,
                                 SideBits aOverscrollSideBits);

  /**
   * Stop an overscroll animation.
   */
  void EndOverscrollAnimation();

  /**
   * Return whether this axis is overscrolled in either direction.
   */
  bool IsOverscrolled() const;

  /**
   * Return true if this axis is overscrolled but its scroll offset
   * has changed in a way that makes the oversrolled state no longer
   * valid (for example, it is overscrolled at the top but the
   * scroll offset is no longer zero).
   */
  bool IsInInvalidOverscroll() const;

  /**
   * Clear any overscroll amount on this axis.
   */
  void ClearOverscroll();

  /**
   * Returns whether the overscroll animation is alive.
   */
  bool IsOverscrollAnimationAlive() const;

  /**
   * Returns whether the overscroll animation is running.
   * Note that unlike the above IsOverscrollAnimationAlive, this function
   * returns false even if the animation is still there but is very close to
   * the destination position and its velocity is quite low, i.e. it's time to
   * finish.
   */
  bool IsOverscrollAnimationRunning() const;

  /**
   * Gets the starting position of the touch supplied in StartTouch().
   */
  ParentLayerCoord PanStart() const;

  /**
   * Gets the distance between the starting position of the touch supplied in
   * StartTouch() and the current touch from the last
   * UpdateWithTouchAtDevicePoint().
   */
  ParentLayerCoord PanDistance() const;

  /**
   * Gets the distance between the starting position of the touch supplied in
   * StartTouch() and the supplied position.
   */
  ParentLayerCoord PanDistance(ParentLayerCoord aPos) const;

  /**
   * Returns true if the page has room to be scrolled along this axis.
   */
  bool CanScroll() const;

  /**
   * Returns whether this axis can scroll any more in a particular direction.
   */
  bool CanScroll(CSSCoord aDelta) const;
  bool CanScroll(ParentLayerCoord aDelta) const;

  /**
   * Returns true if the page has room to be scrolled along this axis
   * and this axis is not scroll-locked.
   */
  bool CanScrollNow() const;

  /**
   * Clamp a point to the page's scrollable bounds. That is, a scroll
   * destination to the returned point will not contain any overscroll.
   */
  CSSCoord ClampOriginToScrollableRect(CSSCoord aOrigin) const;

  /**
   * Gets the raw velocity of this axis at this moment.
   */
  float GetVelocity() const;

  /**
   * Sets the raw velocity of this axis at this moment.
   * Intended to be called only when the axis "takes over" a velocity from
   * another APZC, in which case there are no touch points available to call
   * UpdateWithTouchAtDevicePoint. In other circumstances,
   * UpdateWithTouchAtDevicePoint should be used and the velocity calculated
   * there.
   */
  void SetVelocity(float aVelocity);

  /**
   * If a displacement will overscroll the axis, this returns the amount and in
   * what direction.
   */
  ParentLayerCoord DisplacementWillOverscrollAmount(
      ParentLayerCoord aDisplacement) const;

  /**
   * If a scale will overscroll the axis, this returns the amount and in what
   * direction.
   *
   * |aFocus| is the point at which the scale is focused at. We will offset the
   * scroll offset in such a way that it remains in the same place on the page
   * relative.
   *
   * Note: Unlike most other functions in Axis, this functions operates in
   *       CSS coordinates so there is no confusion as to whether the
   *       ParentLayer coordinates it operates in are before or after the scale
   *       is applied.
   */
  CSSCoord ScaleWillOverscrollAmount(float aScale, CSSCoord aFocus) const;

  /**
   * Checks if an axis will overscroll in both directions by computing the
   * content rect and checking that its height/width (depending on the axis)
   * does not overextend past the viewport.
   *
   * This gets called by ScaleWillOverscroll().
   */
  bool ScaleWillOverscrollBothSides(float aScale) const;

  /**
   * Returns true if movement on this axis is locked.
   */
  bool IsAxisLocked() const;

  /**
   * Set whether or not the axis is locked.
   */
  void SetAxisLocked(bool aAxisLocked);

  ParentLayerCoord GetOrigin() const;
  ParentLayerCoord GetCompositionLength() const;
  ParentLayerCoord GetPageStart() const;
  ParentLayerCoord GetPageLength() const;
  ParentLayerCoord GetCompositionEnd() const;
  ParentLayerCoord GetPageEnd() const;
  ParentLayerCoord GetScrollRangeEnd() const;

  bool IsScrolledToStart() const;
  bool IsScrolledToEnd() const;

  ParentLayerCoord GetPos() const { return mPos; }

  bool OverscrollBehaviorAllowsHandoff() const;
  bool OverscrollBehaviorAllowsOverscrollEffect() const;

  virtual CSSToParentLayerScale GetAxisScale(
      const CSSToParentLayerScale2D& aScale) const = 0;
  virtual CSSCoord GetPointOffset(const CSSPoint& aPoint) const = 0;
  virtual OuterCSSCoord GetPointOffset(const OuterCSSPoint& aPoint) const = 0;
  virtual ParentLayerCoord GetPointOffset(
      const ParentLayerPoint& aPoint) const = 0;
  virtual ParentLayerCoord GetRectLength(
      const ParentLayerRect& aRect) const = 0;
  virtual CSSCoord GetRectLength(const CSSRect& aRect) const = 0;
  virtual ParentLayerCoord GetRectOffset(
      const ParentLayerRect& aRect) const = 0;
  virtual CSSCoord GetRectOffset(const CSSRect& aRect) const = 0;
  virtual float GetTransformScale(
      const AsyncTransformComponentMatrix& aMatrix) const = 0;
  virtual ParentLayerCoord GetTransformTranslation(
      const AsyncTransformComponentMatrix& aMatrix) const = 0;
  virtual void PostScale(AsyncTransformComponentMatrix& aMatrix,
                         float aScale) const = 0;
  virtual void PostTranslate(AsyncTransformComponentMatrix& aMatrix,
                             ParentLayerCoord aTranslation) const = 0;

  virtual ScreenPoint MakePoint(ScreenCoord aCoord) const = 0;

  const void* OpaqueApzcPointer() const { return mAsyncPanZoomController; }

  virtual const char* Name() const = 0;

  // Convert a velocity from global inches/ms into ParentLayerCoords/ms.
  float ToLocalVelocity(float aVelocityInchesPerMs) const;

 protected:
  // A position along the axis, used during input event processing to
  // track velocities (and for touch gestures, to track the length of
  // the gesture). For touch events, this represents the position of
  // the finger (or in the case of two-finger scrolling, the midpoint
  // of the two fingers). For pan gesture events, this represents an
  // invented position corresponding to the mouse position at the start
  // of the pan, plus deltas representing the displacement of the pan.
  ParentLayerCoord mPos;

  ParentLayerCoord mStartPos;
  // The velocity can be accessed from multiple threads (e.g. APZ
  // controller thread and APZ sampler thread), so needs to be
  // protected by a mutex.
  // Units: ParentLayerCoords per millisecond
  mutable DataMutex<float> mVelocity;
  // Whether movement on this axis is locked.
  mutable DataMutex<bool> mAxisLocked;
  AsyncPanZoomController* mAsyncPanZoomController;

  // The amount by which we are overscrolled; see GetOverscroll().
  ParentLayerCoord mOverscroll;

  // The mass-spring-damper model for overscroll physics.
  AxisPhysicsMSDModel mMSDModel;

  // Used to track velocity over a series of input events and compute
  // a resulting velocity to use for e.g. starting a fling animation.
  // This member can only be accessed on the controller/UI thread.
  UniquePtr<VelocityTracker> mVelocityTracker;

  float DoGetVelocity() const;
  void DoSetVelocity(float aVelocity);

  const FrameMetrics& GetFrameMetrics() const;
  const ScrollMetadata& GetScrollMetadata() const;

  // Do not use this function directly, use
  // AsyncPanZoomController::GetAllowedHandoffDirections instead.
  virtual OverscrollBehavior GetOverscrollBehavior() const = 0;

  // Adjust a requested overscroll amount for resistance, yielding a smaller
  // actual overscroll amount.
  ParentLayerCoord ApplyResistance(ParentLayerCoord aOverscroll) const;

  // Helper function for SampleOverscrollAnimation().
  void StepOverscrollAnimation(double aStepDurationMilliseconds);
};

class AxisX : public Axis {
 public:
  explicit AxisX(AsyncPanZoomController* mAsyncPanZoomController);
  CSSToParentLayerScale GetAxisScale(
      const CSSToParentLayerScale2D& aScale) const override;
  CSSCoord GetPointOffset(const CSSPoint& aPoint) const override;
  OuterCSSCoord GetPointOffset(const OuterCSSPoint& aPoint) const override;
  ParentLayerCoord GetPointOffset(
      const ParentLayerPoint& aPoint) const override;
  ParentLayerCoord GetRectLength(const ParentLayerRect& aRect) const override;
  CSSCoord GetRectLength(const CSSRect& aRect) const override;
  ParentLayerCoord GetRectOffset(const ParentLayerRect& aRect) const override;
  CSSCoord GetRectOffset(const CSSRect& aRect) const override;
  float GetTransformScale(
      const AsyncTransformComponentMatrix& aMatrix) const override;
  ParentLayerCoord GetTransformTranslation(
      const AsyncTransformComponentMatrix& aMatrix) const override;
  void PostScale(AsyncTransformComponentMatrix& aMatrix,
                 float aScale) const override;
  void PostTranslate(AsyncTransformComponentMatrix& aMatrix,
                     ParentLayerCoord aTranslation) const override;
  ScreenPoint MakePoint(ScreenCoord aCoord) const override;
  const char* Name() const override;
  bool CanScrollTo(Side aSide) const;
  SideBits ScrollableDirections() const;

 private:
  OverscrollBehavior GetOverscrollBehavior() const override;
};

class AxisY : public Axis {
 public:
  explicit AxisY(AsyncPanZoomController* mAsyncPanZoomController);
  CSSCoord GetPointOffset(const CSSPoint& aPoint) const override;
  OuterCSSCoord GetPointOffset(const OuterCSSPoint& aPoint) const override;
  ParentLayerCoord GetPointOffset(
      const ParentLayerPoint& aPoint) const override;
  CSSToParentLayerScale GetAxisScale(
      const CSSToParentLayerScale2D& aScale) const override;
  ParentLayerCoord GetRectLength(const ParentLayerRect& aRect) const override;
  CSSCoord GetRectLength(const CSSRect& aRect) const override;
  ParentLayerCoord GetRectOffset(const ParentLayerRect& aRect) const override;
  CSSCoord GetRectOffset(const CSSRect& aRect) const override;
  float GetTransformScale(
      const AsyncTransformComponentMatrix& aMatrix) const override;
  ParentLayerCoord GetTransformTranslation(
      const AsyncTransformComponentMatrix& aMatrix) const override;
  void PostScale(AsyncTransformComponentMatrix& aMatrix,
                 float aScale) const override;
  void PostTranslate(AsyncTransformComponentMatrix& aMatrix,
                     ParentLayerCoord aTranslation) const override;
  ScreenPoint MakePoint(ScreenCoord aCoord) const override;
  const char* Name() const override;
  bool CanScrollTo(Side aSide) const;
  bool CanVerticalScrollWithDynamicToolbar() const;
  SideBits ScrollableDirections() const;
  SideBits ScrollableDirectionsWithDynamicToolbar(
      const ScreenMargin& aFixedLayerMargins) const;

 private:
  OverscrollBehavior GetOverscrollBehavior() const override;
  ParentLayerCoord GetCompositionLengthWithoutDynamicToolbar() const;
  bool HasDynamicToolbar() const;
};

}  // namespace layers
}  // namespace mozilla

#endif
