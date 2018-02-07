/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#pragma once

#include "config.h"

#include "configuration.h"
#include "logger.h"

#include <string>

class Logger;

class KVStoreConfig {
public:
    /**
     * This constructor intialises the object from a central
     * ep-engine Configuration instance.
     */
    KVStoreConfig(Configuration& config, uint16_t shardId);

    /**
     * This constructor sets the mandatory config options
     *
     * Optional config options are set using a separate method
     */
    KVStoreConfig(uint16_t _maxVBuckets,
                  uint16_t _maxShards,
                  const std::string& _dbname,
                  const std::string& _backend,
                  uint16_t _shardId,
                  bool persistDocNamespace,
                  const std::string& rocksDBOptions_ = "",
                  const std::string& rocksDBCFOptions_ = "",
                  const std::string& rocksDbBBTOptions_ = "");

    uint16_t getMaxVBuckets() const {
        return maxVBuckets;
    }

    uint16_t getMaxShards() const {
        return maxShards;
    }

    std::string getDBName() const {
        return dbname;
    }

    const std::string& getBackend() const {
        return backend;
    }

    uint16_t getShardId() const {
        return shardId;
    }

    Logger& getLogger() {
        return *logger;
    }

    /**
     * Indicates whether or not underlying file operations will be
     * buffered by the storage engine used.
     *
     * Only recognised by CouchKVStore
     */
    bool getBuffered() const {
        return buffered;
    }

    /**
     * Used to override the default logger object
     */
    KVStoreConfig& setLogger(Logger& _logger);

    /**
     * Used to override the default buffering behaviour.
     *
     * Only recognised by CouchKVStore
     */
    KVStoreConfig& setBuffered(bool _buffered);

    bool shouldPersistDocNamespace() const {
        return persistDocNamespace;
    }

    void setPersistDocNamespace(bool value) {
        persistDocNamespace = value;
    }

    uint64_t getPeriodicSyncBytes() const {
        return periodicSyncBytes;
    }

    void setPeriodicSyncBytes(uint64_t bytes) {
        periodicSyncBytes = bytes;
    }

    // Following specific to RocksDB.
    // TODO: Move into a RocksDBKVStoreConfig subclass.

    /*
     * Return the RocksDB Database level options.
     */
    const std::string& getRocksDBOptions() {
        return rocksDBOptions;
    }

    /*
     * Return the RocksDB Column Family level options.
     */
    const std::string& getRocksDBCFOptions() {
        return rocksDBCFOptions;
    }

    /*
     * Return the RocksDB Block Based Table options.
     */
    const std::string& getRocksDbBBTOptions() {
        return rocksDbBBTOptions;
    }

    /// Return the RocksDB low priority background thread count.
    size_t getRocksDbLowPriBackgroundThreads() const {
        return rocksDbLowPriBackgroundThreads;
    }

    /// Return the RocksDB high priority background thread count.
    size_t getRocksDbHighPriBackgroundThreads() const {
        return rocksDbHighPriBackgroundThreads;
    }

    /*
     * Return the RocksDB Statistics 'stats_level'.
     */
    const std::string& getRocksdbStatsLevel() {
        return rocksdbStatsLevel;
    }

    // Return the Bucket Quota
    size_t getBucketQuota() {
        return bucketQuota;
    }

    /*
     * Return the RocksDB Block Cache ratio of the Bucket Quota.
     */
    float getRocksdbBlockCacheRatio() {
        return rocksdbBlockCacheRatio;
    }

    // RocksDB ratio of the BlockCache quota reserved for index/filter blocks
    float getRocksdbBlockCacheHighPriPoolRatio() {
        return rocksdbBlockCacheHighPriPoolRatio;
    }

    // Return the RocksDB total Memtables ratio of the Bucket Quota
    float getRocksdbMemtablesRatio() {
        return rocksdbMemtablesRatio;
    }

    // Return the RocksDB Compaction Optimization type for the 'default' CF
    std::string getRocksdbDefaultCfOptimizeCompaction() {
        return rocksdbDefaultCfOptimizeCompaction;
    }

    // Return the RocksDB Compaction Optimization type for the 'seqno' CF
    std::string getRocksdbSeqnoCfOptimizeCompaction() {
        return rocksdbSeqnoCfOptimizeCompaction;
    }

private:
    class ConfigChangeListener;

    uint16_t maxVBuckets;
    uint16_t maxShards;
    std::string dbname;
    std::string backend;
    uint16_t shardId;
    Logger* logger;
    bool buffered;
    bool persistDocNamespace;

    /**
     * If non-zero, tell storage layer to issue a sync() operation after every
     * N bytes written.
     */
    uint64_t periodicSyncBytes;

    // Amount of memory reserved for the bucket.
    size_t bucketQuota = 0;

    // RocksDB Database level options. Semicolon-separated `<option>=<value>`
    // pairs.
    std::string rocksDBOptions;
    // RocksDB Column Family level options. Semicolon-separated
    // `<option>=<value>` pairs.
    std::string rocksDBCFOptions;
    // RocksDB Block Based Table options. Semicolon-separated
    // `<option>=<value>` pairs.
    std::string rocksDbBBTOptions;

    /// RocksDB low priority background thread count.
    size_t rocksDbLowPriBackgroundThreads = 0;

    /// RocksDB high priority background thread count.
    size_t rocksDbHighPriBackgroundThreads = 0;

    // RocksDB Statistics 'stats_level'. Possible values:
    // {'', 'kAll', 'kExceptTimeForMutex', 'kExceptDetailedTimers'}
    std::string rocksdbStatsLevel;

    // RocksDB Block Cache ratio of the Bucket Quota
    float rocksdbBlockCacheRatio = 0.0;

    // RocksDB ratio of the BlockCache quota reserved for index/filter blocks
    float rocksdbBlockCacheHighPriPoolRatio = 0.0;

    // RocksDB total Memtables ratio of the Bucket Quota.
    // This ratio represents the total quota of memory allocated for the
    // Memtables of all Column Families. The logic in 'RocksDBKVStore' decides
    // how this quota is split among different CFs. If this ratio is set to
    // 0.0, then we set each Memtable size to a baseline value.
    float rocksdbMemtablesRatio = 0.0;

    // RocksDB flag to enable Compaction Optimization for the 'default' CF
    std::string rocksdbDefaultCfOptimizeCompaction;

    // RocksDB flag to enable Compaction Optimization for the 'seqno' CF
    std::string rocksdbSeqnoCfOptimizeCompaction;
};
