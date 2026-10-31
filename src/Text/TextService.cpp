// =============================================================================
//  TextService.cpp
//  Worker thread that extracts text and walks the document for search hits.
//
//  All PDF access happens on the worker thread; the UI thread only reads the
//  cache under a short mutex or receives heap allocated result messages.  That
//  split keeps scrolling and painting responsive even while a 500 page search
//  is running.
// =============================================================================
#include "Text/TextService.h"

#include <algorithm>
#include <utility>

#include "Text/TextSearch.h"

namespace pengpdf {
namespace {

// Pages kept in memory.  A text page is a few kilobytes, so this bounds memory
// to a couple of megabytes even for very large documents.
constexpr size_t kCacheLimit = 64;

}  // namespace

// Starts the worker immediately; it sleeps until the first job arrives.
TextService::TextService(HWND notifyWindow) : m_notifyWindow(notifyWindow) {
    m_thread = std::thread(&TextService::WorkerMain, this);
}

// Signals shutdown, discards queued work and waits for the worker.  Any
// in-flight extraction finishes first, which is bounded by the page budget.
TextService::~TextService() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
        // Drop queued work: the worker only needs to observe the Quit job.
        m_jobs.clear();
        Job quit;
        quit.kind = Job::Kind::Quit;
        m_jobs.push_back(std::move(quit));
    }
    m_condition.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void TextService::OpenDocument(std::shared_ptr<pdf::Document> document,
                               uint64_t token) {
    Job job;
    job.kind = Job::Kind::Open;
    job.documentToken = token;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_document = document;
        m_documentToken = token;
        m_activeSearchId = 0;
        m_cache.clear();
        m_lastUse.clear();
        m_pendingPages.clear();
        m_useCounter = 0;

        // Results for the previous document are meaningless now.
        m_jobs.erase(std::remove_if(m_jobs.begin(), m_jobs.end(),
                                    [](const Job& queued) {
                                        return queued.kind == Job::Kind::Extract ||
                                               queued.kind == Job::Kind::Search;
                                    }),
                     m_jobs.end());
        job.document = std::move(document);
        m_jobs.push_back(std::move(job));
    }
    m_condition.notify_one();
}

// Drops the document and caches on the UI side; the Close job releases the
// worker's extractor and document reference.
void TextService::CloseDocument() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_document.reset();
        m_documentToken++;
        m_activeSearchId = 0;
        m_cache.clear();
        m_lastUse.clear();
        m_pendingPages.clear();
        // An in-flight job may still finish; the incremented token makes its
        // result harmless because the UI discards stale tokens.
        m_jobs.erase(std::remove_if(m_jobs.begin(), m_jobs.end(),
                                    [](const Job& queued) {
                                        return queued.kind == Job::Kind::Extract ||
                                               queued.kind == Job::Kind::Search ||
                                               queued.kind == Job::Kind::Open;
                                    }),
                     m_jobs.end());
        Job job;
        job.kind = Job::Kind::Close;
        m_jobs.push_back(std::move(job));
    }
    m_condition.notify_one();
}

// Queues an extraction only when the page is neither cached nor already
// pending, which keeps scrolling from generating duplicate jobs.
void TextService::RequestPage(int pageIndex) {
    if (pageIndex < 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_document) {
            return;
        }
        // The cache and the pending set together prevent duplicate work while
        // the user keeps scrolling over the same page.
        if (m_cache.find(pageIndex) != m_cache.end() ||
            m_pendingPages.find(pageIndex) != m_pendingPages.end()) {
            return;
        }
        m_pendingPages.insert(pageIndex);
        Job job;
        job.kind = Job::Kind::Extract;
        job.pageIndex = pageIndex;
        job.documentToken = m_documentToken;
        m_jobs.push_back(std::move(job));
    }
    m_condition.notify_one();
}

// Non-blocking cache lookup used by the UI thread; refreshes the LRU stamp.
std::shared_ptr<const pdf::TextPage> TextService::CachedPage(int pageIndex) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto page = m_cache.find(pageIndex);
    if (page == m_cache.end()) {
        return nullptr;
    }
    // Reading a page counts as a use so it survives the next eviction round.
    m_lastUse[pageIndex] = ++m_useCounter;
    return page->second;
}

// Cancels queued work of the previous query and pushes page 0 so the first
// results appear immediately.
uint64_t TextService::StartSearch(const std::wstring& query) {
    uint64_t searchId = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        searchId = ++m_searchCounter;
        m_activeSearchId = searchId;

        // A new query supersedes every queued page of the previous one.
        m_jobs.erase(std::remove_if(m_jobs.begin(), m_jobs.end(),
                                    [](const Job& queued) {
                                        return queued.kind == Job::Kind::Search;
                                    }),
                     m_jobs.end());

        if (m_document) {
            Job job;
            job.kind = Job::Kind::Search;
            job.searchId = searchId;
            // Normalise once here; every page then reuses the same needle.
            job.query = NormalizeSearchQuery(query);
            job.pageIndex = 0;
            job.documentToken = m_documentToken;
            // Search first so that results appear while other pages are still
            // being extracted.
            m_jobs.push_front(std::move(job));
        }
    }
    m_condition.notify_one();
    return searchId;
}

// The worker checks the active id before every page, so dropping queued jobs
// and clearing the id is enough to stop a running search.
void TextService::CancelSearch() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_activeSearchId = 0;
    m_jobs.erase(std::remove_if(m_jobs.begin(), m_jobs.end(),
                                [](const Job& queued) {
                                    return queued.kind == Job::Kind::Search;
                                }),
                 m_jobs.end());
}

// The worker owns the document, extractor and font cache; all jobs are
// processed sequentially, which removes the need for locks around PDF access.
void TextService::WorkerMain() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_condition.wait(lock, [this] { return m_stopping || !m_jobs.empty(); });
            if (m_jobs.empty()) {
                break;
            }
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
        }

        switch (job.kind) {
            case Job::Kind::Open:
                HandleOpen(job);
                break;
            case Job::Kind::Close:
                // Release the extractor (and its font cache) before the
                // document itself; the order keeps the destructor simple.
                m_extractor.reset();
                m_workerDocument.reset();
                m_workerToken = 0;
                m_workerPageCount = 0;
                m_searchMatchCount = 0;
                m_searchIdForCount = 0;
                break;
            case Job::Kind::Extract:
                HandleExtract(job);
                break;
            case Job::Kind::Search:
                HandleSearch(job);
                break;
            case Job::Kind::Quit:
                return;
        }
    }
}

// Adopts the document and creates an extractor, whose font cache is reused for
// every subsequent page of this document.
void TextService::HandleOpen(Job& job) {
    m_workerDocument = job.document;
    m_workerToken = job.documentToken;
    m_workerPageCount = m_workerDocument ? m_workerDocument->PageCount() : 0;
    m_searchMatchCount = 0;
    m_searchIdForCount = 0;
    // A new extractor also drops the font cache of the previous document.
    m_extractor.reset();
    if (m_workerDocument) {
        m_extractor = std::make_unique<pdf::TextExtractor>(*m_workerDocument);
    }
}

// Extracts one page, publishes it to the cache and posts the UI notification.
// The pending flag is cleared even for stale jobs so a page is not stuck.
void TextService::HandleExtract(const Job& job) {
    if (job.documentToken != m_workerToken || !m_extractor) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pendingPages.erase(job.pageIndex);
        return;
    }

    const std::shared_ptr<const pdf::TextPage> page = ExtractPage(job.pageIndex);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (job.documentToken == m_workerToken) {
            m_cache[job.pageIndex] = page;
            m_lastUse[job.pageIndex] = ++m_useCounter;
            EvictCacheLocked();
        }
        m_pendingPages.erase(job.pageIndex);
    }

    // The message keeps the page alive for the UI even if the cache evicts it.
    auto* message = new TextReadyMessage();
    message->documentToken = job.documentToken;
    message->pageIndex = job.pageIndex;
    message->page = page;
    NotifyTextReady(message);
}

// Searches a single page and either queues the next one or reports completion.
// A page with hits posts its matches immediately so the UI can highlight them.
void TextService::HandleSearch(const Job& job) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (job.searchId == 0 || job.searchId != m_activeSearchId) {
            return;  // a newer query replaced this one
        }
    }
    if (!m_extractor || !m_workerDocument) {
        auto* done = new SearchDoneMessage();
        done->searchId = job.searchId;
        NotifySearchDone(done);
        return;
    }

    if (m_searchIdForCount != job.searchId) {
        m_searchIdForCount = job.searchId;
        m_searchMatchCount = 0;
    }

    // EnsurePage extracts and caches on demand; no UI round trip is needed.
    const std::shared_ptr<const pdf::TextPage> page = EnsurePage(job.pageIndex);
    if (!page) {
        return;
    }

    PageTextIndex index(*page);
    // job.query was normalised by StartSearch, so no per-page normalisation.
    std::vector<pdf::TextMatch> matches =
        index.FindAllNormalized(job.query, job.pageIndex);
    if (!matches.empty()) {
        m_searchMatchCount += matches.size();
        auto* message = new SearchPageMessage();
        message->searchId = job.searchId;
        message->pageIndex = job.pageIndex;
        message->matches = std::move(matches);
        NotifySearchPage(message);
    }

    if (job.pageIndex + 1 < m_workerPageCount) {
        // Queueing the next page at the front keeps a search moving even when
        // the user is not interacting; the UI still gets to process a message
        // between two pages.
        QueueSearchPage(job.searchId, job.query, job.pageIndex + 1);
    } else {
        auto* done = new SearchDoneMessage();
        done->searchId = job.searchId;
        done->pageCount = m_workerPageCount;
        done->matchCount = static_cast<int>(m_searchMatchCount);
        NotifySearchDone(done);
    }
}

void TextService::QueueSearchPage(uint64_t searchId, const std::wstring& query,
                                  int pageIndex) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping || searchId != m_activeSearchId) {
            return;
        }
        Job job;
        job.kind = Job::Kind::Search;
        job.searchId = searchId;
        job.query = query;
        job.pageIndex = pageIndex;
        job.documentToken = m_workerToken;
        m_jobs.push_front(std::move(job));
    }
    m_condition.notify_one();
}

// The extractor reports failure through an empty page; callers therefore never
// receive nullptr from here.
std::shared_ptr<const pdf::TextPage> TextService::ExtractPage(int pageIndex) {
    auto page = std::make_shared<pdf::TextPage>();
    if (m_extractor) {
        m_extractor->ExtractPage(pageIndex, *page);
    }
    return page;
}

// Used by search so a page is extracted at most once per query; the result is
// cached with the same LRU policy as UI-requested pages.
std::shared_ptr<const pdf::TextPage> TextService::EnsurePage(int pageIndex) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto cached = m_cache.find(pageIndex);
        if (cached != m_cache.end()) {
            m_lastUse[pageIndex] = ++m_useCounter;
            return cached->second;
        }
    }

    const std::shared_ptr<const pdf::TextPage> page = ExtractPage(pageIndex);
    if (!page) {
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cache[pageIndex] = page;
        m_lastUse[pageIndex] = ++m_useCounter;
        EvictCacheLocked();
    }
    return page;
}

// Drops least recently used pages until the cache fits its page budget.
void TextService::EvictCacheLocked() {
    // Linear scan over at most kCacheLimit + 1 entries; a real priority queue
    // would not pay for itself at this size.
    while (m_cache.size() > kCacheLimit) {
        auto oldest = m_lastUse.begin();
        for (auto it = m_lastUse.begin(); it != m_lastUse.end(); ++it) {
            if (it->second < oldest->second) {
                oldest = it;
            }
        }
        if (oldest == m_lastUse.end()) {
            m_cache.clear();
            return;
        }
        m_cache.erase(oldest->first);
        m_lastUse.erase(oldest);
    }
}

// PostMessage transfers ownership to the UI message queue; if the window is
// already gone the payload must be freed here.
void TextService::NotifyTextReady(TextReadyMessage* message) {
    // PostMessage takes ownership only on success; delete on shutdown so the
    // UI never leaks a message it cannot receive.
    if (m_notifyWindow == nullptr ||
        ::PostMessageW(m_notifyWindow, WM_APP_TEXT_READY, 0,
                       reinterpret_cast<LPARAM>(message)) == FALSE) {
        delete message;
    }
}

// One page worth of hits for the running query.
void TextService::NotifySearchPage(SearchPageMessage* message) {
    if (m_notifyWindow == nullptr ||
        ::PostMessageW(m_notifyWindow, WM_APP_SEARCH_PAGE, 0,
                       reinterpret_cast<LPARAM>(message)) == FALSE) {
        delete message;
    }
}

// Terminal message for a query; the UI uses it to switch from "searching" to
// the final count (or the empty-result state).
void TextService::NotifySearchDone(SearchDoneMessage* message) {
    if (m_notifyWindow == nullptr ||
        ::PostMessageW(m_notifyWindow, WM_APP_SEARCH_DONE, 0,
                       reinterpret_cast<LPARAM>(message)) == FALSE) {
        delete message;
    }
}

}  // namespace pengpdf
