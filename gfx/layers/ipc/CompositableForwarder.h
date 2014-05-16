/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_LAYERS_COMPOSITABLEFORWARDER
#define MOZILLA_LAYERS_COMPOSITABLEFORWARDER

#include <stdint.h>                     // for int32_t, uint64_t
#include "gfxTypes.h"
#include "mozilla/Attributes.h"         // for MOZ_OVERRIDE
#include "mozilla/layers/CompositorTypes.h"
#include "mozilla/layers/ISurfaceAllocator.h"  // for ISurfaceAllocator
#include "mozilla/layers/LayersTypes.h"  // for LayersBackend
#include "mozilla/layers/TextureClient.h"  // for TextureClient
#include "nsRegion.h"                   // for nsIntRegion

struct nsIntPoint;
struct nsIntRect;

namespace mozilla {
namespace layers {

class CompositableClient;
class AsyncTransactionTracker;
class TextureFactoryIdentifier;
class SurfaceDescriptor;
class SurfaceDescriptorTiles;
class ThebesBufferData;
class DeprecatedTextureClient;
class ClientTiledLayerBuffer;
class PTextureChild;

/**
 * A transaction is a set of changes that happenned on the content side, that
 * should be sent to the compositor side.
 * CompositableForwarder is an interface to manage a transaction of
 * compositable objetcs.
 *
 * ShadowLayerForwarder is an example of a CompositableForwarder (that can
 * additionally forward modifications of the Layer tree).
 * ImageBridgeChild is another CompositableForwarder.
 */
class CompositableForwarder : public ISurfaceAllocator
{
  friend class AutoOpenSurface;
  friend class DeprecatedTextureClientShmem;
public:

  CompositableForwarder()
    : mSerial(++sSerialCounter)
  {}

  /**
   * Setup the IPDL actor for aCompositable to be part of layers
   * transactions.
   */
  virtual void Connect(CompositableClient* aCompositable) = 0;

  /**
   * When using the Thebes layer pattern of swapping or updating
   * TextureClient/Host pairs without sending SurfaceDescriptors,
   * use these messages to assign the single or double buffer
   * (TextureClient/Host pairs) to the CompositableHost.
   * We expect the textures to already have been created.
   * With these messages, the ownership of the SurfaceDescriptor(s)
   * moves to the compositor.
   */
  virtual void CreatedSingleBuffer(CompositableClient* aCompositable,
                                   const SurfaceDescriptor& aDescriptor,
                                   const TextureInfo& aTextureInfo,
                                   const SurfaceDescriptor* aDescriptorOnWhite = nullptr) = 0;
  virtual void CreatedDoubleBuffer(CompositableClient* aCompositable,
                                   const SurfaceDescriptor& aFrontDescriptor,
                                   const SurfaceDescriptor& aBackDescriptor,
                                   const TextureInfo& aTextureInfo,
                                   const SurfaceDescriptor* aFrontDescriptorOnWhite = nullptr,
                                   const SurfaceDescriptor* aBackDescriptorOnWhite = nullptr) = 0;

  /**
   * Notify the CompositableHost that it should create host-side-only
   * texture(s), that we will update incrementally using UpdateTextureIncremental.
   */
  virtual void CreatedIncrementalBuffer(CompositableClient* aCompositable,
                                        const TextureInfo& aTextureInfo,
                                        const nsIntRect& aBufferRect) = 0;

  /**
   * Tell the compositor that a Compositable is killing its buffer(s),
   * that is TextureClient/Hosts.
   */
  virtual void DestroyThebesBuffer(CompositableClient* aCompositable) = 0;

  /**
   * Tell the CompositableHost on the compositor side what TiledLayerBuffer to
   * use for the next composition.
   */
  virtual void UseTiledLayerBuffer(CompositableClient* aCompositable,
                                   const SurfaceDescriptorTiles& aTiledDescriptor) = 0;

  /**
   * Create a TextureChild/Parent pair as as well as the TextureHost on the parent side.
   */
  virtual PTextureChild* CreateTexture(const SurfaceDescriptor& aSharedData, TextureFlags aFlags) = 0;

  /**
   * Communicate to the compositor that the texture identified by aCompositable
   * and aTextureId has been updated to aImage.
   */
  virtual void UpdateTexture(CompositableClient* aCompositable,
                             TextureIdentifier aTextureId,
                             SurfaceDescriptor* aDescriptor) = 0;

  /**
   * Same as UpdateTexture, but performs an asynchronous layer transaction (if possible)
   */
  virtual void UpdateTextureNoSwap(CompositableClient* aCompositable,
                                   TextureIdentifier aTextureId,
                                   SurfaceDescriptor* aDescriptor) = 0;

  /**
   * Communicate to the compositor that aRegion in the texture identified by
   * aCompositable and aIdentifier has been updated to aThebesBuffer.
   */
  virtual void UpdateTextureRegion(CompositableClient* aCompositable,
                                   const ThebesBufferData& aThebesBufferData,
                                   const nsIntRegion& aUpdatedRegion) = 0;

  /**
   * Notify the compositor to update aTextureId using aDescriptor, and take
   * ownership of aDescriptor.
   *
   * aDescriptor only contains the pixels for aUpdatedRegion, and is relative
   * to aUpdatedRegion.TopLeft().
   *
   * aBufferRect/aBufferRotation define the new valid region contained
   * within the texture after the update has been applied.
   */
  virtual void UpdateTextureIncremental(CompositableClient* aCompositable,
                                        TextureIdentifier aTextureId,
                                        SurfaceDescriptor& aDescriptor,
                                        const nsIntRegion& aUpdatedRegion,
                                        const nsIntRect& aBufferRect,
                                        const nsIntPoint& aBufferRotation) = 0;

  /**
   * Communicate the picture rect of a YUV image in aLayer to the compositor
   */
  virtual void UpdatePictureRect(CompositableClient* aCompositable,
                                 const nsIntRect& aRect) = 0;

  /**
   * The specified layer is destroying its buffers.
   * |aBackBufferToDestroy| is deallocated when this transaction is
   * posted to the parent.  During the parent-side transaction, the
   * shadow is told to destroy its front buffer.  This can happen when
   * a new front/back buffer pair have been created because of a layer
   * resize, e.g.
   */
  virtual void DestroyedThebesBuffer(const SurfaceDescriptor& aBackBufferToDestroy) = 0;

  /**
   * Tell the CompositableHost on the compositor side to remove the texture
   * from the CompositableHost.
   * This function does not delete the TextureHost corresponding to the
   * TextureClient passed in parameter.
   * When the TextureClient has TEXTURE_DEALLOCATE_CLIENT flag,
   * the transaction becomes synchronous.
   */
  virtual void RemoveTextureFromCompositable(CompositableClient* aCompositable,
                                             TextureClient* aTexture) = 0;

  /**
   * Tell the CompositableHost on the compositor side to remove the texture
   * from the CompositableHost. The compositor side sends back transaction
   * complete message.
   * This function does not delete the TextureHost corresponding to the
   * TextureClient passed in parameter.
   * It is used when the TextureClient recycled.
   * Only ImageBridge implements it.
   */
  virtual void RemoveTextureFromCompositableAsync(AsyncTransactionTracker* aAsyncTransactionTracker,
                                                  CompositableClient* aCompositable,
                                                  TextureClient* aTexture) {}

  /**
   * Tell the compositor side to delete the TextureHost corresponding to the
   * TextureClient passed in parameter.
   */
  virtual void RemoveTexture(TextureClient* aTexture) = 0;

  /**
   * Holds a reference to a TextureClient until after the next
   * compositor transaction, and then drops it.
   */
  virtual void HoldUntilTransaction(TextureClient* aClient)
  {
    if (aClient) {
      mTexturesToRemove.AppendElement(aClient);
    }
  }

  /**
   * Forcibly remove texture data from TextureClient
   * This function needs to be called after a tansaction with Compositor.
   */
  virtual void RemoveTexturesIfNecessary()
  {
    mTexturesToRemove.Clear();
  }

  virtual void HoldTransactionsToRespond(uint64_t aTransactionId)
  {
    mTransactionsToRespond.push_back(aTransactionId);
  }

  virtual void ClearTransactionsToRespond()
  {
    mTransactionsToRespond.clear();
  }

  /**
   * Tell the CompositableHost on the compositor side what texture to use for
   * the next composition.
   */
  virtual void UseTexture(CompositableClient* aCompositable,
                          TextureClient* aClient) = 0;
  virtual void UseComponentAlphaTextures(CompositableClient* aCompositable,
                                         TextureClient* aClientOnBlack,
                                         TextureClient* aClientOnWhite) = 0;

  /**
   * Tell the compositor side that the shared data has been modified so that
   * it can react accordingly (upload textures, etc.).
   */
  virtual void UpdatedTexture(CompositableClient* aCompositable,
                              TextureClient* aTexture,
                              nsIntRegion* aRegion) = 0;

  void IdentifyTextureHost(const TextureFactoryIdentifier& aIdentifier);

  virtual int32_t GetMaxTextureSize() const MOZ_OVERRIDE
  {
    return mTextureFactoryIdentifier.mMaxTextureSize;
  }

  bool IsOnCompositorSide() const MOZ_OVERRIDE { return false; }

  virtual bool IsImageBridgeChild() const { return false; }

  /**
   * Returns the type of backend that is used off the main thread.
   * We only don't allow changing the backend type at runtime so this value can
   * be queried once and will not change until Gecko is restarted.
   */
  virtual LayersBackend GetCompositorBackendType() const MOZ_OVERRIDE
  {
    return mTextureFactoryIdentifier.mParentBackend;
  }

  bool SupportsTextureBlitting() const
  {
    return mTextureFactoryIdentifier.mSupportsTextureBlitting;
  }

  bool SupportsPartialUploads() const
  {
    return mTextureFactoryIdentifier.mSupportsPartialUploads;
  }

  const TextureFactoryIdentifier& GetTextureFactoryIdentifier() const
  {
    return mTextureFactoryIdentifier;
  }

  int32_t GetSerial() { return mSerial; }

protected:
  TextureFactoryIdentifier mTextureFactoryIdentifier;
  nsTArray<RefPtr<TextureClient> > mTexturesToRemove;
  std::vector<uint64_t> mTransactionsToRespond;
  const int32_t mSerial;
  static mozilla::Atomic<int32_t> sSerialCounter;
};

} // namespace
} // namespace

#endif
