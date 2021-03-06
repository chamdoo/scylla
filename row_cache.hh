/*
 * Copyright (C) 2015 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <boost/intrusive/list.hpp>
#include <boost/intrusive/set.hpp>

#include "core/memory.hh"
#include <seastar/core/thread.hh>

#include "mutation_reader.hh"
#include "mutation_partition.hh"
#include "utils/logalloc.hh"
#include "key_reader.hh"
#include "utils/phased_barrier.hh"
#include "utils/histogram.hh"
#include "partition_version.hh"

namespace scollectd {

struct registrations;

}

namespace bi = boost::intrusive;

class row_cache;

// Intrusive set entry which holds partition data.
//
// TODO: Make memtables use this format too.
class cache_entry {
    // We need auto_unlink<> option on the _cache_link because when entry is
    // evicted from cache via LRU we don't have a reference to the container
    // and don't want to store it with each entry. As for the _lru_link, we
    // have a global LRU, so technically we could not use auto_unlink<> on
    // _lru_link, but it's convenient to do so too. We may also want to have
    // multiple eviction spaces in the future and thus multiple LRUs.
    using lru_link_type = bi::list_member_hook<bi::link_mode<bi::auto_unlink>>;
    using cache_link_type = bi::set_member_hook<bi::link_mode<bi::auto_unlink>>;

    schema_ptr _schema;
    dht::ring_position _key;
    partition_entry _pe;
    // True when we know that there is nothing between this entry and the next one in cache
    bool _continuous;
    lru_link_type _lru_link;
    cache_link_type _cache_link;
    friend class size_calculator;
public:
    friend class row_cache;
    friend class cache_tracker;

    cache_entry(schema_ptr s)
        : _schema(std::move(s))
        , _key(dht::ring_position::starting_at(dht::minimum_token()))
        , _pe(_schema)
        , _continuous(false)
    { }

    cache_entry(schema_ptr s, const dht::decorated_key& key, const mutation_partition& p, bool continuous = false)
        : _schema(std::move(s))
        , _key(key)
        , _pe(p)
        , _continuous(continuous)
    { }

    cache_entry(schema_ptr s, dht::decorated_key&& key, mutation_partition&& p, bool continuous = false) noexcept
        : _schema(std::move(s))
        , _key(std::move(key))
        , _pe(std::move(p))
        , _continuous(continuous)
    { }

    cache_entry(schema_ptr s, dht::decorated_key&& key, partition_entry&& pe, bool continuous = false) noexcept
        : _schema(std::move(s))
        , _key(std::move(key))
        , _pe(std::move(pe))
        , _continuous(continuous)
    { }

    cache_entry(cache_entry&&) noexcept;

    bool is_evictable() { return _lru_link.is_linked(); }
    const dht::ring_position& key() const { return _key; }
    const partition_entry& partition() const { return _pe; }
    partition_entry& partition() { return _pe; }
    const schema_ptr& schema() const { return _schema; }
    schema_ptr& schema() { return _schema; }
    streamed_mutation read(row_cache&, const schema_ptr&);
    streamed_mutation read(row_cache&, const schema_ptr&, query::clustering_key_filtering_context);
    bool continuous() const { return _continuous; }
    void set_continuous(bool value) { _continuous = value; }

    struct compare {
        dht::ring_position_less_comparator _c;

        compare(schema_ptr s)
            : _c(*s)
        {}

        bool operator()(const dht::decorated_key& k1, const cache_entry& k2) const {
            return _c(k1, k2._key);
        }

        bool operator()(const dht::ring_position& k1, const cache_entry& k2) const {
            return _c(k1, k2._key);
        }

        bool operator()(const cache_entry& k1, const cache_entry& k2) const {
            return _c(k1._key, k2._key);
        }

        bool operator()(const cache_entry& k1, const dht::decorated_key& k2) const {
            return _c(k1._key, k2);
        }

        bool operator()(const cache_entry& k1, const dht::ring_position& k2) const {
            return _c(k1._key, k2);
        }
    };
};

// Tracks accesses and performs eviction of cache entries.
class cache_tracker final {
public:
    using lru_type = bi::list<cache_entry,
        bi::member_hook<cache_entry, cache_entry::lru_link_type, &cache_entry::_lru_link>,
        bi::constant_time_size<false>>; // we need this to have bi::auto_unlink on hooks.
private:
    uint64_t _hits = 0;
    uint64_t _misses = 0;
    uint64_t _uncached_wide_partitions = 0;
    uint64_t _insertions = 0;
    uint64_t _merges = 0;
    uint64_t _evictions = 0;
    uint64_t _removals = 0;
    uint64_t _partitions = 0;
    uint64_t _modification_count = 0;
    uint64_t _continuity_flags_cleared = 0;
    std::unique_ptr<scollectd::registrations> _collectd_registrations;
    logalloc::region _region;
    lru_type _lru;
private:
    void setup_collectd();
public:
    cache_tracker();
    ~cache_tracker();
    void clear();
    void touch(cache_entry&);
    void insert(cache_entry&);
    void clear_continuity(cache_entry& ce);
    void on_erase();
    void on_merge();
    void on_hit();
    void on_miss();
    void on_uncached_wide_partition();
    void on_continuity_flag_cleared();
    allocation_strategy& allocator();
    logalloc::region& region();
    const logalloc::region& region() const;
    uint64_t modification_count() const { return _modification_count; }
    uint64_t partitions() const { return _partitions; }
    uint64_t uncached_wide_partitions() const { return _uncached_wide_partitions; }
    uint64_t continuity_flags_cleared() const { return _continuity_flags_cleared; }
};

// Returns a reference to shard-wide cache_tracker.
cache_tracker& global_cache_tracker();

//
// A data source which wraps another data source such that data obtained from the underlying data source
// is cached in-memory in order to serve queries faster.
//
// To query the underlying data source through cache, use make_reader().
//
// Cache populates itself automatically during misses.
//
// Cache needs to be maintained externally so that it remains consistent with the underlying data source.
// Any incremental change to the underlying data source should result in update() being called on cache.
//
class row_cache final {
public:
    using partitions_type = bi::set<cache_entry,
        bi::member_hook<cache_entry, cache_entry::cache_link_type, &cache_entry::_cache_link>,
        bi::constant_time_size<false>, // we need this to have bi::auto_unlink on hooks
        bi::compare<cache_entry::compare>>;
    friend class single_partition_populating_reader;
    friend class cache_entry;
public:
    struct stats {
        utils::timed_rate_moving_average hits;
        utils::timed_rate_moving_average misses;
    };
private:
    cache_tracker& _tracker;
    stats _stats{};
    schema_ptr _schema;
    partitions_type _partitions; // Cached partitions are complete.
    mutation_source _underlying;
    key_source _underlying_keys;
    uint64_t _max_cached_partition_size_in_bytes;

    // Synchronizes populating reads with updates of underlying data source to ensure that cache
    // remains consistent across flushes with the underlying data source.
    // Readers obtained from the underlying data source in earlier than
    // current phases must not be used to populate the cache, unless they hold
    // phaser::operation created in the reader's phase of origin. Readers
    // should hold to a phase only briefly because this inhibits progress of
    // updates. Phase changes occur in update()/clear(), which can be assumed to
    // be asynchronous wrt invoking of the underlying data source.
    utils::phased_barrier _populate_phaser;

    logalloc::allocating_section _update_section;
    logalloc::allocating_section _populate_section;
    logalloc::allocating_section _read_section;
    mutation_reader make_scanning_reader(schema_ptr,
                                         const query::partition_range&,
                                         const io_priority_class& pc,
                                         query::clustering_key_filtering_context ck_filtering);
    void on_hit();
    void on_miss();
    void on_uncached_wide_partition();
    void upgrade_entry(cache_entry&);
    void invalidate_locked(const dht::decorated_key&);
    void invalidate_unwrapped(const query::partition_range&);
    void clear_now() noexcept;
    static thread_local seastar::thread_scheduling_group _update_thread_scheduling_group;
public:
    ~row_cache();
    row_cache(schema_ptr, mutation_source underlying, key_source, cache_tracker&, uint64_t _max_cached_partition_size_in_bytes = 10 * 1024 * 1024);
    row_cache(row_cache&&) = default;
    row_cache(const row_cache&) = delete;
    row_cache& operator=(row_cache&&) = default;
public:
    // Implements mutation_source for this cache, see mutation_reader.hh
    // User needs to ensure that the row_cache object stays alive
    // as long as the reader is used.
    // The range must not wrap around.
    mutation_reader make_reader(schema_ptr,
                                const query::partition_range& = query::full_partition_range,
                                query::clustering_key_filtering_context = query::no_clustering_key_filtering,
                                const io_priority_class& = default_priority_class());

    const stats& stats() const { return _stats; }
public:
    // Populate cache from given mutation. The mutation must contain all
    // information there is for its partition in the underlying data sources.
    void populate(const mutation& m);

    // Clears the cache.
    // Guarantees that cache will not be populated using readers created
    // before this method was invoked.
    future<> clear();

    // Synchronizes cache with the underlying data source from a memtable which
    // has just been flushed to the underlying data source.
    // The memtable can be queried during the process, but must not be written.
    // After the update is complete, memtable is empty.
    future<> update(memtable&, partition_presence_checker underlying_negative);

    // Moves given partition to the front of LRU if present in cache.
    void touch(const dht::decorated_key&);

    // Removes given partition from cache.
    //
    // Guarantees that cache will not be populated with given key
    // using readers created before this method was invoked.
    //
    // The key must be kept alive until method resolves.
    future<> invalidate(const dht::decorated_key& key);

    // Removes given range of partitions from cache.
    // The range can be a wrap around.
    //
    // Guarantees that cache will not be populated with partitions from that range
    // using readers created before this method was invoked.
    //
    // The range must be kept alive until method resolves.
    future<> invalidate(const query::partition_range&);

    bool has_continuous_entry(const dht::ring_position& key) const;

    auto num_entries() const {
        return _partitions.size();
    }
    const cache_tracker& get_cache_tracker() const {
        return _tracker;
    }

    void set_schema(schema_ptr) noexcept;
    const schema_ptr& schema() const;

    friend class just_cache_scanning_reader;
    friend class scanning_and_populating_reader;
    friend class range_populating_reader;
    friend class cache_tracker;
    friend class mark_end_as_continuous;
};
