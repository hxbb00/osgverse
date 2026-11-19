#include "VirtualShadowScheduler.h"

namespace osgVerse
{
    VirtualShadowScheduler::VirtualShadowScheduler()
    :   _maxRequestsPerFrame(4), _sequence(0)
    {}

    void VirtualShadowScheduler::configure(int maxRequestsPerFrame)
    {
        setMaxRequestsPerFrame(maxRequestsPerFrame);
        clear();
    }

    void VirtualShadowScheduler::clear()
    {
        while (!_pending.empty()) _pending.pop();
        _queued.clear(); _cancelled.clear();
        _sequence = 0;
    }

    bool VirtualShadowScheduler::submit(const VirtualShadowPageKey& key,
                                        float priority, std::uint64_t frame)
    {
        std::uint64_t packed = packVirtualShadowPageKey(key);
        if (_queued.find(packed) != _queued.end()) return false;

        Request request;
        request.key = key; request.priority = priority;
        request.frame = frame; request.sequence = _sequence++;
        _pending.push(request); _queued.insert(packed);
        _cancelled.erase(packed);
        return true;
    }

    bool VirtualShadowScheduler::cancel(const VirtualShadowPageKey& key)
    {
        std::uint64_t packed = packVirtualShadowPageKey(key);
        std::unordered_set<std::uint64_t>::iterator itr = _queued.find(packed);
        if (itr == _queued.end()) return false;

        _queued.erase(itr);
        _cancelled.insert(packed);
        return true;
    }

    bool VirtualShadowScheduler::isQueued(const VirtualShadowPageKey& key) const
    {
        std::uint64_t packed = packVirtualShadowPageKey(key);
        return _queued.find(packed) != _queued.end() &&
               _cancelled.find(packed) == _cancelled.end();
    }

    void VirtualShadowScheduler::take(int maxCount, std::vector<Request>& result)
    {
        result.clear();
        std::uint64_t keepSequence = _sequence;
        for (int i = 0; i < maxCount; ++i)
        {
            Request request;
            if (!pop(request)) break;
            result.push_back(request);
        }
        _sequence = keepSequence;  // taking requests must not renumber later submissions
    }

    bool VirtualShadowScheduler::pop(Request& result)
    {
        while (!_pending.empty())
        {
            Request request = _pending.top(); _pending.pop();
            std::uint64_t packed = packVirtualShadowPageKey(request.key);

            std::unordered_set<std::uint64_t>::iterator citr = _cancelled.find(packed);
            if (citr != _cancelled.end())
            {
                // The item was cancelled while it was still in the heap
                _cancelled.erase(citr); _queued.erase(packed);
                continue;
            }

            std::unordered_set<std::uint64_t>::iterator qitr = _queued.find(packed);
            if (qitr == _queued.end()) continue;  // already consumed

            _queued.erase(qitr);
            result = request;
            return true;
        }
        return false;
    }
}
