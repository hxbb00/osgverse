#ifndef VERSE_STRIKE_ECS_H
#define VERSE_STRIKE_ECS_H

#include <cstdint>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ecs
{
    typedef std::uint32_t Entity;
    static const Entity InvalidEntity = 0u;

    /** Component storage base: allows the registry to erase one entity from every pool */
    struct PoolBase
    {
        virtual void erase(Entity e) = 0;
        virtual ~PoolBase() {}
    };

    /** Sparse component pool: at most one T instance per entity */
    template<typename T>
    class Pool : public PoolBase
    {
    public:
        virtual void erase(Entity e) { _map.erase(e); }
        bool has(Entity e) const { return _map.find(e) != _map.end(); }
        T* find(Entity e)
        { auto i = _map.find(e); return i == _map.end() ? NULL : &i->second; }
        const T* find(Entity e) const
        { auto i = _map.find(e); return i == _map.end() ? NULL : &i->second; }
        T& get(Entity e) { return _map.find(e)->second; }

        template<typename... A>
        T& emplace(Entity e, A&&... a)
        {
            _map.erase(e);
            auto r = _map.insert(std::make_pair(e, T{ std::forward<A>(a)... }));
            return r.first->second;
        }

        std::unordered_map<Entity, T>& raw() { return _map; }

    private:
        std::unordered_map<Entity, T> _map;
    };

    /** A minimal registry of entities and components. destroy() only takes effect
        after flush(), so systems may destroy entities safely even while iterating. */
    class Registry
    {
    public:
        Entity create() { Entity e = _next++; _entities.push_back(e); return e; }

        void destroy(Entity e)
        {
            for (size_t i = 0; i < _entities.size(); ++i)
                if (_entities[i] == e) { _pending.push_back(e); return; }
        }

        /** Apply all pending destroys: components are removed and ids retired (never reused) */
        void flush()
        {
            for (size_t i = 0; i < _pending.size(); ++i)
            {
                Entity e = _pending[i];
                for (auto& p : _pools) p.second->erase(e);
                for (auto j = _entities.begin(); j != _entities.end(); ++j)
                    if (*j == e) { _entities.erase(j); break; }
            }
            _pending.clear();
        }

        size_t count() const { return _entities.size(); }

        template<typename T> Pool<T>& pool()
        {
            auto i = _pools.find(std::type_index(typeid(T)));
            if (i != _pools.end()) return *static_cast<Pool<T>*>(i->second.get());
            auto p = std::make_shared<Pool<T>>();
            _pools[std::type_index(typeid(T))] = p; return *p;
        }

        template<typename T> bool has(Entity e) { return pool<T>().has(e); }
        template<typename T> T* find(Entity e) { return pool<T>().find(e); }
        template<typename T> T& get(Entity e) { return pool<T>().get(e); }

        template<typename T, typename... A>
        T& emplace(Entity e, A&&... a) { return pool<T>().emplace(e, std::forward<A>(a)...); }

        /** Iterate over all entities having component T: f(Entity, T&) */
        template<typename T, typename F>
        void view(F&& f)
        {
            Pool<T>& p = pool<T>();
            for (size_t i = 0; i < _entities.size(); ++i)
            {
                T* c = p.find(_entities[i]);
                if (c) f(_entities[i], *c);
            }
        }

    private:
        std::unordered_map<std::type_index, std::shared_ptr<PoolBase>> _pools;
        std::vector<Entity> _entities, _pending;
        Entity _next = InvalidEntity + 1;
    };
}

#endif
