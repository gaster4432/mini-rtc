// mf_video.h - H.264 through Media Foundation, on the GPU where available.
//
// One implementation covers NVIDIA NVENC, Intel Quick Sync and AMD VCN,
// discrete and integrated, because Media Foundation fronts all three behind
// IMFTransform. That is the reason to use it rather than a vendor SDK: no
// per-vendor code, and integrated parts are covered for free.
//
// The distinction that cannot be designed away: hardware H.264 *encode* does
// not exist on every GPU. Intel GMA, NVIDIA before Kepler and AMD before NVC0
// have no encoder block, and no API can add one. So hardware is treated as an
// optimisation and never a dependency - every call site falls back to the
// OpenH264 path already in the DLL when a hardware path is unavailable.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

// A bounded queue that never discards.
//
// Bounded, because an unbounded queue is just an out-of-memory condition later.
// Never discarding, because the alternative is what this whole change exists to
// fix: throwing away an access unit orphans the H.264 reference chain, and the
// receiver cannot tell that from a picture that has simply stopped moving. On
// overflow the producer waits instead, which surfaces as backpressure at the
// call site rather than as a silent gap.
template <typename T>
class LosslessQ {
 public:
  explicit LosslessQ(size_t cap) : cap_(cap ? cap : 1) {}

  // Blocks while full. Returns false only once the queue is closed, so a
  // teardown cannot wedge a producer.
  bool push(T v) {
    std::unique_lock<std::mutex> lk(m_);
    notFull_.wait(lk, [&] { return q_.size() < cap_ || closed_; });
    if (closed_) return false;
    q_.push_back(std::move(v));
    lk.unlock();
    notEmpty_.notify_one();
    return true;
  }

  // Waits up to timeoutMs. Returns false on timeout or after close.
  bool pop(T &out, int timeoutMs) {
    std::unique_lock<std::mutex> lk(m_);
    if (!notEmpty_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                            [&] { return !q_.empty() || closed_; }))
      return false;
    if (q_.empty()) return false;
    out = std::move(q_.front());
    q_.pop_front();
    lk.unlock();
    notFull_.notify_one();
    return true;
  }

  size_t size() const {
    std::lock_guard<std::mutex> lk(m_);
    return q_.size();
  }
  bool full() const {
    std::lock_guard<std::mutex> lk(m_);
    return q_.size() >= cap_;
  }
  size_t cap() const { return cap_; }
  void close() {
    {
      std::lock_guard<std::mutex> lk(m_);
      closed_ = true;
    }
    notFull_.notify_all();
    notEmpty_.notify_all();
  }

 private:
  mutable std::mutex m_;
  std::condition_variable notFull_, notEmpty_;
  std::deque<T> q_;
  size_t cap_;
  bool closed_ = false;
};

// True when this machine exposes at least one usable hardware H.264 encoder.
// Probed once and cached; the result is what net_video_hw_info reports.
bool mf_have_hw_encoder();
bool mf_have_hw_decoder();

// A hardware H.264 encoder fed with I420 planes, producing Annex-B.
//
// Async Media Foundation transforms are event driven: the MFT asks for input
// and signals output through an event queue, and nothing progresses unless
// that queue is serviced. So this owns a thread which does that servicing. The
// host never blocks on encoding - send() copies into a one-slot pending buffer
// and returns, and take() collects a finished access unit. Both are one slot
// deep on purpose: a queue here is latency, and a slow encoder should make the
// encoder repeat frames rather than grow a backlog.
class MfEncoder {
 public:
  MfEncoder() = default;
  ~MfEncoder();
  MfEncoder(const MfEncoder &) = delete;
  MfEncoder &operator=(const MfEncoder &) = delete;

  // Tries every hardware H.264 encoder in turn and keeps the first that opens.
  // Returns false if none does, and the caller should use OpenH264 instead.
  bool open(int w, int h, int fps, int kbps);
  void close();
  bool active() const;

  const char *name() const { return name_.c_str(); }
  const std::string &error() const { return error_; }

  // SPS and PPS, harvested from the first encoded keyframe. Both encoders
  // tested emit them in band on the IDR, which is why bSPSandPPSInBand does
  // not need to be set - it is a name-keyed attribute that neither MinGW nor
  // the Windows SDK declares, and it turned out to be unnecessary.
  bool haveSpsPps() const;
  const std::vector<uint8_t> &sps() const;
  const std::vector<uint8_t> &pps() const;

  // Queues one frame. Returns false only if the encoder is not active.
  bool send(const uint8_t *y, int yStride, const uint8_t *u, const uint8_t *v, int cStride);

  // Collects a finished access unit, if one is waiting. Newest wins.
  bool take(std::vector<uint8_t> &au, bool &isIdr);

  // Asks for the next frame to be a keyframe. Used when a receiver sends a
  // PLI, so honouring one must not depend on which codec is running.
  void forceKeyframe();

  // Lossless-pipeline counters. Both must stay at zero; they are exposed so a
  // host can verify that nothing was discarded rather than take it on trust.
  uint64_t droppedIn() const;
  uint64_t droppedOut() const;
  uint64_t repeated() const;
  uint64_t submitted() const;   // inputs the MFT actually accepted
  size_t queued() const;       // frames submitted but not yet encoded

  // True when another frame can be accepted without the producer blocking.
  // This is the real answer behind net_video_can_send, which used to return 1.0
  // unconditionally and so advertised a backpressure signal that did nothing.
  bool hasRoomForInput() const;

  // Mean wall time spent inside the encoder per frame, for diagnostics.
  double encMs() const;
  uint64_t framesIn() const;
  uint64_t framesOut() const;

 private:
  struct Impl;
  Impl *p_ = nullptr;
  std::string name_;
  std::string error_;
};

// A hardware H.264 decoder, producing I420 from Annex-B input.
//
// Separate from MfEncoder because the two disagree on the one thing that is
// easy to get backwards: the encoder needs the *output* type set first, the
// decoder needs the *input* type set first. Getting it wrong yields
// MF_E_TRANSFORM_TYPE_NOT_SET and looks like a broken codec.
//
// The input subtype is MFVideoFormat_H264_ES rather than MFVideoFormat_H264
// because the stream is Annex-B with start codes. That is also how the
// name-keyed bSPSandPPSInBand attribute - declared in no header on this
// machine - is avoided entirely.
//
// No D3D device manager is attached. Acceleration still happens, because it is
// enabled through the attribute store, and leaving the manager off keeps the
// output in system memory where the existing I420 path can read it directly
// instead of needing a GPU read-back.
class MfDecoder {
 public:
  MfDecoder() = default;
  ~MfDecoder();
  MfDecoder(const MfDecoder &) = delete;
  MfDecoder &operator=(const MfDecoder &) = delete;

  // w/h are the dimensions the stream is expected to be. They matter: a
  // hardware decoder rejects an input type carrying only the two mandatory
  // attributes with MF_E_INVALIDMEDIATYPE, and supplying the size and rate
  // also avoids an initial format change.
  bool open(int w, int h);
  void close();
  bool active() const { return p_ != nullptr; }
  const char *name() const { return name_.c_str(); }
  const std::string &error() const { return error_; }

  // Decodes one Annex-B access unit. On success the I420 planes are appended
  // to y/u/v at the reported strides.
  //
  // Returns false when no picture is available yet, which is normal and not an
  // error: a hardware decoder buffers, so the first access units produce no
  // frame. Use failed() to tell that apart from a genuine decode failure, and
  // only ask for a keyframe when failed() is true.
  bool decode(const uint8_t *data, size_t len, std::vector<uint8_t> &y, std::vector<uint8_t> &u,
              std::vector<uint8_t> &v, int *outW, int *outH, int *yStride, int *cStride);
  bool failed() const;
  const char *lastError() const { return lastError_; }

 private:
  struct Impl;
  Impl *p_ = nullptr;
  std::string name_;
  std::string error_;
  const char *lastError_ = "";
};
