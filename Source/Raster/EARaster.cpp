/*
Copyright (C) 2009-2010 Electronic Arts, Inc.  All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1.  Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
2.  Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
3.  Neither the name of Electronic Arts, Inc. ("EA") nor the names of
    its contributors may be used to endorse or promote products derived
    from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY ELECTRONIC ARTS AND ITS CONTRIBUTORS "AS IS" AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL ELECTRONIC ARTS OR ITS CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

///////////////////////////////////////////////////////////////////////////////
// EARaster.cpp  (ReCap clean-room port — WebCore-free)
// Original by Paul Pedriana
//
// Scope: the concrete EA::Raster::ISurface (32-bit ARGB pixel buffer) plus the
// CreateSurface/DestroySurface factory and the color/pixel helpers the View ABI
// needs. Software blit, primitives (lines/rects/polys), and resampling are NOT
// ported — mb renders into the surface's pixel buffer directly. Those paths are
// stubbed in EARasterConcrete so the ABI vtable stays complete and linkable.
///////////////////////////////////////////////////////////////////////////////


#include <EARaster/EARaster.h>
#include <EARaster/EARasterColor.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>

// ReCap: replaced EAWebKit's allocator/assert macros (EAWEBKIT_NEW/DELETE, EAW_ASSERT,
//        EAW_FAIL_MSG) with plain operator new[]/delete[] and <assert.h> — those macros
//        pull in EAWebKit internal headers we don't ship in the clean-room ABI.
#include <assert.h>
#define EAW_ASSERT(expr) assert(expr)


namespace EA {

namespace Raster {


///////////////////////////////////////////////////////////////////////
// Surface
///////////////////////////////////////////////////////////////////////


Surface::Surface()
{
    InitMembers();
    SetPixelFormat(kPixelFormatTypeARGB);
}


Surface::Surface(int width, int height, PixelFormatType pft)
{
    InitMembers();
    SetPixelFormat(pft);
    Resize(width, height, false);
    // The user can check mpData to see if the resize succeeded.
}


Surface::~Surface()
{
    FreeData();
}


int Surface::AddRef()
{
    // This is not thread-safe.
    return ++mRefCount;
}


int Surface::Release()
{
    // This is not thread-safe.
    if(mRefCount > 1)
        return --mRefCount;

    DestroySurface(this);
    return 0;
}


void Surface::InitMembers()
{
    // mPixelFormat  // Should probably init this.
    mSurfaceFlags  = 0;
    mpData         = NULL;
    mWidth         = 0;
    mHeight        = 0;
    mStride        = 0;
    mLockCount     = 0;
    mRefCount      = 0;
    mpUserData     = NULL;
    mCompressedSize = 0;

    // Draw info.
    mClipRect.x    = 0;
    mClipRect.y    = 0;
    mClipRect.w    = INT_MAX;
    mClipRect.h    = INT_MAX;
    mpBlitDest     = 0;
    mDrawFlags     = 0;
    mpBlitFunction = NULL;
    mCategory      = kSurfaceCategoryDefault;
}


void Surface::SetPixelFormat(PixelFormatType pft)
{
    mPixelFormat.mPixelFormatType = pft;
    mPixelFormat.mSurfaceAlpha    = 255;

    if (pft == kPixelFormatTypeRGB)
        mPixelFormat.mBytesPerPixel = 3;
    else
        mPixelFormat.mBytesPerPixel = 4;

    switch (pft)
    {
        case kPixelFormatTypeARGB:
            mPixelFormat.mRMask  = 0x00ff0000;
            mPixelFormat.mGMask  = 0x0000ff00;
            mPixelFormat.mBMask  = 0x000000ff;
            mPixelFormat.mAMask  = 0xff000000;
            mPixelFormat.mRShift = 16;
            mPixelFormat.mGShift = 8;
            mPixelFormat.mBShift = 0;
            mPixelFormat.mAShift = 24;
            break;

        case kPixelFormatTypeRGBA:
            mPixelFormat.mRMask  = 0xff000000;
            mPixelFormat.mGMask  = 0x00ff0000;
            mPixelFormat.mBMask  = 0x0000ff00;
            mPixelFormat.mAMask  = 0x000000ff;
            mPixelFormat.mRShift = 24;
            mPixelFormat.mGShift = 16;
            mPixelFormat.mBShift = 8;
            mPixelFormat.mAShift = 0;
            break;

        case kPixelFornatTypeXRGB:
            mPixelFormat.mRMask  = 0x00ff0000;
            mPixelFormat.mGMask  = 0x0000ff00;
            mPixelFormat.mBMask  = 0x000000ff;
            mPixelFormat.mAMask  = 0x00000000;
            mPixelFormat.mRShift = 16;
            mPixelFormat.mGShift = 8;
            mPixelFormat.mBShift = 0;
            mPixelFormat.mAShift = 24;
            break;

        case kPixelFormatTypeRGBX:
            mPixelFormat.mRMask  = 0xff000000;
            mPixelFormat.mGMask  = 0x00ff0000;
            mPixelFormat.mBMask  = 0x0000ff00;
            mPixelFormat.mAMask  = 0x00000000;
            mPixelFormat.mRShift = 24;
            mPixelFormat.mGShift = 16;
            mPixelFormat.mBShift = 8;
            mPixelFormat.mAShift = 0;
            break;

        case kPixelFormatTypeRGB:
            mPixelFormat.mRMask  = 0xff000000;  // I think this is wrong, or at least endian-dependent.
            mPixelFormat.mGMask  = 0x00ff0000;
            mPixelFormat.mBMask  = 0x0000ff00;
            mPixelFormat.mAMask  = 0x00000000;
            mPixelFormat.mRShift = 24;
            mPixelFormat.mGShift = 16;
            mPixelFormat.mBShift = 8;
            mPixelFormat.mAShift = 0;
            break;
    }
}

bool Surface::Set(void* pData, int width, int height, int stride, PixelFormatType pft, bool bCopyData, bool bTakeOwnership, SurfaceCategory category)
{
    FreeData();

    mpBlitDest     = 0;
    mDrawFlags     = 0;
    mpBlitFunction = NULL;

    if(pData)
    {
        SetPixelFormat(pft);
        SetCategory(category);

        if(bCopyData)  // If we are copying the data instead of taking over ownership...
        {
            if(Resize(width, height, false))
            {
                Lock();
                if(mStride == stride)
                {
                    // We could blit pSource to ourself, but we happen to know we are of identical
                    // format and we simply want to replicate source onto ourselves. So memcpy it.
                    memcpy(mpData, pData, height * stride);
                }
                else
                {
                    // Case where the source and dest buffers have different strides so line copy instead
                    char* pDestRow = (char*) mpData;
                    char* pSourceRow = (char*) pData;
                    int lineSize = width * GetPixelFormat().mBytesPerPixel;

                    for(int h=0; h < height; h++)
                    {
                         memcpy(pDestRow, pSourceRow, lineSize);
                         pDestRow +=mStride;
                         pSourceRow +=stride;
                    }
                }
                Unlock();
            }
            else
                return false;
        }
        else
        {
            mpData  = pData;
            mWidth  = width;
            mHeight = height;
            mStride = stride;

            if(!bTakeOwnership)
                mSurfaceFlags |= kFlagOtherOwner;
        }
    }

    return true;
}


bool Surface::Set(ISurface* pSource)
{
    if(Resize(pSource->GetWidth(), pSource->GetHeight(), false))
    {
        int stride = pSource->GetStride();
        Lock();
        if(mStride == stride)
        {
            // We could blit pSource to ourself, but we happen to know we are of identical
            // format and we simply want to replicate source onto ourselves. So memcpy it.
            memcpy(mpData, pSource->GetData(), pSource->GetHeight() * stride);
        }
        else
        {
            // Case where the source and dest buffers have different strides so line copy instead
            char* pDestRow = (char*) mpData;
            char* pSourceRow = (char*) pSource->GetData();
            int lineSize = pSource->GetWidth() * GetPixelFormat().mBytesPerPixel;
            int height = pSource->GetHeight();
            for(int h=0; h < height; h++)
            {
                 memcpy(pDestRow, pSourceRow, lineSize);
                 pDestRow +=mStride;
                 pSourceRow +=stride;
            }
        }
        Unlock();
        return true;
    }

    return false;
}


bool Surface::Resize(int width, int height, bool bCopyData)
{
    const size_t kNewMemorySize = (size_t)width * height * mPixelFormat.mBytesPerPixel;

    if(bCopyData && mpData)
    {
        // ReCap: plain new[] in place of EAWEBKIT_NEW. The original left the actual
        //        old->new copy as a TODO (EAW_FAIL_MSG); the View only ever resizes a
        //        fresh surface, so the copy path is unused. We allocate and clear, then
        //        drop the old buffer — same observable result as the original stub.
        void* pData = new char[kNewMemorySize];
        if(pData)
            memset(pData, 0, kNewMemorySize);
        FreeData();
        mpData = pData;
    }
    else
    {
        // We have a separate pathway for this case because it uses less total memory than above.
        FreeData();

        mpData = new char[kNewMemorySize];

        if(mpData)
            memset(mpData, 0, kNewMemorySize);
    }

    if(mpData)
    {
        mWidth  = width;
        mHeight = height;
        mStride = width * mPixelFormat.mBytesPerPixel;

        // Reset the clip rect on resize
        Rect r (0,0,width,height);
        SetClipRect(r);
    }

    return (mpData != NULL);
}


bool Surface::FreeData()
{
    bool returnFlag = false;

    // Remove the compressed data buffer which is stored in the user data
    if( ((mSurfaceFlags & (EA::Raster::kFlagCompressedRLE | EA::Raster::kFlagCompressedYCOCGDXT5 )) != 0 ) &&
        (mpUserData) && (mCompressedSize) )
    {
         delete[] ((char*)mpUserData);
         mpUserData = NULL;
         mCompressedSize = 0;
         mSurfaceFlags &= ~(EA::Raster::kFlagCompressedRLE | EA::Raster::kFlagCompressedYCOCGDXT5);
         returnFlag = true;  // Success
    }

    if((mSurfaceFlags & kFlagOtherOwner) == 0)  // If we own the pointer...
    {
        delete[] ((char*)mpData);
        mpData = NULL;

        // We only want to clear this stuff if there is no other owner for the other owner might still need this info.
        mLockCount    = 0;
        mSurfaceFlags = 0;
        returnFlag = true;  // Success
    }

    return returnFlag;
}

// Can be used for texture lock notification
void Surface::Lock()
{
    mLockCount++;
}

void Surface::Unlock()
{
    EAW_ASSERT(mLockCount > 0);
    mLockCount--;
}

void Surface::SetClipRect(const Rect* pRect)
{
	Rect fullRect(0, 0, mWidth, mHeight);

	if(pRect)
	    IntersectRect(*pRect, fullRect, mClipRect);
    else
    	mClipRect = fullRect;
}


ISurface* CreateSurface()
{
    ISurface* pSurface = new Surface();

    if(pSurface)
        pSurface->AddRef();

    return pSurface;
}


ISurface* CreateSurface(int width, int height, PixelFormatType pft,SurfaceCategory category)
{
    ISurface* pNewSurface = CreateSurface();

    if(pNewSurface)
    {
        // Note that pNewSurface is already AddRef'd.
        pNewSurface->SetPixelFormat(pft);
        pNewSurface->SetCategory(category);
        if(!pNewSurface->Resize(width, height, false))
        {
            DestroySurface(pNewSurface);
            pNewSurface = NULL;
        }
    }

    return pNewSurface;
}


ISurface* CreateSurface(ISurface* pSurface)
{
    ISurface* pNewSurface = CreateSurface();

    if(pNewSurface)
    {
        // Note that pNewSurface is already AddRef'd.
        if(!pNewSurface->Set(pSurface))
        {
            DestroySurface(pNewSurface);
            pNewSurface = NULL;
        }
    }

    return pNewSurface;
}


ISurface* CreateSurface(void* pData, int width, int height, int stride, PixelFormatType pft, bool bCopyData, bool bTakeOwnership,SurfaceCategory category)
{
    ISurface* pNewSurface = CreateSurface();

    if(pNewSurface)
    {
        // Note that pNewSurface is already AddRef'd.
        if(!pNewSurface->Set(pData, width, height, stride, pft, bCopyData, bTakeOwnership, category))
        {
            DestroySurface(pNewSurface);
            pNewSurface = NULL;
        }
    }

    return pNewSurface;
}


void DestroySurface(ISurface* pSurface)
{
    delete pSurface;
}


///////////////////////////////////////////////////////////////////////
// Color / pixel helpers
//
// ReCap: these are the small, self-contained conversions the surface
// lifecycle + EARasterConcrete need. The originals lived in EARaster's
// (un-ported) primitives translation unit; reimplemented here from the
// pixel-format masks/shifts so this file links standalone.
///////////////////////////////////////////////////////////////////////

void ConvertColor(NativeColor c, const PixelFormat& pf, int& r, int& g, int& b, int& a)
{
    r = (int)((c & pf.mRMask) >> pf.mRShift);
    g = (int)((c & pf.mGMask) >> pf.mGShift);
    b = (int)((c & pf.mBMask) >> pf.mBShift);
    a = pf.mAMask ? (int)((c & pf.mAMask) >> pf.mAShift) : 255;
}

void ConvertColor(NativeColor c, const PixelFormat& pf, int& r, int& g, int& b)
{
    int a;
    ConvertColor(c, pf, r, g, b, a);
}

void ConvertColor(NativeColor c, PixelFormatType cFormat, Color& result)
{
    PixelFormat pf;
    Surface tmp;
    tmp.SetPixelFormat(cFormat);
    pf = tmp.GetPixelFormat();

    int r, g, b, a;
    ConvertColor(c, pf, r, g, b, a);
    result.setRGBA(r, g, b, a);
}

void ConvertColor(int r, int g, int b, int a, Color& result)
{
    result.setRGBA(r, g, b, a);
}

void ConvertColor(const Color& c, int& r, int& g, int& b, int& a)
{
    r = c.red();
    g = c.green();
    b = c.blue();
    a = c.alpha();
}

NativeColor ConvertColor(const Color& c, PixelFormatType resultFormat)
{
    Surface tmp;
    tmp.SetPixelFormat(resultFormat);
    const PixelFormat& pf = tmp.GetPixelFormat();

    NativeColor nc = 0;
    nc |= ((NativeColor)c.red()   << pf.mRShift) & pf.mRMask;
    nc |= ((NativeColor)c.green() << pf.mGShift) & pf.mGMask;
    nc |= ((NativeColor)c.blue()  << pf.mBShift) & pf.mBMask;
    if(pf.mAMask)
        nc |= ((NativeColor)c.alpha() << pf.mAShift) & pf.mAMask;
    return nc;
}


void GetPixel(ISurface* pSurface, int x, int y, Color& color)
{
    if(!pSurface || !pSurface->GetData())
    {
        color.setRGBA(0, 0, 0, 0);
        return;
    }

    const PixelFormat& pf = pSurface->GetPixelFormat();
    const uint8_t* pRow   = (const uint8_t*)pSurface->GetData() + (size_t)y * pSurface->GetStride();
    const uint8_t* pPixel = pRow + (size_t)x * pf.mBytesPerPixel;

    NativeColor c;
    if(pf.mBytesPerPixel == 3)
        c = (NativeColor)(pPixel[0]) | ((NativeColor)pPixel[1] << 8) | ((NativeColor)pPixel[2] << 16);
    else
        c = *(const NativeColor*)pPixel;

    int r, g, b, a;
    ConvertColor(c, pf, r, g, b, a);
    color.setRGBA(r, g, b, a);
}


int SetPixelSolidColorNoClip(ISurface* pSurface, int x, int y, const Color& color)
{
    if(!pSurface || !pSurface->GetData())
        return -1;

    const PixelFormat& pf = pSurface->GetPixelFormat();
    uint8_t* pRow   = (uint8_t*)pSurface->GetData() + (size_t)y * pSurface->GetStride();
    uint8_t* pPixel = pRow + (size_t)x * pf.mBytesPerPixel;

    NativeColor c = ConvertColor(color, pf.mPixelFormatType);

    if(pf.mBytesPerPixel == 3)
    {
        pPixel[0] = (uint8_t)(c & 0xff);
        pPixel[1] = (uint8_t)((c >> 8) & 0xff);
        pPixel[2] = (uint8_t)((c >> 16) & 0xff);
    }
    else
        *(NativeColor*)pPixel = c;

    return 0;
}


int SetPixelSolidColor(ISurface* pSurface, int x, int y, const Color& color)
{
    if(!pSurface)
        return -1;

    const Rect& clip = pSurface->GetClipRect();
    if(x < clip.x || x >= clip.x + clip.w || y < clip.y || y >= clip.y + clip.h)
        return 0;

    return SetPixelSolidColorNoClip(pSurface, x, y, color);
}


///////////////////////////////////////////////////////////////////////
// Utility functions
///////////////////////////////////////////////////////////////////////

EARASTER_API bool IntersectRect(const Rect& a, const Rect& b, Rect& result)
{
    // Horizontal
    int aMin = a.x;
    int aMax = aMin + a.w;
    int bMin = b.x;
    int bMax = bMin + b.w;

    if(bMin > aMin)
        aMin = bMin;
    result.x = aMin;
    if(bMax < aMax)
        aMax = bMax;
    result.w = (((aMax - aMin) > 0) ? (aMax - aMin) : 0);

    // Vertical
    aMin = a.y;
    aMax = aMin + a.h;
    bMin = b.y;
    bMax = bMin + b.h;

    if(bMin > aMin)
        aMin = bMin;
    result.y = aMin;
    if(bMax < aMax)
        aMax = bMax;
    result.h = (((aMax - aMin) > 0) ? (aMax - aMin) : 0);

    return (result.w && result.h);
}


EARASTER_API bool WritePPMFile(const char* pPath, ISurface* pSurface, bool bAlphaOnly)
{
    FILE* const fp = fopen(pPath, "w");

    if(fp)
    {
        const bool bARGB = (pSurface->GetPixelFormat().mPixelFormatType == EA::Raster::kPixelFormatTypeARGB);

        fprintf(fp, "P3\n");
        fprintf(fp, "# %s\n", pPath);
        fprintf(fp, "%d %d\n", (int)pSurface->GetWidth(), (int)pSurface->GetHeight());
        fprintf(fp, "%d\n", 255);

        for(int y = 0; y < pSurface->GetHeight(); y++)
        {
            for(int x = 0; x < pSurface->GetWidth(); x++)
            {
                EA::Raster::Color color; EA::Raster::GetPixel(pSurface, x, y, color);
                const uint32_t    c = color.rgb();
                unsigned          a, r, g, b;

                if(bAlphaOnly)
                {
                    if(bARGB)
                        a = (unsigned)((c >> 24) & 0xff);  // ARGB
                    else
                        a = (unsigned)(c & 0xff);          // RGBA

                    fprintf(fp, "%03u %03u %03u \t", a, a, a);
                }
                else
                {
                    if(bARGB)
                    {
                        r = (unsigned)((c >> 16) & 0xff); // ARGB
                        g = (unsigned)((c >>  8) & 0xff);
                        b = (unsigned)((c >>  0) & 0xff);
                    }
                    else
                    {
                        r = (unsigned)((c >> 24) & 0xff); // RGBA
                        g = (unsigned)((c >> 16) & 0xff);
                        b = (unsigned)((c >>  8) & 0xff);
                    }

                    fprintf(fp, "%03u %03u %03u \t", r, g, b);
                }
            }

            fprintf(fp, "\n");
        }

        fprintf(fp, "\n");
        fclose(fp);

        return true;
    }

    return false;
}


// ReCap: stubbed — IntRect<->EARect conversions are the sole WebCore coupling in the
//        original (WKAL::IntRect / WebCore::IntRect). The clean-room ABI forward-declares
//        WKAL::IntRect as an incomplete type, so we can neither read .x()/.y() nor
//        construct one. No first-party code calls these; they exist only to satisfy the
//        exported symbols. They are no-ops (out left untouched / zeroed).
void IntRectToEARect(const WKAL::IntRect& /*in*/, EA::Raster::Rect& out)
{
    out = EA::Raster::Rect(0, 0, 0, 0);
}

void EARectToIntRect(const EA::Raster::Rect& /*in*/, WKAL::IntRect& /*out*/)
{
    // ReCap: stubbed — cannot construct WebCore::IntRect without its (excluded) header.
}

} // namespace Raster

} // namespace EA


///////////////////////////////////////////////////////////////////////
// EARasterConcrete — the exported IEARaster implementation
//
// Surface lifecycle + color/pixel + IntersectRect/WritePPMFile forward to the
// real implementations above. All drawing (lines/rects/polys/circles), blitting,
// and resampling are STUBBED:
//   - int-returning draw/blit calls return -1 (the original's "not implemented"
//     convention, as already used for Textured* in the source).
//   - surface-returning resample calls return NULL.
// ReCap: stubbed — mb renders pixels straight into the surface buffer, so EAWebKit's
//        software rasterizer (EARasterBlit/Primitives, intentionally not ported) is
//        never exercised. Port those TUs if a future path needs CPU drawing.
///////////////////////////////////////////////////////////////////////

namespace EA
{
	namespace Raster
	{
		void EARasterConcrete::IntRectToEARect(const WKAL::IntRect& in, EA::Raster::Rect& out)
		 {
			 EA::Raster::IntRectToEARect(in, out);
		 }
		 void EARasterConcrete::EARectToIntRect(const EA::Raster::Rect& in, WKAL::IntRect& out)
		 {
			EA::Raster::EARectToIntRect(in, out);
		 }
		// Surface management
		 ISurface* EARasterConcrete::CreateSurface()
		 {
			 return EA::Raster::CreateSurface();
		 }
		 ISurface* EARasterConcrete::CreateSurface(int width, int height, PixelFormatType pft,SurfaceCategory category)
		 {
			 return EA::Raster::CreateSurface(width, height, pft, category);
		 }
		 ISurface*    EARasterConcrete::CreateSurface(ISurface* pSurface)
		 {
			 return EA::Raster::CreateSurface(pSurface);
		 }
		 ISurface* EARasterConcrete::CreateSurface(void* pData, int width, int height, int stride, PixelFormatType pft, bool bCopyData, bool bTakeOwnership, SurfaceCategory category)
		 {
			 return EA::Raster::CreateSurface(pData, width, height, stride, pft, bCopyData, bTakeOwnership, category);
		 }
		 void EARasterConcrete::DestroySurface(ISurface* pSurface)
		 {
			 EA::Raster::DestroySurface(pSurface);
		 }

		// Color conversion
		 void EARasterConcrete::ConvertColor(NativeColor c, PixelFormatType cFormat, Color& result)
		 {
			 EA::Raster::ConvertColor(c, cFormat, result);
		 }
		 void EARasterConcrete::ConvertColor(int r, int g, int b, int a, Color& result)
		 {
			 EA::Raster::ConvertColor(r, g, b, a, result);
		 }
		 NativeColor EARasterConcrete::ConvertColor(const Color& c, PixelFormatType resultFormat)
		 {
			 return EA::Raster::ConvertColor(c, resultFormat);
		 }
		 void EARasterConcrete::ConvertColor(const Color& c, int& r, int& g, int& b, int& a)
		 {
			 EA::Raster::ConvertColor(c, r, g, b, a);
		 }
		 void EARasterConcrete::ConvertColor(NativeColor c, const PixelFormat& pf, int& r, int& g, int& b, int& a)
		 {
			 EA::Raster::ConvertColor(c, pf, r, g, b, a);
		 }
		 void EARasterConcrete::ConvertColor(NativeColor c, const PixelFormat& pf, int& r, int& g, int& b)
		 {
			 EA::Raster::ConvertColor(c, pf, r, g, b);
		 }

		// Pixel functions
		 void  EARasterConcrete::GetPixel(ISurface* pSurface, int x, int y, Color& color)
		 {
			 EA::Raster::GetPixel(pSurface, x, y, color);
		 }
		 int EARasterConcrete::SetPixelSolidColor(ISurface* pSurface, int x, int y, const Color& color)
		 {
			 return EA::Raster::SetPixelSolidColor(pSurface, x, y, color);
		 }
		 int EARasterConcrete::SetPixelSolidColorNoClip(ISurface* pSurface, int x, int y, const Color& color)
		 {
			 return EA::Raster::SetPixelSolidColorNoClip(pSurface, x, y, color);
		 }
		 int EARasterConcrete::SetPixelColor(ISurface* pSurface, int x, int y, const Color& color)
		 {
			 // ReCap: alpha-blended pixel set not ported — treat as solid (opaque) write.
			 return EA::Raster::SetPixelSolidColor(pSurface, x, y, color);
		 }
		 int EARasterConcrete::SetPixelColorNoClip(ISurface* pSurface, int x, int y, const Color& color)
		 {
			 return EA::Raster::SetPixelSolidColorNoClip(pSurface, x, y, color);
		 }
		 int EARasterConcrete::SetPixelRGBA(ISurface* pSurface, int x, int y, int r, int g, int b, int a)
		 {
			 return EA::Raster::SetPixelSolidColor(pSurface, x, y, Color(r, g, b, a));
		 }
		 int EARasterConcrete::SetPixelRGBANoClip(ISurface* pSurface, int x, int y, int r, int g, int b, int a)
		 {
			 return EA::Raster::SetPixelSolidColorNoClip(pSurface, x, y, Color(r, g, b, a));
		 }

		// Rectangle functions — ReCap: stubbed (software rasterizer not ported).
		 int EARasterConcrete::FillRectSolidColor(ISurface*, const Rect*, const Color&) { return -1; }
		 int EARasterConcrete::FillRectColor(ISurface*, const Rect*, const Color&) { return -1; }
		 int EARasterConcrete::RectangleColor(ISurface*, int, int, int, int, const Color&) { return -1; }
		 int EARasterConcrete::RectangleColor(ISurface*, const EA::Raster::Rect&, const Color&) { return -1; }
		 int EARasterConcrete::RectangleRGBA(ISurface*, int, int, int, int, int, int, int, int) { return -1; }

		// Line functions — ReCap: stubbed.
		 int EARasterConcrete::HLineSolidColor(ISurface*, int, int, int, const Color&) { return -1; }
		 int EARasterConcrete::HLineColor(ISurface*, int, int, int, const Color&) { return -1; }
		 int EARasterConcrete::VLineSolidColor(ISurface*, int, int, int, const Color&) { return -1; }
		 int EARasterConcrete::VLineColor(ISurface*, int, int, int, const Color&) { return -1; }
		 int EARasterConcrete::LineColor(ISurface*, int, int, int, int, const Color&) { return -1; }
		 int EARasterConcrete::LineRGBA(ISurface*, int, int, int, int, int, int, int, int) { return -1; }
		 int EARasterConcrete::AALineRGBA(ISurface*, int, int, int, int, int, int, int, int) { return -1; }

		// Circle / Ellipse — ReCap: stubbed.
		 int EARasterConcrete::CircleColor(ISurface*, int, int, int, const Color&) { return -1; }
		 int EARasterConcrete::CircleRGBA(ISurface*, int, int, int, int, int, int, int) { return -1; }
		 int EARasterConcrete::EllipseColor(ISurface*, int, int, int, int, const Color&) { return -1; }
		 int EARasterConcrete::EllipseRGBA(ISurface*, int, int, int, int, int, int, int, int) { return -1; }
		 int EARasterConcrete::AAEllipseColor(ISurface*, int, int, int, int, const Color&) { return -1; }
		 int EARasterConcrete::FilledEllipseColor(ISurface*, int, int, int, int, const Color&) { return -1; }
		 int EARasterConcrete::FilledEllipseRGBA(ISurface*, int, int, int, int, int, int, int, int) { return -1; }

		// Polygon — ReCap: stubbed.
		 int EARasterConcrete::SimpleTriangle(ISurface*, int, int, int, Orientation, const Color&) { return -1; }
		 int EARasterConcrete::PolygonColor(ISurface*, const int*, const int*, int, const Color&) { return -1; }
		 int EARasterConcrete::PolygonRGBA(ISurface*, const int*, const int*, int, int, int, int, int) { return -1; }
		 int EARasterConcrete::AAPolygonColor(ISurface*, const int*, const int*, int, const Color&) { return -1; }
		 int EARasterConcrete::AAPolygonRGBA(ISurface*, const int*, const int*, int, int, int, int, int) { return -1; }
		 int EARasterConcrete::FilledPolygonColor(ISurface*, const int*, const int*, int, const Color&) { return -1; }
		 int EARasterConcrete::FilledPolygonRGBA(ISurface*, const int*, const int*, int, int, int, int, int) { return -1; }
		 int EARasterConcrete::FilledPolygonColorMT(ISurface*, const int*, const int*, int, const Color&, int**, int*) { return -1; }
		 int EARasterConcrete::FilledPolygonRGBAMT(ISurface*, const int*, const int*, int, int, int, int, int, int**, int*) { return -1; }


#if UNUSED_IRASTER_CALLS_ENABLED
		// ReCap: stubbed — unused-calls block (only compiled when UNUSED_IRASTER_CALLS_ENABLED).
         int EARasterConcrete::HLineSolidRGBA(ISurface*, int, int, int, int, int, int, int) { return -1; }
         int EARasterConcrete::HLineRGBA(ISurface*, int, int, int, int, int, int, int) { return -1; }
         int EARasterConcrete::VLineSolidRGBA(ISurface*, int, int, int, int, int, int, int) { return -1; }
         int EARasterConcrete::VLineRGBA(ISurface*, int, int, int, int, int, int, int) { return -1; }
         int EARasterConcrete::AALineColor(ISurface*, int, int, int, int, const Color&, bool) { return -1; }
		 int EARasterConcrete::AALineColor(ISurface*, int, int, int, int, const Color&) { return -1; }
         int EARasterConcrete::ArcColor(ISurface*, int, int, int, int, int, const Color&) { return -1; }
		 int EARasterConcrete::ArcRGBA(ISurface*, int, int, int, int, int, int, int, int, int) { return -1; }
         int EARasterConcrete::AACircleColor(ISurface*, int, int, int, const Color&) { return -1; }
		 int EARasterConcrete::AACircleRGBA(ISurface*, int, int, int, int, int, int, int) { return -1; }
         int EARasterConcrete::FilledCircleColor(ISurface*, int, int, int, const Color&) { return -1; }
		 int EARasterConcrete::FilledCircleRGBA(ISurface*, int, int, int, int, int, int, int) { return -1; }
         int EARasterConcrete::AAEllipseRGBA(ISurface*, int, int, int, int, int, int, int, int) { return -1; }
         int EARasterConcrete::PieColor(ISurface*, int, int, int, int, int, const Color&) { return -1; }
		 int EARasterConcrete::PieRGBA(ISurface*, int, int, int, int, int, int, int, int, int) { return -1; }
		 int EARasterConcrete::FilledPieColor(ISurface*, int, int, int, int, int, const Color&) { return -1; }
		 int EARasterConcrete::FilledPieRGBA(ISurface*, int, int, int, int, int, int, int, int, int) { return -1; }
         int EARasterConcrete::TrigonColor(ISurface*, int, int, int, int, int, int, const Color&) { return -1; }
		 int EARasterConcrete::TrigonRGBA(ISurface*, int, int, int, int, int, int, int, int, int, int) { return -1; }
		 int EARasterConcrete::AATrigonColor(ISurface*, int, int, int, int, int, int, const Color&) { return -1; }
		 int EARasterConcrete::AATrigonRGBA(ISurface*, int, int, int, int, int, int, int, int, int, int) { return -1; }
		 int EARasterConcrete::FilledTrigonColor(ISurface*, int, int, int, int, int, int, const Color&) { return -1; }
		 int EARasterConcrete::FilledTrigonRGBA(ISurface*, int, int, int, int, int, int, int, int, int, int) { return -1; }
		 int EARasterConcrete::TexturedPolygon(ISurface*, const int*, const int*, int, ISurface*, int, int) { return -1; }
		 int EARasterConcrete::TexturedPolygonMT(ISurface*, const int*, const int*, int, ISurface*, int, int, int**, int*) { return -1; }
#endif


		///////////////////////////////////////////////////////////////////////
		// Resampling — ReCap: stubbed (return NULL, no resampler ported).
		///////////////////////////////////////////////////////////////////////

		 ISurface* EARasterConcrete::ZoomSurface(ISurface*, double, double, bool) { return NULL; }
		 void EARasterConcrete::ZoomSurfaceSize(int, int, double, double, int* dstwidth, int* dstheight)
		 {
			 if(dstwidth)  *dstwidth  = 0;
			 if(dstheight) *dstheight = 0;
		 }
		 ISurface* EARasterConcrete::ShrinkSurface(ISurface*, int, int) { return NULL; }
		 ISurface* EARasterConcrete::RotateSurface90Degrees(ISurface*, int) { return NULL; }
         ISurface* EARasterConcrete::TransformSurface(ISurface*, Rect&, Matrix2D&) { return NULL; }
         ISurface* EARasterConcrete::CreateTransparentSurface(ISurface*, int) { return NULL; }


		///////////////////////////////////////////////////////////////////////
		// Blit functions — ReCap: stubbed (mb renders; no CPU blitter ported).
		///////////////////////////////////////////////////////////////////////

		 bool EARasterConcrete::ClipForBlit(ISurface*, const Rect*, ISurface*, const Rect*, Rect&, Rect&) { return false; }
		 int EARasterConcrete::Blit(ISurface*, const Rect*, ISurface*, const Rect*, const Rect*) { return -1; }
		 int EARasterConcrete::BlitNoClip(ISurface*, const Rect*, ISurface*, const Rect*) { return -1; }
		 int EARasterConcrete::BlitTiled(ISurface*, const Rect*, ISurface*, const Rect*, int, int) { return -1; }
		 int EARasterConcrete::BlitEdgeTiled(ISurface*, const Rect*, ISurface*, const Rect*, const Rect*) { return -1; }
		 bool EARasterConcrete::SetupBlitFunction(ISurface*, ISurface*) { return false; }

		///////////////////////////////////////////////////////////////////////
		// Utility functions
		///////////////////////////////////////////////////////////////////////

		 bool EARasterConcrete::IntersectRect(const Rect& a, const Rect& b, Rect& result)
		 {
			 return EA::Raster::IntersectRect(a, b, result);
		 }

		 bool EARasterConcrete::WritePPMFile(const char* pPath, ISurface* pSurface, bool bAlphaOnly)
		 {
			 return EA::Raster::WritePPMFile(pPath, pSurface, bAlphaOnly);
		 }

		 RGBA32 EARasterConcrete::makeRGB(int32_t r, int32_t g, int32_t b)
		 {
			 return EA::Raster::makeRGB( r,  g,  b);
		 }
		 RGBA32 EARasterConcrete::makeRGBA(int32_t r, int32_t g, int32_t b, int32_t a)
		 {
			 return EA::Raster::makeRGBA( r,  g,  b,  a);
		 }

	}
}
