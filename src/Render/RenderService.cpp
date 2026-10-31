// =============================================================================
//  RenderService.cpp
//  Worker thread that owns the Windows PDF engine.
//
//  The worker runs in a multi-threaded COM apartment and keeps the WinRT
//  PdfDocument alive for as long as a document is open; the UI thread never
//  touches WinRT.  Communication is strictly one-way in each direction (the UI
//  posts jobs, the worker posts messages), so neither side ever blocks on the
//  other.  Everything the worker touches - the document, the WIC factory and
//  the decoded bitmaps - is created and destroyed on that thread.
// =============================================================================
#include "Render/RenderService.h"

#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.h>
#include <winrt/base.h>

#include <shlwapi.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdint>
#include <memory>

#include "Core/Log.h"
#include "Core/Utf.h"

using winrt::Windows::Data::Pdf::PdfDocument;
using winrt::Windows::Data::Pdf::PdfPageRenderOptions;

namespace pengpdf {
namespace {

namespace streams = winrt::Windows::Storage::Streams;

// Turns a WinRT error into a short human readable sentence.  The UI adds the
// document name around this text, so it must stay a fragment.
//
// The mapping is necessarily heuristic: WinRT reports several unrelated
// failures with the same HRESULT, and the engine uses a few undocumented ones
// for damaged files.  Only the cases that are clearly distinguishable are
// classified; everything else falls through to InvalidDocument, because a
// truncated download or a mislabelled .pdf is by far the most common cause in
// practice.  The fallback text comes from Windows and may be localised, so the
// UI uses the error kind (not this text) to pick its message.
std::wstring DescribeRenderError(const winrt::hresult_error& error,
                                 RenderErrorKind& kind) {
    const uint32_t code = static_cast<uint32_t>(error.code());

    switch (code) {
        case 0x8007052Bu:  // ERROR_WRONG_PASSWORD
        case 0x80070057u:  // E_INVALIDARG (some builds report this for passwords)
            kind = RenderErrorKind::PasswordProtected;
            return L"The document is protected by a password.";
        case 0x80070002u:  // ERROR_FILE_NOT_FOUND
            kind = RenderErrorKind::FileNotFound;
            return L"The file could not be found.";
        case 0x80070005u:  // E_ACCESSDENIED
            kind = RenderErrorKind::AccessDenied;
            return L"Access to the file was denied.";
        case 0x80004005u:  // E_FAIL: the engine's generic failure
            kind = RenderErrorKind::InvalidDocument;
            return L"The PDF engine could not open the document.";
        default:
            break;
    }

    const std::wstring message(error.message().c_str());
    // The engine reports corrupt files with various undocumented codes; in
    // practice "not a valid PDF" is the useful explanation for the user.
    kind = RenderErrorKind::InvalidDocument;
    if (!message.empty()) {
        return message;
    }
    return Format(L"Unexpected error 0x%08X.", code);
}

// Converts the PNG emitted by the PDF engine into 32bpp BGRA pixels, which is
// the layout GDI expects for a top-down DIB.
//
// The engine can only render to an image stream, so every page makes a detour
// through WIC; the COM objects are reference counted by winrt::com_ptr and are
// released when this function returns.  A decode failure is reported as "page
// unavailable" (only the decoder creation step logs); it must not tear down the
// worker.
bool DecodePng(const std::vector<uint8_t>& png,
               const winrt::com_ptr<IWICImagingFactory>& factory,
               std::vector<uint32_t>& pixels, int& pixelWidth, int& pixelHeight) {
    if (png.empty() || factory == nullptr) {
        return false;
    }

    winrt::com_ptr<IStream> memoryStream;
    // SHCreateMemStream() hands back a stream with one reference that belongs
    // to the caller, so attach() adopts it; adding a reference here would leak
    // the stream once the com_ptr goes out of scope.
    memoryStream.attach(::SHCreateMemStream(png.data(), static_cast<UINT>(png.size())));
    if (memoryStream == nullptr) {
        return false;
    }

    winrt::com_ptr<IWICBitmapDecoder> decoder;
    const HRESULT createResult = factory->CreateDecoderFromStream(
        memoryStream.get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.put());
    if (FAILED(createResult)) {
        // WIC refuses PNG data without a container signature in rare cases;
        // decoding through the WinRT bitmap decoder is not worth the detour,
        // so the page is simply reported as unavailable.
        LogError(L"WIC decoder creation failed: 0x%08X", static_cast<unsigned>(createResult));
        return false;
    }

    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.put()))) {
        return false;
    }

    winrt::com_ptr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.put()))) {
        return false;
    }
    if (FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0) {
        return false;
    }

    const UINT stride = width * 4;
    const UINT byteCount = stride * height;
    pixels.resize(static_cast<size_t>(width) * height);
    // An explicit stride guarantees that the rows are tightly packed.  A
    // top-down DIB uses exactly this layout, so the canvas can wrap the vector
    // in a BITMAPINFOHEADER without any repacking.
    if (FAILED(converter->CopyPixels(nullptr, stride, byteCount,
                                     reinterpret_cast<BYTE*>(pixels.data())))) {
        return false;
    }

    pixelWidth = static_cast<int>(width);
    pixelHeight = static_cast<int>(height);
    return true;
}

}  // namespace

RenderService::RenderService(HWND notifyWindow) : m_notifyWindow(notifyWindow) {
    // Start the thread eagerly so the first OpenDocument() only has to enqueue
    // a job.  std::thread throws on failure; the caller creates this object
    // while constructing the main window, where that is still fatal.
    m_thread = std::thread(&RenderService::WorkerMain, this);
}

RenderService::~RenderService() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
        // Discard queued renders: closing the window must not wait for pages
        // the user is never going to see.  A render already in flight still
        // finishes (WinRT offers no cancellation), but that is bounded by one
        // page instead of the whole queue.
        m_jobs.clear();
        Job quit;
        quit.kind = Job::Kind::Quit;
        m_jobs.push_back(std::move(quit));
    }
    m_condition.notify_all();
    if (m_thread.joinable()) {
        // Joining guarantees that all WinRT objects were released on the
        // worker thread before this destructor returns.
        m_thread.join();
    }
}

void RenderService::OpenDocument(const std::wstring& path, uint64_t token) {
    Job job;
    job.kind = Job::Kind::Open;
    job.path = path;
    job.token = token;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Anything queued for the previous document is now meaningless.  The
        // currently loaded document stays alive until the worker reaches this
        // job, so the UI can still finish drawing the old page in the
        // meantime; Close/Quit jobs keep their relative order.
        m_jobs.erase(std::remove_if(m_jobs.begin(), m_jobs.end(),
                                    [](const Job& queued) {
                                        return queued.kind == Job::Kind::Open ||
                                               queued.kind == Job::Kind::Render;
                                    }),
                     m_jobs.end());
        m_jobs.push_back(std::move(job));
    }
    m_condition.notify_one();
}

void RenderService::CloseDocument() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Pending work for the document is dropped; a render already running
        // on the worker still finishes and posts its result, which the UI
        // discards through the document token.
        m_jobs.erase(std::remove_if(m_jobs.begin(), m_jobs.end(),
                                    [](const Job& queued) {
                                        return queued.kind == Job::Kind::Render ||
                                               queued.kind == Job::Kind::Open;
                                    }),
                     m_jobs.end());
        Job job;
        job.kind = Job::Kind::Close;
        m_jobs.push_back(std::move(job));
    }
    m_condition.notify_one();
}

void RenderService::RequestPage(int pageIndex, int targetWidth, uint64_t token) {
    if (pageIndex < 0 || targetWidth <= 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    // Coalesce repeated requests for the same page: only the newest width is
    // interesting, everything else would just waste CPU time.  Comparing the
    // token too keeps a request for the newly opened document separate from a
    // leftover one for the previous document.
    for (Job& queued : m_jobs) {
        if (queued.kind == Job::Kind::Render && queued.pageIndex == pageIndex &&
            queued.token == token) {
            queued.targetWidth = targetWidth;
            return;
        }
    }

    Job job;
    job.kind = Job::Kind::Render;
    job.pageIndex = pageIndex;
    job.targetWidth = targetWidth;
    job.token = token;
    m_jobs.push_back(std::move(job));
    m_condition.notify_one();
}

void RenderService::CancelPendingPages() {
    // Called when the cached bitmaps become invalid, mainly on zoom changes.
    // Only queued jobs are removed: a render already inside
    // RenderToStreamAsync() cannot be cancelled, but its result is dropped by
    // the canvas because requestedWidth no longer matches.
    std::lock_guard<std::mutex> lock(m_mutex);
    m_jobs.erase(std::remove_if(m_jobs.begin(), m_jobs.end(),
                                [](const Job& queued) {
                                    return queued.kind == Job::Kind::Render;
                                }),
                 m_jobs.end());
}

bool RenderService::HasDocument() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_hasDocument;
}

int RenderService::PageCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<int>(m_pages.size());
}

PageMetrics RenderService::PageSizeAt(int pageIndex) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Out-of-range queries return a zero size instead of asserting.  Callers
    // can race with an asynchronous load or close, and a zero size is an
    // unambiguous "unknown page" that needs no extra error channel.
    if (pageIndex < 0 || pageIndex >= static_cast<int>(m_pages.size())) {
        return PageMetrics{};
    }
    return m_pages[static_cast<size_t>(pageIndex)];
}

void RenderService::PostDocumentReady(DocumentReadyMessage* message) {
    if (m_notifyWindow == nullptr) {
        delete message;
        return;
    }
    // PostMessageW() does not copy the payload: on success ownership moves to
    // the window procedure (which wraps it in a unique_ptr), on failure the
    // message was never delivered and must be freed here.
    if (::PostMessageW(m_notifyWindow, WM_RENDER_DOCUMENT_READY, 0,
                       reinterpret_cast<LPARAM>(message)) == FALSE) {
        delete message;
    }
}

void RenderService::PostPageReady(PageReadyMessage* message) {
    // Same ownership contract as PostDocumentReady().
    if (m_notifyWindow == nullptr) {
        delete message;
        return;
    }
    if (::PostMessageW(m_notifyWindow, WM_RENDER_PAGE_READY, 0,
                       reinterpret_cast<LPARAM>(message)) == FALSE) {
        delete message;
    }
}

void RenderService::WorkerMain() {
    // The PDF engine and WIC are COM based; an MTA apartment on this thread
    // keeps the blocking .get() calls safe.  init_apartment can throw (for
    // example when COM is already initialised with an incompatible model), and
    // an exception escaping a std::thread body would call std::terminate, so
    // the failure is caught and reported through the normal Open error path.
    bool apartmentReady = false;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartmentReady = true;
    } catch (const winrt::hresult_error& error) {
        LogError(L"COM apartment initialisation failed: 0x%08X %s",
                 static_cast<unsigned>(error.code()), error.message().c_str());
    } catch (...) {
        LogError(L"COM apartment initialisation failed with an unknown error");
    }

    // Created once and reused for every page: the factory is free-threaded and
    // creating one per page would cost more than decoding a small bitmap.
    winrt::com_ptr<IWICImagingFactory> imagingFactory;
    if (apartmentReady &&
        FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(imagingFactory.put())))) {
        LogError(L"Failed to create the WIC imaging factory");
    }

    // Null until the first successful Open job.  Only this thread ever reads
    // or writes the handle, which is why no lock protects it.
    PdfDocument document{nullptr};
    std::vector<PageMetrics> pageGeometry;

    for (;;) {
        Job job;
        {
            // Sleep until there is work or the destructor asked us to stop.
            // The lock is released before the job runs: everything the job
            // needs has been copied into `job` by the posting thread.
            std::unique_lock<std::mutex> lock(m_mutex);
            m_condition.wait(lock, [this] { return m_stopping || !m_jobs.empty(); });
            if (m_jobs.empty()) {
                break;  // only possible when stopping
            }
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
        }

        if (job.kind == Job::Kind::Quit) {
            break;
        }

        if (job.kind == Job::Kind::Close) {
            // Release the engine's file handle before clearing the sizes, so a
            // closed document does not keep the PDF locked on disk.  The table
            // and flag are updated under the lock the accessors use.
            document = nullptr;
            pageGeometry.clear();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_pages.clear();
                m_hasDocument = false;
            }
            continue;
        }

        if (job.kind == Job::Kind::Open) {
            // Drop the previous document first: if the new load fails the
            // service must not keep rendering the old document under the new
            // token.
            document = nullptr;
            pageGeometry.clear();

            auto* message = new DocumentReadyMessage();
            message->token = job.token;

            if (!apartmentReady) {
                // Without a COM apartment no WinRT call can succeed; report a
                // normal engine failure instead of crashing the worker.
                message->ok = false;
                message->errorKind = RenderErrorKind::EngineFailure;
                message->error = L"The PDF engine could not be initialised.";
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_pages.clear();
                    m_hasDocument = false;
                }
                PostDocumentReady(message);
                continue;
            }

            try {
                // StorageFile only accepts an absolute path; MainWindow passes
                // the canonicalised document path, so no extra normalisation
                // is needed here.
                const auto file = winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(
                                      winrt::hstring(job.path))
                                      .get();
                document = PdfDocument::LoadFromFileAsync(file).get();

                // The engine exposes page sizes but no layout, so every page
                // is measured once here.  That lets the canvas build the full
                // scroll geometry before a single bitmap has been rendered.
                const uint32_t pageCount = document.PageCount();
                pageGeometry.reserve(pageCount);
                for (uint32_t index = 0; index < pageCount; ++index) {
                    const auto page = document.GetPage(index);
                    const auto size = page.Size();
                    PageMetrics metrics;
                    metrics.width = size.Width;
                    metrics.height = size.Height;
                    pageGeometry.push_back(metrics);
                }

                message->ok = true;
                // The geometry is copied into the message and into the shared
                // table below; a few hundred bytes per page, negligible next
                // to the bitmaps that are produced later.
                message->pages = pageGeometry;
                LogInfo(L"Opened %s: %u pages", job.path.c_str(), pageCount);
            } catch (const winrt::hresult_error& error) {
                // Password protected documents end up here as well: there is
                // no password prompt, so the user gets a clear error instead of
                // an empty window.
                message->ok = false;
                message->error = DescribeRenderError(error, message->errorKind);
                document = nullptr;
                pageGeometry.clear();
                LogError(L"Failed to open %s: %s", job.path.c_str(),
                         message->error.c_str());
            } catch (...) {
                // Anything that is not an HRESULT (allocation failure, a
                // WinRT wrapper exception) is still reported to the UI.
                message->ok = false;
                message->errorKind = RenderErrorKind::EngineFailure;
                message->error = L"The PDF engine failed unexpectedly.";
                document = nullptr;
                pageGeometry.clear();
            }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_pages = pageGeometry;
                m_hasDocument = message->ok;
            }
            PostDocumentReady(message);
            continue;
        }

        // Page rendering ----------------------------------------------------
        // A render job can arrive after the document was closed or after the
        // Open job failed; drop it silently.  The UI has already moved on to
        // another token and ignores any late result anyway.
        if (!document) {
            continue;
        }

        auto* message = new PageReadyMessage();
        message->token = job.token;
        message->pageIndex = job.pageIndex;
        message->requestedWidth = job.targetWidth;

        // From here on every failure path deletes the message; only the happy
        // path at the bottom passes ownership to PostPageReady().
        try {
            const uint32_t pageCount = document.PageCount();
            if (job.pageIndex < 0 || static_cast<uint32_t>(job.pageIndex) >= pageCount) {
                delete message;
                continue;
            }

            const auto page = document.GetPage(static_cast<uint32_t>(job.pageIndex));
            auto stream = streams::InMemoryRandomAccessStream();

            PdfPageRenderOptions options;
            // Defensive bound.  The canvas already clamps to
            // kMaxPageRenderWidth; 16 px is the smallest bitmap worth making,
            // and the upper bound protects against a bogus display width
            // allocating a multi-gigabyte buffer.
            const uint32_t width = static_cast<uint32_t>(
                std::clamp(job.targetWidth, 16, 20000));
            options.DestinationWidth(width);
            // PDF pages have no background of their own; without this the
            // engine would leave the page transparent and the canvas colour
            // would show through instead of white paper.
            options.BackgroundColor(winrt::Windows::UI::Color{255, 255, 255, 255});
            // Blocking is safe here: this is the MTA worker thread and nothing
            // else waits for it.  A zoom change does not cancel the operation,
            // it only makes the result stale (checked by the canvas).
            page.RenderToStreamAsync(stream, options).get();

            const uint32_t streamSize = static_cast<uint32_t>(stream.Size());
            if (streamSize == 0) {
                delete message;
                continue;
            }

            // The PNG is small enough to be read in one LoadAsync() call.
            streams::DataReader reader(stream.GetInputStreamAt(0));
            reader.LoadAsync(streamSize).get();
            std::vector<uint8_t> png(reader.UnconsumedBufferLength());
            if (!png.empty()) {
                reader.ReadBytes(
                    winrt::array_view<uint8_t>(png.data(), png.data() + png.size()));
            }

            int pixelWidth = 0;
            int pixelHeight = 0;
            if (png.empty() ||
                !DecodePng(png, imagingFactory, message->pixels, pixelWidth, pixelHeight)) {
                // Keep the page unavailable instead of failing the document:
                // the canvas keeps its placeholder and tries again the next
                // time the page becomes visible or the zoom changes.
                delete message;
                continue;
            }
            message->pixelWidth = pixelWidth;
            message->pixelHeight = pixelHeight;
        } catch (const winrt::hresult_error& error) {
            // A failed page is logged, not surfaced: one broken page must not
            // abort the loop or the rest of the document.
            LogError(L"Rendering page %d failed: 0x%08X %s", job.pageIndex,
                     static_cast<unsigned>(error.code()), error.message().c_str());
            delete message;
            continue;
        } catch (...) {
            LogError(L"Rendering page %d failed with an unknown error", job.pageIndex);
            delete message;
            continue;
        }

        PostPageReady(message);
    }

    // Release every WinRT object while the apartment is still alive, then tear
    // the apartment down.  The document may still be open here when the window
    // was destroyed without closing the document first, so it has to be
    // released before uninit_apartment().
    document = nullptr;
    imagingFactory = nullptr;
    if (apartmentReady) {
        winrt::uninit_apartment();
    }
}

}  // namespace pengpdf
