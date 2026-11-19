#ifndef OSGVERSE_VIRTUAL_SHADOW_SCHEDULER_H
#define OSGVERSE_VIRTUAL_SHADOW_SCHEDULER_H

#include "VirtualShadowTypes.h"
#include <osg/Referenced>
#include <queue>
#include <unordered_set>
#include <vector>

namespace osgVerse
{
    /** Priority queue of page render requests. Requests are deduplicated by page key and
        processed with a per-frame budget: the first frame of a scene can request thousands
        of pages, and rendering all of them at once would create a visible stall. Higher
        priority values are processed first. */
    class VirtualShadowScheduler : public osg::Referenced
    {
    public:
        struct Request
        {
            VirtualShadowPageKey key;
            float priority;
            std::uint64_t frame;
            std::uint64_t sequence;
        };

        VirtualShadowScheduler();

        void configure(int maxRequestsPerFrame);
        void clear();

        /** Add a request. Returns false when the page is already queued. */
        bool submit(const VirtualShadowPageKey& key, float priority,
                    std::uint64_t frame = 0);

        /** Remove a request that became unnecessary (for example, the camera moved away). */
        bool cancel(const VirtualShadowPageKey& key);
        bool isQueued(const VirtualShadowPageKey& key) const;

        /** Pop up to 'maxCount' requests, highest priority first. */
        void take(int maxCount, std::vector<Request>& result);
        bool pop(Request& result);

        int getPendingCount() const
        { return (int)(_queued.size() - _cancelled.size()); }
        int getMaxRequestsPerFrame() const { return _maxRequestsPerFrame; }
        void setMaxRequestsPerFrame(int n) { _maxRequestsPerFrame = (n > 0) ? n : 1; }

    protected:
        struct Compare
        {
            bool operator()(const Request& a, const Request& b) const
            {
                if (a.priority != b.priority) return a.priority < b.priority;
                return a.sequence > b.sequence;  // earlier submission wins on ties
            }
        };

        std::priority_queue<Request, std::vector<Request>, Compare> _pending;
        std::unordered_set<std::uint64_t> _queued;
        std::unordered_set<std::uint64_t> _cancelled;  // lazy deletion from the queue
        int _maxRequestsPerFrame;
        std::uint64_t _sequence;
    };
}

#endif
