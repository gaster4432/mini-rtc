// mf_compat.h - declarations Media Foundation needs that MinGW does not ship.
//
// Every item here is transcribed from the Windows SDK, not inferred. MinGW's
// Media Foundation headers are complete enough to run a hardware H.264 pipeline
// (see examples/mfd3d.cpp, which encodes real frames) but are missing a few
// specific things:
//
//   - IMFDXGIDeviceManager is forward-declared in mfobjects.h and never defined.
//   - Several encoder/async attribute GUIDs appear in no MinGW header.
//
// Anything MinGW *does* declare is used directly rather than redeclared here,
// including MFSetAttributeSize, MFSetAttributeRatio, IMFMediaEventGenerator,
// METransformNeedInput/METransformHaveOutput, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
// MF_SA_D3D11_AWARE, MFVideoFormat_H264_ES and MFT_SET_TYPE_TEST_ONLY.
//
// The probe verified the full chain end to end before any of this was written
// into the library: hardware H.264 encoders accept a hand-built output type and
// an enumerated-plus-patched input type, and emit Annex-B with SPS (7), PPS (8)
// and IDR (5) in band on the keyframe.
#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mfobjects.h>
#include <Mferror.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// IMFDXGIDeviceManager, from the Windows SDK mfobjects.h.
// IID eb533d5d-2db6-40f8-97a9-494692014f07. Method order is the SDK's.
//
// Despite the name this is not a device owner. It is an adapter lock manager:
// every method takes a HANDLE naming the GPU adapter, and its DXGI device is
// uninitialised until ResetDevice is called. Skipping ResetDevice makes
// OpenDeviceHandle fail with MF_E_DXGI_DEVICE_NOT_INITIALIZED (0x80041000).
// ---------------------------------------------------------------------------
struct MfD3D11DeviceManager : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE CloseDeviceHandle(HANDLE) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetVideoService(HANDLE, REFIID, void **) = 0;
  virtual HRESULT STDMETHODCALLTYPE LockDevice(HANDLE, REFIID, void **, BOOL) = 0;
  virtual HRESULT STDMETHODCALLTYPE OpenDeviceHandle(HANDLE *) = 0;
  virtual HRESULT STDMETHODCALLTYPE ResetDevice(IUnknown *, UINT) = 0;
  virtual HRESULT STDMETHODCALLTYPE TestDevice(HANDLE) = 0;
  virtual HRESULT STDMETHODCALLTYPE UnlockDevice(HANDLE, BOOL) = 0;
};

// ---------------------------------------------------------------------------
// Attribute GUIDs, from the Windows SDK. None appear in MinGW's MF headers.
// ---------------------------------------------------------------------------

// Latency. Asynchronous MFTs answer every call with
// MF_E_TRANSFORM_ASYNC_LOCKED until MF_TRANSFORM_ASYNC_UNLOCK is set, so this
// is what unlocks them. The async MFTs also report themselves async through
// MF_TRANSFORM_ASYNC; there is no other way to tell a locked MFT from a broken
// one, and setting the unlock on a synchronous MFT is harmless.
static const GUID kMfTransformAsync = {0xf81a699a, 0x649a, 0x497d,
                                       {0x8c, 0x73, 0x29, 0xf8, 0xfe, 0xd6, 0xad, 0x7a}};
static const GUID kMfTransformAsyncUnlock = {0xe5666d6b, 0x3422, 0x4eb6,
                                             {0xa4, 0x21, 0xda, 0x7d, 0xb1, 0xf8, 0xe2, 0x7}};

// Low latency, from mfapi.h.
static const GUID kMfLowLatency = {0x9c27891a, 0xed7a, 0x40e1,
                                   {0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee}};

// Encoder attributes, from the Windows SDK codecapi.h. These are the
// ICodecAPI values, applied through the MFT's attribute store, which is how
// they are set without an ICodecAPI declaration.
static const GUID kEncCommonMeanBitRate = {0xf7222374, 0x2144, 0x4815,
                                           {0xb5, 0x50, 0xa3, 0x7f, 0x8e, 0x12, 0xee, 0x52}};
static const GUID kEncCommonLowLatency = {0x9d3ecd55, 0x89e8, 0x490a,
                                          {0x97, 0x0a, 0x0c, 0x95, 0x48, 0xd5, 0xa5, 0x6e}};
static const GUID kEncCommonRealTime = {0x143a0ff6, 0xa131, 0x43da,
                                        {0xb8, 0x1e, 0x98, 0xfb, 0xb8, 0xec, 0x37, 0x8e}};
static const GUID kEncVideoForceKeyFrame = {0x398c1b98, 0x8353, 0x475a,
                                            {0x9e, 0xf2, 0x8f, 0x26, 0x5d, 0x26, 0x03, 0x45}};

// Turns hardware acceleration on for an H.264 decoder. Without it the decoder
// runs on the CPU even though a GPU decoder was enumerated.
static const GUID kDecVideoAccelerationH264 = {
    0xf7db8a2f, 0x4f48, 0x4ee8, {0xae, 0x31, 0x8b, 0x6e, 0xbe, 0x55, 0x8a, 0xe2}};

// Sample attribute: forces the current input sample to be encoded as a keyframe.
// MFSampleExtension_VideoEncodePictureType (from mfapi.h):
// {973704e6-cd14-483c-8f20-c9fc0928bad5}, value 2 = eAVEncVideoForceKeyFrame.
static const GUID kSampleExtVideoEncodePictureType = {
    0x973704e6, 0xcd14, 0x483c, {0x8f, 0x20, 0xc9, 0xfc, 0x09, 0x28, 0xba, 0xd5}};

// GOP size: CODECAPI_AVEncMPVGOPSize {83a31530-63e5-4f6a-8284-3b42a9573b04}
static const GUID kEncMPVGOPSize = {
    0x83a31530, 0x63e5, 0x4f6a, {0x82, 0x84, 0x3b, 0x42, 0xa9, 0x57, 0x3b, 0x04}};
// B-frame count: CODECAPI_AVEncMPVDefaultBPictureCount {8d390aac-dc5c-4200-b57f-814d04babab2}
static const GUID kEncMPVDefaultBPictureCount = {
    0x8d390aac, 0xdc5c, 0x4200, {0xb5, 0x7f, 0x81, 0x4d, 0x04, 0xba, 0xba, 0xb2}};

// Minimal ICodecAPI interface (from strmif.h / codecapi.h, IID 901db4c7-31ce-41a2-85dc-8fa0bf41b8da)
// Hardware MFTs (NVENC / QuickSync / VCN) often require ICodecAPI::SetValue for
// runtime properties such as CODECAPI_AVEncVideoForceKeyFrame.
static const GUID kIID_ICodecAPI = {
    0x901db4c7, 0x31ce, 0x41a2, {0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda}};

struct MfCodecAPI : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE IsSupported(const GUID *Api) = 0;
  virtual HRESULT STDMETHODCALLTYPE IsModifiable(const GUID *Api) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetParameterRange(const GUID *Api, VARIANT *ValueMin,
                                                      VARIANT *ValueMax,
                                                      VARIANT *SteppingDelta) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetParameterValues(const GUID *Api, VARIANT **Values,
                                                       ULONG *ValuesCount) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDefaultValue(const GUID *Api, VARIANT *Value) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetValue(const GUID *Api, VARIANT *Value) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetValue(const GUID *Api, VARIANT *Value) = 0;
};

