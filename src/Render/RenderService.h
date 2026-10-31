// =============================================================================
//  RenderService.h
//  Asynchronous page rasteriser.
//
//  Windows ships a complete, high fidelity PDF renderer: the WinRT API
//  Windows.Data.Pdf.PdfDocument.  Using it keeps font handling, transparency,
//  shadings and images correct without embedding a third-party engine.
//
//  Rendering and PDF parsing are much slower than painting, so all WinRT work
//  happens on a dedicated worker thread.  Results are delivered to the UI
//  thread through ordinary window messages, which avoids any locking in the
//  paint path.
// =============================================================================
#pragma once

#include <windows.h>

#include <cstdint>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <string>
#include <thread>
#include <vector>

namespace pengpdf {

// Custom window messages posted by the render thread.
//   WM_RENDER_DOCUMENT_READY -> DocumentReadyMessage*
//   WM_RENDER_PAGE_READY     -> PageReadyMessage*
// The receiver owns the payload and must delete it.
//
// The payload is a raw heap pointer, so a window that is destroyed before the
// message is dispatched discards it and leaks the payload.  The notify window
// therefore has to outlive the service: MainWindow destroys the service from
// its WM_DESTROY handler, while the HWND is still valid.
inline constexpr UINT WM_RENDER_DOCUMENT_READY = WM_APP + 100;
inline constexpr UINT WM_RENDER_PAGE_READY = WM_APP + 101;

// Page geometry in device independent pixels (96 per inch), as reported by the
// Windows PDF engine.  The canvas turns these into pixels using the current
// zoom and the monitor DPI; the render service never deals with zoom.
struct PageMetrics {
    double width = 0.0;
    double height = 0.0;
};

// Classification of a failed document load.  The UI maps these values onto
// localised messages instead of showing raw HRESULT text.
enum class RenderErrorKind {
    None,
    FileNotFound,
    AccessDenied,
    PasswordProtected,
    InvalidDocument,
    EngineFailure,
};

// Result of an OpenDocument() request.  `token` is the caller's document
// generation counter: the UI ignores messages whose token no longer matches,
// which is how a slow load of an already replaced document is discarded.
struct DocumentReadyMessage {
    uint64_t token = 0;
    bool ok = false;
    RenderErrorKind errorKind = RenderErrorKind::None;
    std::wstring error;  // technical detail, used for logging
    std::vector<PageMetrics> pages;
};

// One rasterised page.  `requestedWidth` lets the canvas drop bitmaps that were
// produced for an older zoom level, while `pixelWidth`/`pixelHeight` describe
// the bitmap that actually came back (the engine may round the size).
struct PageReadyMessage {
    uint64_t token = 0;
    int pageIndex = 0;
    int requestedWidth = 0;  // display width the bitmap was produced for
    int pixelWidth = 0;
    int pixelHeight = 0;
    // Tightly packed 32bpp BGRA, top-down rows, stride = pixelWidth * 4.  This
    // is exactly the layout CreateDIBSection() expects for a top-down DIB.
    std::vector<uint32_t> pixels;
};

// Owns the worker thread and the (thread affine) WinRT PDF document.  The
// request methods are safe to call from the UI thread: they only enqueue work
// under m_mutex and never block on the engine, so painting is never stalled by
// a slow page.  Only the destructor waits, because it has to join the worker.
class RenderService {
public:
    // `notifyWindow` receives the messages above.  It must outlive the service.
    explicit RenderService(HWND notifyWindow);
    ~RenderService();

    RenderService(const RenderService&) = delete;
    RenderService& operator=(const RenderService&) = delete;

    // Starts loading a document.  The result arrives asynchronously.
    // Loading and rendering jobs queued for the previous document are dropped,
    // but a render that is already running cannot be interrupted; its result
    // is then discarded by the token check in the UI.
    void OpenDocument(const std::wstring& path, uint64_t token);

    // Drops the currently loaded document (any queued work is discarded).
    // The page table is cleared by the worker, so PageCount()/HasDocument()
    // keep reporting the old document until this job has been processed.
    void CloseDocument();

    // Requests one page bitmap at the given width in pixels.  Requests are
    // coalesced: asking for the same page again replaces the queued request.
    // Negative page indices and non-positive widths are ignored.
    void RequestPage(int pageIndex, int targetWidth, uint64_t token);

    // Forgets queued page requests, e.g. after a zoom change.  A render that
    // is already in flight is not affected.
    void CancelPendingPages();

    // These only see completed jobs: after OpenDocument() they still describe
    // the previous document until the load has finished on the worker thread.
    bool HasDocument() const;
    int PageCount() const;
    PageMetrics PageSizeAt(int pageIndex) const;

private:
    // One unit of work in the queue.  A job is moved out of the queue by the
    // worker, so no member is shared after that point.
    struct Job {
        enum class Kind { Open, Close, Render, Quit };
        Kind kind = Kind::Quit;
        std::wstring path;
        int pageIndex = 0;
        int targetWidth = 0;
        uint64_t token = 0;
    };

    // Worker thread body.  It exclusively owns the WinRT document object and
    // exits after processing the Quit job.
    void WorkerMain();

    // Both helpers take ownership of `message` and delete it when the window
    // handle is null or PostMessageW() fails, so a failed post cannot leak.
    void PostDocumentReady(DocumentReadyMessage* message);
    void PostPageReady(PageReadyMessage* message);

    HWND m_notifyWindow = nullptr;

    std::thread m_thread;
    // Guards the job queue, m_stopping and the page table below.  The worker
    // holds it only while popping a job or copying results; all WinRT calls
    // run outside the lock so the UI never waits for the engine.
    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    std::deque<Job> m_jobs;
    bool m_stopping = false;

    // Guarded by m_mutex.
    bool m_hasDocument = false;
    std::vector<PageMetrics> m_pages;
};

}  // namespace pengpdf
