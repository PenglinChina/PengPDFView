// =============================================================================
//  TextService.h
//  Background text extraction and search.
//
//  Extracting text and searching hundreds of pages must never block the UI
//  thread, so this service owns a worker thread with its own PDF document
//  handle.  Results are delivered through window messages, exactly like the
//  render service.  A small LRU cache keeps the most recently used pages so
//  selection is instant when the user revisits a page.
//
//  Message payloads are heap allocated and owned by the receiver:
//    WM_APP_TEXT_READY   -> TextReadyMessage*
//    WM_APP_SEARCH_PAGE  -> SearchPageMessage*
//    WM_APP_SEARCH_DONE  -> SearchDoneMessage*
// =============================================================================
#pragma once

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Pdf/PdfParser.h"
#include "Pdf/PdfTextExtractor.h"
#include "Pdf/PdfTextTypes.h"

namespace pengpdf {

inline constexpr UINT WM_APP_TEXT_READY = WM_APP + 140;
inline constexpr UINT WM_APP_SEARCH_PAGE = WM_APP + 141;
inline constexpr UINT WM_APP_SEARCH_DONE = WM_APP + 142;

// One extracted page.  The shared pointer keeps the page alive while the UI
// uses it even if the cache evicts it in the meantime.
struct TextReadyMessage {
    uint64_t documentToken = 0;
    int pageIndex = 0;
    std::shared_ptr<const pdf::TextPage> page;
};

// Search results for one page.  `searchId` lets the UI discard results of a
// query the user has already replaced.
struct SearchPageMessage {
    uint64_t searchId = 0;
    int pageIndex = 0;
    std::vector<pdf::TextMatch> matches;
};

struct SearchDoneMessage {
    uint64_t searchId = 0;
    int pageCount = 0;
    int matchCount = 0;
    bool cancelled = false;
};

class TextService {
public:
    // `notifyWindow` receives the messages above and must outlive the service.
    explicit TextService(HWND notifyWindow);
    ~TextService();

    TextService(const TextService&) = delete;
    TextService& operator=(const TextService&) = delete;

    // Starts using a parsed document.  The service keeps the shared pointer
    // alive on its worker thread until CloseDocument is called.
    void OpenDocument(std::shared_ptr<pdf::Document> document, uint64_t token);
    void CloseDocument();

    // Extracts a page in the background when it is not cached yet.
    void RequestPage(int pageIndex);

    // Returns the cached page without blocking; null when it has not been
    // extracted yet.  Accessing a page refreshes its LRU position.
    std::shared_ptr<const pdf::TextPage> CachedPage(int pageIndex);

    // Starts a new search.  Older searches are cancelled implicitly.  The
    // returned id is echoed in the result messages.  The query is normalised
    // (whitespace collapsed, lower-cased) before the first page is searched.
    uint64_t StartSearch(const std::wstring& query);

    // Cancels the running search and drops queued search work.
    void CancelSearch();

private:
    struct Job {
        enum class Kind { Open, Close, Extract, Search, Quit };
        Kind kind = Kind::Quit;
        // Only used by Open; keeps the document alive until the worker sees it.
        std::shared_ptr<pdf::Document> document;
        uint64_t documentToken = 0;
        int pageIndex = 0;
        uint64_t searchId = 0;
        // Already normalised for Search jobs (see StartSearch).
        std::wstring query;
    };

    void WorkerMain();
    void HandleOpen(Job& job);
    void HandleExtract(const Job& job);
    void HandleSearch(const Job& job);
    void QueueSearchPage(uint64_t searchId, const std::wstring& query, int pageIndex);

    // Extracts a page with the worker's extractor (never returns null; a page
    // without text is represented by an empty TextPage).
    std::shared_ptr<const pdf::TextPage> ExtractPage(int pageIndex);
    // Returns the cached page, extracting it synchronously when necessary.
    std::shared_ptr<const pdf::TextPage> EnsurePage(int pageIndex);
    // Drops least recently used pages until the cache fits its budget.
    void EvictCacheLocked();

    // Message helpers; they delete the payload when PostMessage fails.
    void NotifyTextReady(TextReadyMessage* message);
    void NotifySearchPage(SearchPageMessage* message);
    void NotifySearchDone(SearchDoneMessage* message);

    HWND m_notifyWindow = nullptr;

    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    std::deque<Job> m_jobs;
    bool m_stopping = false;

    // Shared state guarded by m_mutex.
    std::shared_ptr<pdf::Document> m_document;
    uint64_t m_documentToken = 0;
    uint64_t m_activeSearchId = 0;
    uint64_t m_searchCounter = 0;
    std::map<int, std::shared_ptr<const pdf::TextPage>> m_cache;
    std::map<int, uint64_t> m_lastUse;
    std::set<int> m_pendingPages;
    uint64_t m_useCounter = 0;

    // Worker thread state (only touched by WorkerMain and its helpers).
    std::shared_ptr<pdf::Document> m_workerDocument;
    std::unique_ptr<pdf::TextExtractor> m_extractor;
    uint64_t m_workerToken = 0;
    int m_workerPageCount = 0;
    uint64_t m_searchMatchCount = 0;
    uint64_t m_searchIdForCount = 0;
};

}  // namespace pengpdf
