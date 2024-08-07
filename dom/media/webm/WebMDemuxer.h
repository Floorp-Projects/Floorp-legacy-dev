/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef WebMDemuxer_h_
#define WebMDemuxer_h_

#include "nsTArray.h"
#include "MediaDataDemuxer.h"
#include "MediaResource.h"
#include "NesteggPacketHolder.h"

#include <deque>
#include <stdint.h>
#include <utility>

typedef struct nestegg nestegg;

namespace mozilla {

class WebMBufferedState;

// Queue for holding MediaRawData samples
class MediaRawDataQueue {
  typedef std::deque<RefPtr<MediaRawData>> ContainerType;

 public:
  uint32_t GetSize() { return mQueue.size(); }

  void Push(MediaRawData* aItem) { mQueue.push_back(aItem); }

  void Push(already_AddRefed<MediaRawData>&& aItem) {
    mQueue.push_back(std::move(aItem));
  }

  void PushFront(MediaRawData* aItem) { mQueue.push_front(aItem); }

  void PushFront(already_AddRefed<MediaRawData>&& aItem) {
    mQueue.push_front(std::move(aItem));
  }

  void PushFront(MediaRawDataQueue&& aOther) {
    while (!aOther.mQueue.empty()) {
      PushFront(aOther.Pop());
    }
  }

  already_AddRefed<MediaRawData> PopFront() {
    RefPtr<MediaRawData> result = std::move(mQueue.front());
    mQueue.pop_front();
    return result.forget();
  }

  already_AddRefed<MediaRawData> Pop() {
    RefPtr<MediaRawData> result = std::move(mQueue.back());
    mQueue.pop_back();
    return result.forget();
  }

  void Reset() {
    while (!mQueue.empty()) {
      mQueue.pop_front();
    }
  }

  MediaRawDataQueue& operator=(const MediaRawDataQueue& aOther) = delete;

  const RefPtr<MediaRawData>& First() const { return mQueue.front(); }

  const RefPtr<MediaRawData>& Last() const { return mQueue.back(); }

  // Methods for range-based for loops.
  ContainerType::iterator begin() { return mQueue.begin(); }

  ContainerType::const_iterator begin() const { return mQueue.begin(); }

  ContainerType::iterator end() { return mQueue.end(); }

  ContainerType::const_iterator end() const { return mQueue.end(); }

 private:
  ContainerType mQueue;
};

class WebMTrackDemuxer;

DDLoggedTypeDeclNameAndBase(WebMDemuxer, MediaDataDemuxer);
DDLoggedTypeNameAndBase(WebMTrackDemuxer, MediaTrackDemuxer);

class WebMDemuxer : public MediaDataDemuxer,
                    public DecoderDoctorLifeLogger<WebMDemuxer> {
 public:
  explicit WebMDemuxer(MediaResource* aResource);
  // Indicate if the WebMDemuxer is to be used with MediaSource. In which
  // case the demuxer will stop reads to the last known complete block.
  WebMDemuxer(
      MediaResource* aResource, bool aIsMediaSource,
      Maybe<media::TimeUnit> aFrameEndTimeBeforeRecreateDemuxer = Nothing());

  RefPtr<InitPromise> Init() override;

  uint32_t GetNumberTracks(TrackInfo::TrackType aType) const override;

  UniquePtr<TrackInfo> GetTrackInfo(TrackInfo::TrackType aType,
                                    size_t aTrackNumber) const;

  already_AddRefed<MediaTrackDemuxer> GetTrackDemuxer(
      TrackInfo::TrackType aType, uint32_t aTrackNumber) override;

  bool IsSeekable() const override;

  bool IsSeekableOnlyInBufferedRanges() const override;

  UniquePtr<EncryptionInfo> GetCrypto() override;

  bool GetOffsetForTime(uint64_t aTime, int64_t* aOffset);

  // Demux next WebM packet and append samples to MediaRawDataQueue
  nsresult GetNextPacket(TrackInfo::TrackType aType,
                         MediaRawDataQueue* aSamples);

  void Reset(TrackInfo::TrackType aType);

  // Pushes a packet to the front of the audio packet queue.
  void PushAudioPacket(NesteggPacketHolder* aItem);

  // Pushes a packet to the front of the video packet queue.
  void PushVideoPacket(NesteggPacketHolder* aItem);

  // Public accessor for nestegg callbacks
  bool IsMediaSource() const { return mIsMediaSource; }

  int64_t LastWebMBlockOffset() const { return mLastWebMBlockOffset; }

  struct NestEggContext {
    NestEggContext(WebMDemuxer* aParent, MediaResource* aResource)
        : mParent(aParent), mResource(aResource), mContext(nullptr) {}

    ~NestEggContext();

    int Init();

    // Public accessor for nestegg callbacks

    bool IsMediaSource() const { return mParent->IsMediaSource(); }
    MediaResourceIndex* GetResource() { return &mResource; }

    int64_t GetEndDataOffset() const {
      return (!mParent->IsMediaSource() || mParent->LastWebMBlockOffset() < 0)
                 ? mResource.GetLength()
                 : mParent->LastWebMBlockOffset();
    }

    WebMDemuxer* mParent;
    MediaResourceIndex mResource;
    nestegg* mContext;
  };

 private:
  friend class WebMTrackDemuxer;

  ~WebMDemuxer();
  void InitBufferedState();
  nsresult ReadMetadata();
  void NotifyDataArrived() override;
  void NotifyDataRemoved() override;
  void EnsureUpToDateIndex();

  // A helper to catch bad intervals during `GetBuffered`.
  // Verifies if the interval given by start and end is valid, returning true if
  // it is, or false if not. Logs failure reason if the interval is invalid.
  bool IsBufferedIntervalValid(uint64_t start, uint64_t end);

  media::TimeIntervals GetBuffered();
  nsresult SeekInternal(TrackInfo::TrackType aType,
                        const media::TimeUnit& aTarget);
  CryptoTrack GetTrackCrypto(TrackInfo::TrackType aType, size_t aTrackNumber);

  // Read a packet from the nestegg file. Returns nullptr if all packets for
  // the particular track have been read. Pass TrackInfo::kVideoTrack or
  // TrackInfo::kVideoTrack to indicate the type of the packet we want to read.
  nsresult NextPacket(TrackInfo::TrackType aType,
                      RefPtr<NesteggPacketHolder>& aPacket);

  // Internal method that demuxes the next packet from the stream. The caller
  // is responsible for making sure it doesn't get lost.
  nsresult DemuxPacket(TrackInfo::TrackType aType,
                       RefPtr<NesteggPacketHolder>& aPacket);

  // libnestegg audio and video context for webm container.
  // Access on reader's thread only.
  NestEggContext mVideoContext;
  NestEggContext mAudioContext;
  MediaResourceIndex& Resource(TrackInfo::TrackType aType) {
    return aType == TrackInfo::kVideoTrack ? mVideoContext.mResource
                                           : mAudioContext.mResource;
  }
  nestegg* Context(TrackInfo::TrackType aType) const {
    return aType == TrackInfo::kVideoTrack ? mVideoContext.mContext
                                           : mAudioContext.mContext;
  }

  MediaInfo mInfo;
  nsTArray<RefPtr<WebMTrackDemuxer>> mDemuxers;

  // Parser state and computed offset-time mappings.  Shared by multiple
  // readers when decoder has been cloned.  Main thread only.
  RefPtr<WebMBufferedState> mBufferedState;
  RefPtr<MediaByteBuffer> mInitData;

  // Queue of video and audio packets that have been read but not decoded.
  WebMPacketQueue mVideoPackets;
  WebMPacketQueue mAudioPackets;

  // Index of video and audio track to play
  uint32_t mVideoTrack;
  uint32_t mAudioTrack;

  // Nanoseconds to discard after seeking.
  uint64_t mSeekPreroll;

  // Calculate the frame duration from the last decodeable frame using the
  // previous frame's timestamp.  In NS.
  Maybe<int64_t> mLastAudioFrameTime;
  Maybe<int64_t> mLastVideoFrameTime;

  Maybe<media::TimeUnit> mVideoFrameEndTimeBeforeReset;

  // Codec ID of audio track
  int mAudioCodec;
  // Codec ID of video track
  int mVideoCodec;

  // Booleans to indicate if we have audio and/or video data
  bool mHasVideo;
  bool mHasAudio;
  bool mNeedReIndex;

  // The last complete block parsed by the WebMBufferedState. -1 if not set.
  // We cache those values rather than retrieving them for performance reasons
  // as nestegg only performs 1-byte read at a time.
  int64_t mLastWebMBlockOffset;
  const bool mIsMediaSource;

  EncryptionInfo mCrypto;
};

class WebMTrackDemuxer : public MediaTrackDemuxer,
                         public DecoderDoctorLifeLogger<WebMTrackDemuxer> {
 public:
  WebMTrackDemuxer(WebMDemuxer* aParent, TrackInfo::TrackType aType,
                   uint32_t aTrackNumber);

  UniquePtr<TrackInfo> GetInfo() const override;

  RefPtr<SeekPromise> Seek(const media::TimeUnit& aTime) override;

  RefPtr<SamplesPromise> GetSamples(int32_t aNumSamples = 1) override;

  void Reset() override;

  nsresult GetNextRandomAccessPoint(media::TimeUnit* aTime) override;

  RefPtr<SkipAccessPointPromise> SkipToNextRandomAccessPoint(
      const media::TimeUnit& aTimeThreshold) override;

  media::TimeIntervals GetBuffered() override;

  int64_t GetEvictionOffset(const media::TimeUnit& aTime) override;

  void BreakCycles() override;

 private:
  friend class WebMDemuxer;
  ~WebMTrackDemuxer();
  void UpdateSamples(const nsTArray<RefPtr<MediaRawData>>& aSamples);
  void SetNextKeyFrameTime();
  nsresult NextSample(RefPtr<MediaRawData>& aData);
  RefPtr<WebMDemuxer> mParent;
  TrackInfo::TrackType mType;
  UniquePtr<TrackInfo> mInfo;
  Maybe<media::TimeUnit> mNextKeyframeTime;
  bool mNeedKeyframe;

  // Queued samples extracted by the demuxer, but not yet returned.
  MediaRawDataQueue mSamples;
};

}  // namespace mozilla

#endif
