// =============================================================================
//  PdfOutline.cpp
//  Outline (bookmark) extraction.
//
//  The PDF outline is a linked tree:
//
//      /Outlines << /First <item> /Last <item> >>
//      item      << /Title (...) /Dest [...] /A << /S /GoTo /D [...] >>
//                   /Parent <item> /First <item> /Next <item> /Count n >>
//
//  Each item links to a destination which is either an explicit array pointing
//  at a page object, a named destination resolved through the name tree, or an
//  action dictionary carrying the array under /D.
// =============================================================================
#include "Pdf/PdfOutline.h"

#include <string>
#include <unordered_map>
#include <unordered_set>

#include "Core/Log.h"
#include "Pdf/PdfTypes.h"

namespace pdf {

using pengpdf::LogInfo;

namespace {

// Budgets.  A sidebar with 20000 entries is already unusable, and the bounds
// make the walk linear in the file size no matter how the tree is shaped.
constexpr int kMaxDepth = 32;
constexpr int kMaxItems = 20000;
constexpr int kMaxNameTreeDepth = 16;

class OutlineWalker {
public:
    explicit OutlineWalker(Document& document) : m_document(document) {}

    std::vector<OutlineEntry> Build();

private:
    // Walks the /Next chain of one level and recurses into children.  Entries
    // are appended parent-first, which is the order the sidebar displays.
    void WalkSiblings(const Object& firstItem, int level, std::vector<OutlineEntry>& entries);

    // Resolves a destination value to a zero based page index (-1 on failure).
    int ResolveDestination(const Object& destination, int depth);

    // Looks up a named destination (name trees and the legacy /Dests entry).
    bool LookupNamedDestination(const Object& key, Object& destination);
    void LoadNamedDestinations();
    void CollectNameTree(const Object& nodeReference, int depth);
    Object Catalog();

    Document& m_document;
    // Named destinations are collected on first use because most documents have
    // none and the name tree can be large.
    std::unordered_map<std::string, Object> m_namedDestinations;
    bool m_namedDestinationsLoaded = false;
    // Object numbers already emitted; a /Next chain that loops back must stop.
    std::unordered_set<int> m_visitedItems;
    int m_itemCount = 0;
};

Object OutlineWalker::Catalog() {
    // TrailerItem returns a pointer into the immutable trailer; the resolved
    // catalog is a copy (a dictionary shared with the object cache).
    const Object* rootEntry = m_document.TrailerItem("Root");
    if (rootEntry == nullptr) {
        return Object();
    }
    return m_document.Resolve(*rootEntry);
}

std::vector<OutlineEntry> OutlineWalker::Build() {
    std::vector<OutlineEntry> entries;

    // A missing catalog or outline is normal (scanned documents have neither);
    // every step returns an empty list instead of raising an error.
    const Object catalog = Catalog();
    if (!catalog.IsDict()) {
        return entries;
    }
    const Object* outlinesEntry = catalog.Get("Outlines");
    if (outlinesEntry == nullptr) {
        return entries;
    }
    const Object outlines = m_document.Resolve(*outlinesEntry);
    if (!outlines.IsDict()) {
        return entries;
    }
    const Object* firstEntry = outlines.Get("First");
    if (firstEntry == nullptr) {
        return entries;
    }

    WalkSiblings(*firstEntry, 0, entries);
    LogInfo(L"Outline: %d entries", static_cast<int>(entries.size()));
    return entries;
}

void OutlineWalker::WalkSiblings(const Object& firstItem, int level,
                                 std::vector<OutlineEntry>& entries) {
    Object current = firstItem;
    // A second guard: `guard` limits one /Next chain, m_itemCount limits the
    // whole tree including all nested levels.
    int guard = 0;

    while (!current.IsNull() && guard++ < 10000 && m_itemCount < kMaxItems) {
        // Protect against /Next chains that point back at an earlier item.
        if (current.IsRef() && !m_visitedItems.insert(current.refNum).second) {
            break;
        }

        const Object item = m_document.Resolve(current);
        if (!item.IsDict()) {
            break;
        }

        OutlineEntry entry;
        entry.level = level;

        // /Title is a PDF text string, so it may be UTF-16BE with a BOM.
        const Object* titleEntry = item.Get("Title");
        if (titleEntry != nullptr) {
            const Object title = m_document.Resolve(*titleEntry);
            if (title.IsString()) {
                entry.title = DecodeTextString(title.text);
            }
        }
        if (entry.title.empty()) {
            // Keep the entry visible even without a title: it still navigates.
            entry.title = L"(untitled)";
        }

        // Resolve the link target.  A /Dest entry wins over an action.
        const Object* destinationEntry = item.Get("Dest");
        if (destinationEntry != nullptr) {
            entry.pageIndex = ResolveDestination(*destinationEntry, 0);
        } else {
            const Object* actionEntry = item.Get("A");
            if (actionEntry != nullptr) {
                const Object action = m_document.Resolve(*actionEntry);
                if (action.IsDict()) {
                    const Object* actionDestination = action.Get("D");
                    if (actionDestination != nullptr) {
                        entry.pageIndex = ResolveDestination(*actionDestination, 0);
                    }
                }
            }
        }

        const Object* childEntry = item.Get("First");
        // /First is the expandability signal; if the child walk later yields
        // nothing the sidebar simply shows a closed, empty branch.
        entry.hasChildren = childEntry != nullptr;

        // A negative /Count means the branch is collapsed by default.
        const Object* countEntry = item.Get("Count");
        if (countEntry != nullptr) {
            const Object count = m_document.Resolve(*countEntry);
            if (count.IsNumber() && count.Integer() < 0) {
                entry.initiallyOpen = false;
            }
        }

        entries.push_back(entry);
        ++m_itemCount;

        if (childEntry != nullptr && level + 1 < kMaxDepth) {
            // Deeper items are dropped rather than flattened into the wrong
            // level, which would make the sidebar misleading.
            WalkSiblings(*childEntry, level + 1, entries);
        }

        // Follow the sibling chain.  The loop condition also stops on a null
        // /Next, which some writers use to terminate the list explicitly.
        const Object* nextEntry = item.Get("Next");
        if (nextEntry == nullptr) {
            break;
        }
        current = *nextEntry;
    }
}

int OutlineWalker::ResolveDestination(const Object& destination, int depth) {
    // Named destinations may point at other named destinations, so resolution is
    // recursive; the cap turns a loop (A -> B -> A) into "unresolved".
    if (depth > 8) {
        return -1;
    }

    const Object value = m_document.Resolve(destination);

    if (value.IsArray() && !value.items.empty()) {
        // Explicit destination: [page /Fit], [page /XYZ left top zoom] ...  Only
        // the first element identifies the page; the view parameters are
        // irrelevant for a sidebar jump.
        const Object& target = value.items.front();

        // The usual case: an indirect reference to the page object.
        if (target.IsRef()) {
            const int pageIndex = m_document.PageIndexForObject(target.refNum);
            if (pageIndex >= 0) {
                return pageIndex;
            }
        }

        const Object directTarget = m_document.Resolve(target);
        if (directTarget.IsNumber()) {
            // Some producers store a plain page number in the destination.
            const int pageIndex = static_cast<int>(directTarget.Integer());
            if (pageIndex >= 0 && pageIndex < m_document.PageCount()) {
                return pageIndex;
            }
        }
        return -1;
    }

    // A named destination: /Dest /ChapterOne or /Dest (ChapterOne).
    if (value.IsName() || value.IsString()) {
        Object resolvedDestination;
        if (LookupNamedDestination(value, resolvedDestination)) {
            return ResolveDestination(resolvedDestination, depth + 1);
        }
        return -1;
    }

    // An action-like dictionary carrying the destination under /D.
    if (value.IsDict()) {
        const Object* innerDestination = value.Get("D");
        if (innerDestination != nullptr) {
            return ResolveDestination(*innerDestination, depth + 1);
        }
    }

    return -1;
}

bool OutlineWalker::LookupNamedDestination(const Object& key, Object& destination) {
    // Keys are compared as raw bytes: the name tree stores either a name or a
    // string, and the reference in the outline uses the same spelling.
    LoadNamedDestinations();
    const auto it = m_namedDestinations.find(key.text);
    if (it == m_namedDestinations.end()) {
        return false;
    }
    destination = it->second;
    return true;
}

void OutlineWalker::LoadNamedDestinations() {
    if (m_namedDestinationsLoaded) {
        return;
    }
    // Set the flag before doing the work: recursive lookups triggered while the
    // tree is being read must not start a second collection pass.
    m_namedDestinationsLoaded = true;

    const Object catalog = Catalog();
    if (!catalog.IsDict()) {
        return;
    }

    // Modern documents keep destinations in a name tree below /Names /Dests.
    const Object* namesEntry = catalog.Get("Names");
    if (namesEntry != nullptr) {
        const Object names = m_document.Resolve(*namesEntry);
        if (names.IsDict()) {
            const Object* destsEntry = names.Get("Dests");
            if (destsEntry != nullptr) {
                CollectNameTree(*destsEntry, 0);
            }
        }
    }

    // Older documents use a plain dictionary /Dests at the catalog level.  The
    // values are stored unresolved (usually arrays) and only interpreted when a
    // bookmark actually references them.
    const Object* legacyDests = catalog.Get("Dests");
    if (legacyDests != nullptr) {
        const Object dests = m_document.Resolve(*legacyDests);
        if (dests.IsDict() && dests.dict != nullptr) {
            for (const auto& item : *dests.dict) {
                m_namedDestinations.emplace(item.first, item.second);
            }
        }
    }

    LogInfo(L"Named destinations: %d", static_cast<int>(m_namedDestinations.size()));
}

void OutlineWalker::CollectNameTree(const Object& nodeReference, int depth) {
    // The depth cap covers cyclic /Kids arrays and trees that are deeper than the
    // specification allows.
    if (depth > kMaxNameTreeDepth) {
        return;
    }
    const Object node = m_document.Resolve(nodeReference);
    if (!node.IsDict()) {
        return;
    }

    // A leaf node stores a flat array alternating key and value.  An odd trailing
    // element is ignored because there is no value to pair it with.
    const Object* namesEntry = node.Get("Names");
    if (namesEntry != nullptr) {
        const Object names = m_document.Resolve(*namesEntry);
        if (names.IsArray()) {
            for (size_t index = 0; index + 1 < names.items.size(); index += 2) {
                const Object key = m_document.Resolve(names.items[index]);
                if (key.IsString() || key.IsName()) {
                    // emplace keeps the first definition when a damaged file
                    // repeats a key; either value is equally arbitrary.
                    m_namedDestinations.emplace(key.text, names.items[index + 1]);
                }
            }
        }
    }

    // Intermediate node: keys are distributed over the children by range, so the
    // whole tree has to be walked.
    const Object* kidsEntry = node.Get("Kids");
    if (kidsEntry != nullptr) {
        const Object kids = m_document.Resolve(*kidsEntry);
        if (kids.IsArray()) {
            for (const Object& kid : kids.items) {
                CollectNameTree(kid, depth + 1);
            }
        }
    }
}

}  // namespace

std::vector<OutlineEntry> BuildOutline(Document& document) {
    // One walker per call: the visited sets and named destination cache are only
    // meaningful for a single traversal.
    OutlineWalker walker(document);
    return walker.Build();
}

}  // namespace pdf
