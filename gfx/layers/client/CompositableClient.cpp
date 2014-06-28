/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/layers/CompositableClient.h"
#include <stdint.h>                     // for uint64_t, uint32_t
#include "gfxPlatform.h"                // for gfxPlatform
#include "mozilla/layers/CompositableForwarder.h"
#include "mozilla/layers/TextureClient.h"  // for DeprecatedTextureClient, etc
#include "mozilla/layers/TextureClientOGL.h"
#include "mozilla/mozalloc.h"           // for operator delete, etc
#include "gfxASurface.h"                // for gfxContentType
#ifdef XP_WIN
#include "gfxWindowsPlatform.h"         // for gfxWindowsPlatform
#include "mozilla/layers/TextureD3D11.h"
#include "mozilla/layers/TextureD3D9.h"
#endif

using namespace mozilla::gfx;

namespace mozilla {
namespace layers {

CompositableClient::CompositableClient(CompositableForwarder* aForwarder,
                                       TextureFlags aTextureFlags)
: mCompositableChild(nullptr)
, mForwarder(aForwarder)
, mTextureFlags(aTextureFlags)
{
  MOZ_COUNT_CTOR(CompositableClient);
}

CompositableClient::~CompositableClient()
{
  MOZ_COUNT_DTOR(CompositableClient);
  Destroy();
}

LayersBackend
CompositableClient::GetCompositorBackendType() const
{
  return mForwarder->GetCompositorBackendType();
}

void
CompositableClient::SetIPDLActor(CompositableChild* aChild)
{
  mCompositableChild = aChild;
}

CompositableChild*
CompositableClient::GetIPDLActor() const
{
  return mCompositableChild;
}

bool
CompositableClient::Connect()
{
  if (!GetForwarder() || GetIPDLActor()) {
    return false;
  }
  GetForwarder()->Connect(this);
  return true;
}

void
CompositableClient::Destroy()
{
  if (!mCompositableChild) {
    return;
  }
  mCompositableChild->SetClient(nullptr);
  mCompositableChild->Destroy();
  mCompositableChild = nullptr;
}

uint64_t
CompositableClient::GetAsyncID() const
{
  if (mCompositableChild) {
    return mCompositableChild->GetAsyncID();
  }
  return 0; // zero is always an invalid async ID
}

void
RemoveTextureFromCompositableTracker::ReleaseTextureClient()
{
  if (mTextureClient) {
    TextureClientReleaseTask* task = new TextureClientReleaseTask(mTextureClient);
    RefPtr<ISurfaceAllocator> allocator = mTextureClient->GetAllocator();
    mTextureClient = nullptr;
    allocator->GetMessageLoop()->PostTask(FROM_HERE, task);
  }
}

/* static */ void
CompositableClient::TransactionCompleteted(PCompositableChild* aActor, uint64_t aTransactionId)
{
  CompositableChild* child = static_cast<CompositableChild*>(aActor);
  child->TransactionCompleteted(aTransactionId);
}

/* static */ void
CompositableClient::HoldUntilComplete(PCompositableChild* aActor, AsyncTransactionTracker* aTracker)
{
  CompositableChild* child = static_cast<CompositableChild*>(aActor);
  child->HoldUntilComplete(aTracker);
}

/* static */ uint64_t
CompositableClient::GetTrackersHolderId(PCompositableChild* aActor)
{
  CompositableChild* child = static_cast<CompositableChild*>(aActor);
  return child->GetId();
}

void
CompositableChild::Destroy()
{
  Send__delete__(this);
}

TemporaryRef<DeprecatedTextureClient>
CompositableClient::CreateDeprecatedTextureClient(DeprecatedTextureClientType aDeprecatedTextureClientType,
                                                  gfxContentType aContentType)
{
  MOZ_ASSERT(GetForwarder(), "Can't create a texture client if the compositable is not connected to the compositor.");
  LayersBackend parentBackend = GetForwarder()->GetCompositorBackendType();
  RefPtr<DeprecatedTextureClient> result;

  switch (aDeprecatedTextureClientType) {
  case TEXTURE_SHARED_GL:
  case TEXTURE_SHARED_GL_EXTERNAL:
  case TEXTURE_STREAM_GL:
    MOZ_CRASH("Unsupported. this should not be reached");
  case TEXTURE_YCBCR:
    if (parentBackend == LayersBackend::LAYERS_D3D9 ||
        parentBackend == LayersBackend::LAYERS_D3D11 ||
        parentBackend == LayersBackend::LAYERS_BASIC) {
      result = new DeprecatedTextureClientShmemYCbCr(GetForwarder(), GetTextureInfo());
    } else {
      MOZ_CRASH("Unsupported. this should not be reached");
    }
    break;
  case TEXTURE_CONTENT:
#ifdef XP_WIN
    if (parentBackend == LayersBackend::LAYERS_D3D11 && gfxWindowsPlatform::GetPlatform()->GetD2DDevice()) {
      result = new DeprecatedTextureClientD3D11(GetForwarder(), GetTextureInfo());
      break;
    }
    if (parentBackend == LayersBackend::LAYERS_D3D9 &&
        GetForwarder()->IsSameProcess()) {
      // We can't use a d3d9 texture for an RGBA surface because we cannot get a DC for
      // for a gfxWindowsSurface.
      // We have to wait for the compositor thread to create a d3d9 device before we
      // can create d3d9 textures on the main thread (because we need to reset on the
      // compositor thread, and the d3d9 device must be reset on the same thread it was
      // created on).
      if (aContentType == gfxContentType::COLOR_ALPHA ||
          !gfxWindowsPlatform::GetPlatform()->GetD3D9Device()) {
        result = new DeprecatedTextureClientDIB(GetForwarder(), GetTextureInfo());
      } else {
        result = new DeprecatedTextureClientD3D9(GetForwarder(), GetTextureInfo());
      }
      break;
    }
#endif
     // fall through to TEXTURE_SHMEM
  case TEXTURE_SHMEM:
    result = new DeprecatedTextureClientShmem(GetForwarder(), GetTextureInfo());
    break;
  case TEXTURE_FALLBACK:
#ifdef XP_WIN
    if (parentBackend == LayersBackend::LAYERS_D3D11 ||
        parentBackend == LayersBackend::LAYERS_D3D9) {
      result = new DeprecatedTextureClientShmem(GetForwarder(), GetTextureInfo());
    }
#endif
    break;
  default:
    MOZ_ASSERT(false, "Unhandled texture client type");
  }

  // If we couldn't create an appropriate texture client,
  // then return nullptr so the caller can chose another
  // type.
  if (!result) {
    return nullptr;
  }

  MOZ_ASSERT(result->SupportsType(aDeprecatedTextureClientType),
             "Created the wrong texture client?");
  result->SetFlags(GetTextureInfo().mTextureFlags);

  return result.forget();
}

TemporaryRef<BufferTextureClient>
CompositableClient::CreateBufferTextureClient(SurfaceFormat aFormat,
                                              TextureFlags aTextureFlags)
{
  return TextureClient::CreateBufferTextureClient(GetForwarder(), aFormat,
                                                  aTextureFlags | mTextureFlags);
}

TemporaryRef<TextureClient>
CompositableClient::CreateTextureClientForDrawing(SurfaceFormat aFormat,
                                                  TextureFlags aTextureFlags,
                                                  const IntSize& aSizeHint)
{
  return TextureClient::CreateTextureClientForDrawing(GetForwarder(), aFormat,
                                                      aTextureFlags | mTextureFlags,
                                                      aSizeHint);
}

bool
CompositableClient::AddTextureClient(TextureClient* aClient)
{
  return aClient->InitIPDLActor(mForwarder);
}

void
CompositableClient::OnTransaction()
{
}

void
CompositableClient::UseTexture(TextureClient* aTexture)
{
  MOZ_ASSERT(aTexture);
  if (!aTexture) {
    return;
  }

#if defined(MOZ_WIDGET_GONK) && ANDROID_VERSION >= 17
  FenceHandle handle = aTexture->GetAcquireFenceHandle();
  if (handle.IsValid()) {
    RefPtr<FenceDeliveryTracker> tracker = new FenceDeliveryTracker(handle);
    mForwarder->SendFenceHandle(tracker, aTexture->GetIPDLActor(), handle);
  }
#endif
  mForwarder->UseTexture(this, aTexture);
}

} // namespace layers
} // namespace mozilla
