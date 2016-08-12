//--------------------------------------------------------------------------------------
// File: LinearAllocator.cpp
//
// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved.
//
// http://go.microsoft.com/fwlink/?LinkID=615561
//--------------------------------------------------------------------------------------

#include "pch.h"
#include "DirectXHelpers.h"
#include "PlatformHelpers.h"
#include "LinearAllocator.h"

#define VALIDATE_LISTS 0

#if VALIDATE_LISTS
#   include <unordered_set>
#endif

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace
{
    inline size_t AlignOffset(size_t offset, size_t alignment)
    {
        if (alignment > 0)
        {
            // Alignment must be a power of 2
            assert((alignment & (alignment - 1)) == 0);
            offset = (offset + alignment - 1) & ~(alignment - 1);
        }
        return offset;
    }
}

LinearAllocatorPage::LinearAllocatorPage()
    : pPrevPage(nullptr)
    , pNextPage(nullptr)
    , mMemory(nullptr)
    , mUploadResource(nullptr)
    , mFence(nullptr)
    , mPendingFence(0)
    , mGpuAddress {}
    , mOffset(0)
    , mSize(0)
    , mRefCount(0)
{
}

size_t LinearAllocatorPage::Suballocate(_In_ size_t size, _In_ size_t alignment)
{
    size_t offset = AlignOffset(mOffset, alignment);
#ifdef _DEBUG
    if (offset + size > mSize)
        throw std::exception("Out of free memory in page suballoc");
#endif
    mOffset = offset + size;
    return offset;
}

LinearAllocator::LinearAllocator(
    _In_ ID3D12Device* pDevice,
    _In_ size_t pageSize,
    _In_ size_t preallocateBytes)
    : m_pendingPages( nullptr )
    , m_usedPages( nullptr )
    , m_unusedPages( nullptr )
    , m_increment( 0 )
{
    m_increment = pageSize;
    m_usedPages = nullptr;
    m_unusedPages = nullptr;
    m_pendingPages = nullptr;
    m_device = pDevice;
    m_totalPages = 0;
    m_numPending = 0;

    size_t preallocatePageCount = ( ( preallocateBytes + pageSize - 1 ) / pageSize );
    for (size_t preallocatePages = 0; preallocateBytes != 0 && preallocatePages < preallocatePageCount; ++preallocatePages )
    {
        if ( GetNewPage() == nullptr )
        {
            throw std::exception("Out of memory.");
        }
    }
}

LinearAllocator::~LinearAllocator()
{
    // Must wait for all pending fences!
    while ( m_pendingPages != nullptr )
    {
        RetirePendingPages();
    }

    assert( m_pendingPages == nullptr );

    // Return all the memory
    FreePages( m_unusedPages );
    FreePages( m_usedPages );

    m_pendingPages = nullptr;
    m_usedPages = nullptr;
    m_unusedPages = nullptr;
    m_increment = 0;
}

LinearAllocatorPage* LinearAllocator::FindPageForAlloc(_In_ size_t size, _In_ size_t alignment)
{
#ifdef _DEBUG
    if( size > m_increment )
        throw std::out_of_range(__FUNCTION__ " size must be less or equal to the allocator's increment");
    if( alignment > m_increment )
        throw std::out_of_range(__FUNCTION__ " alignment must be less or equal to the allocator's increment");
    if ( size == 0 )
        throw std::exception("Cannot honor zero size allocation request.");
#endif

    LinearAllocatorPage* page = GetPageForAlloc( size, alignment );
    if ( page == nullptr )
    {
        throw std::exception("Out of memory.");
    }

    return page;
}

// Call this after you submit your work to the driver.
void LinearAllocator::FenceCommittedPages(_In_ ID3D12CommandQueue* commandQueue)
{
    // No pending pages
    if (m_usedPages == nullptr)
        return;

    // For all the used pages, fence them
    UINT numReady = 0;
    LinearAllocatorPage* readyPages = nullptr;
    LinearAllocatorPage* unreadyPages = nullptr;
    LinearAllocatorPage* nextPage = nullptr;
    for (LinearAllocatorPage* page = m_usedPages; page != nullptr; page = nextPage)
    {
        nextPage = page->pNextPage;

        // Disconnect from the list
        page->pPrevPage = nullptr;

        if (page->RefCount() == 0)
        {
            // Signal the fence
            numReady++;
            commandQueue->Signal(page->mFence.Get(), ++page->mPendingFence);
                
            // Link to the ready pages list
            page->pNextPage = readyPages;
            if (readyPages) readyPages->pPrevPage = page;
            readyPages = page;
        }
        else
        {
            // Link to the unready list
            page->pNextPage = unreadyPages;
            if (unreadyPages) unreadyPages->pPrevPage = page;
            unreadyPages = page;
        }
    }

    // Replace the used pages list with the new unready list
    m_usedPages = unreadyPages;

    // Append all those pages from the ready list to the pending list
    if (numReady > 0)
    {
        m_numPending += numReady;
        LinkPageChain(readyPages, m_pendingPages);
    }

#if VALIDATE_LISTS
    ValidatePageLists();
#endif
}

// Call this once a frame after all of your driver submissions.
// (immediately before or after Present-time)
void LinearAllocator::RetirePendingPages()
{
    // For each page that we know has a fence pending, check it. If the fence has passed,
    // we can mark the page for re-use.
    LinearAllocatorPage* page = m_pendingPages;
    while ( page != nullptr )
    {
        LinearAllocatorPage* nextPage = page->pNextPage;

        assert( page->mPendingFence != 0 );

        if ( page->mFence->GetCompletedValue() >= page->mPendingFence )
        {
            // Fence has passed. It is safe to use this page again.
            ReleasePage( page );
        }

        page = nextPage;
    }
}

void LinearAllocator::Shrink()
{
    FreePages( m_unusedPages );
    m_unusedPages = nullptr;

#if VALIDATE_LISTS
    ValidatePageLists();
#endif
}

LinearAllocatorPage* LinearAllocator::GetCleanPageForAlloc()
{
    // Grab the first unused page, if one exists. Else, allocate a new page.
    LinearAllocatorPage* page = m_unusedPages;
    if (page == nullptr)
    {
        // Allocate a new page
        page = GetNewPage();
        if (page == nullptr)
        {
            // OOM.
            return nullptr;
        }
    }

    // Mark this page as used
    UnlinkPage(page);
    LinkPage(page, m_usedPages);

    assert(page->mOffset == 0);

    return page;
}

LinearAllocatorPage* LinearAllocator::GetPageForAlloc(
    size_t sizeBytes,
    size_t alignment)
{
    // Fast path
    if ( sizeBytes == m_increment && (alignment == 0 || alignment == m_increment) )
    {
        return GetCleanPageForAlloc();
    }

    // Find a page in the pending pages list that has space.
    LinearAllocatorPage* page = FindPageForAlloc( m_usedPages, sizeBytes, alignment );
    if ( page == nullptr )
    {
        page = GetCleanPageForAlloc();
    }

    return page;
}

LinearAllocatorPage* LinearAllocator::FindPageForAlloc(
    LinearAllocatorPage* list,
    size_t sizeBytes,
    size_t alignment)
{
    for ( LinearAllocatorPage* page = list; page != nullptr; page = page->pNextPage )
    {
        size_t offset = AlignOffset(page->mOffset, alignment);
        if ( offset + sizeBytes <= m_increment )
            return page;
    }
    return nullptr;
}

LinearAllocatorPage* LinearAllocator::GetNewPage()
{
    ComPtr<ID3D12Resource> spResource;

    CD3DX12_HEAP_PROPERTIES uploadHeapProperties( D3D12_HEAP_TYPE_UPLOAD );
    CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer( m_increment );

    // Allocate the upload heap
    if (FAILED(m_device->CreateCommittedResource(
        &uploadHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_GRAPHICS_PPV_ARGS(spResource.ReleaseAndGetAddressOf()) )))
    {
        return nullptr;
    }

    if (m_debugName.size() > 0)
    {
        spResource->SetName(m_debugName.c_str());
    }

    // Get a pointer to the memory
    void* pMemory = nullptr;
    ThrowIfFailed(spResource->Map( 0, nullptr, &pMemory ));
    memset(pMemory, 0, m_increment);

    // Create a fence
    ComPtr<ID3D12Fence> spFence;
    if (FAILED(m_device->CreateFence(
        0,
        D3D12_FENCE_FLAG_NONE,
        IID_GRAPHICS_PPV_ARGS(spFence.ReleaseAndGetAddressOf()) )))
    {
        return nullptr;
    }

    // Add the page to the page list
    LinearAllocatorPage* page = new LinearAllocatorPage;
    page->mSize = m_increment;
    page->mMemory = pMemory;
    page->pPrevPage = nullptr;
    page->pNextPage = m_unusedPages;
    page->mUploadResource = spResource;
    page->mFence = spFence;
    page->mGpuAddress = spResource->GetGPUVirtualAddress();

    // Set as head of the list
    page->pNextPage = m_unusedPages;
    if (m_unusedPages) m_unusedPages->pPrevPage = page;
    m_unusedPages = page;
    m_totalPages++;

#if VALIDATE_LISTS
    ValidatePageLists();
#endif

    return page;
}

void LinearAllocator::UnlinkPage( LinearAllocatorPage* page )
{
    if ( page->pPrevPage )
        page->pPrevPage->pNextPage = page->pNextPage;

    // Check that it isn't the head of any of our tracked lists
    else if ( page == m_unusedPages )
        m_unusedPages = page->pNextPage;
    else if ( page == m_usedPages )
        m_usedPages = page->pNextPage;
    else if ( page == m_pendingPages )
        m_pendingPages = page->pNextPage;

    if ( page->pNextPage )
        page->pNextPage->pPrevPage = page->pPrevPage;

    page->pNextPage = nullptr;
    page->pPrevPage = nullptr;

#if VALIDATE_LISTS
    ValidatePageLists();
#endif
}

void LinearAllocator::LinkPageChain(LinearAllocatorPage* page, LinearAllocatorPage*& list)
{
#if VALIDATE_LISTS
    // Walk the chain and ensure it's not in the list twice
    for (LinearAllocatorPage* cur = list; cur != nullptr; cur = cur->pNextPage)
    {
        assert(cur != page);
    }
#endif
    assert(page->pPrevPage == nullptr);
    assert(list == nullptr || list->pPrevPage == nullptr);

    // Follow chain to the end and append
    LinearAllocatorPage* lastPage = nullptr;
    for (lastPage = page; lastPage->pNextPage != nullptr; lastPage = lastPage->pNextPage) {}

    lastPage->pNextPage = list;
    if (list)
        list->pPrevPage = lastPage;

    list = page;

#if VALIDATE_LISTS
    ValidatePageLists();
#endif
}

void LinearAllocator::LinkPage( LinearAllocatorPage* page, LinearAllocatorPage*& list )
{
#if VALIDATE_LISTS
    // Walk the chain and ensure it's not in the list twice
    for ( LinearAllocatorPage* cur = list; cur != nullptr; cur = cur->pNextPage )
    {
        assert( cur != page );
    }
#endif
    assert(page->pNextPage == nullptr);
    assert(page->pPrevPage == nullptr);
    assert(list == nullptr || list->pPrevPage == nullptr);

    page->pNextPage = list;
    if ( list )
        list->pPrevPage = page;

    list = page;

#if VALIDATE_LISTS
    ValidatePageLists();
#endif
}

void LinearAllocator::ReleasePage( LinearAllocatorPage* page )
{
    assert( m_numPending > 0 );
    m_numPending--;

    UnlinkPage( page );
    LinkPage( page, m_unusedPages );

    // Reset the page offset (effectively erasing the memory)
    page->mOffset = 0;

#ifdef _DEBUG
    memset( page->mMemory, 0, m_increment );
#endif

#if VALIDATE_LISTS
    ValidatePageLists();
#endif
}

void LinearAllocator::FreePages( LinearAllocatorPage* page )
{
    while ( page != nullptr )
    {
        LinearAllocatorPage* nextPage = page->pNextPage;

        page->mUploadResource->Unmap( 0, nullptr );
        delete page;

        page = nextPage;
        m_totalPages--;
    }
}

#if VALIDATE_LISTS
void LinearAllocator::ValidateList(LinearAllocatorPage* list)
{
    for (LinearAllocatorPage* page = list, *lastPage = nullptr;
            page != nullptr;
            lastPage = page, page = page->pNextPage)
    {
        if (page->pPrevPage != lastPage)
        {
            throw std::exception("Broken link to previous");
        }
    }
}

void LinearAllocator::ValidatePageLists()
{
    ValidateList(m_pendingPages);
    ValidateList(m_usedPages);
    ValidateList(m_unusedPages);
}
#endif

void LinearAllocator::SetDebugName(const char* name)
{
    wchar_t wname[MAX_PATH] = {};
    int result = MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, name, static_cast<int>(strlen(name)), wname, MAX_PATH);
    if ( result > 0 )
    {
        SetDebugName(wname);
    }
}

void LinearAllocator::SetDebugName(const wchar_t* name)
{
    m_debugName = name;

    // Rename existing pages
    SetPageDebugName(m_pendingPages);
    SetPageDebugName(m_usedPages);
    SetPageDebugName(m_unusedPages);
}

void LinearAllocator::SetPageDebugName(LinearAllocatorPage* list)
{
    for ( LinearAllocatorPage* page = list; page != nullptr; page = page->pNextPage )
    {
        page->mUploadResource->SetName(m_debugName.c_str());
    }
}