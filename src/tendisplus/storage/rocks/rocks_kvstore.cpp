#include <memory>
#include <utility>
#include <string>
#include <sstream>
#include <map>
#include <vector>
#include "glog/logging.h"
#include "rocksdb/table.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/utilities/backupable_db.h"
#include "rocksdb/utilities/checkpoint.h"
#include "rocksdb/options.h"
#include "tendisplus/storage/rocks/rocks_kvstore.h"
#include "tendisplus/utils/sync_point.h"
#include "tendisplus/utils/scopeguard.h"
#include "tendisplus/utils/invariant.h"
#include "tendisplus/utils/time.h"

namespace tendisplus {

RocksKVCursor::RocksKVCursor(std::unique_ptr<rocksdb::Iterator> it)
        :Cursor(),
         _it(std::move(it)) {
    _it->Seek("");
}

void RocksKVCursor::seek(const std::string& prefix) {
    _it->Seek(rocksdb::Slice(prefix.c_str(), prefix.size()));
}

void RocksKVCursor::seekToLast() {
    _it->SeekToLast();
}

Expected<Record> RocksKVCursor::next() {
    if (!_it->status().ok()) {
        return {ErrorCodes::ERR_INTERNAL, _it->status().ToString()};
    }
    if (!_it->Valid()) {
        return {ErrorCodes::ERR_EXHAUST, "no more data"};
    }
    const std::string& key = _it->key().ToString();
    const std::string& val = _it->value().ToString();
    auto result = Record::decode(key, val);
    _it->Next();
    if (result.ok()) {
        return std::move(result.value());
    }
    return result.status();
}

RocksOptTxn::RocksOptTxn(RocksKVStore* store, uint64_t txnId)
        :_txnId(txnId),
         _txn(nullptr),
         _store(store),
         _done(false) {
}

std::unique_ptr<BinlogCursor> RocksOptTxn::createBinlogCursor(uint64_t begin) {
    ensureTxn();
    auto cursor = createCursor();

    uint64_t hv = _store->getHighestVisibleTxnId();
    return std::make_unique<BinlogCursor>(std::move(cursor), begin, hv);
}

std::unique_ptr<Cursor> RocksOptTxn::createCursor() {
    ensureTxn();
    rocksdb::ReadOptions readOpts;
    readOpts.snapshot =  _txn->GetSnapshot();
    rocksdb::Iterator* iter = _txn->GetIterator(readOpts);
    return std::unique_ptr<Cursor>(
        new RocksKVCursor(
            std::move(std::unique_ptr<rocksdb::Iterator>(iter))));
}

Expected<uint64_t> RocksOptTxn::commit() {
    INVARIANT(!_done);
    _done = true;

    const auto guard = MakeGuard([this] {
        _txn.reset();
        _store->markCommitted(_txnId);
    });

    if (_txn == nullptr) {
        return {ErrorCodes::ERR_OK, ""};
    }

    if (_binlogs.size() != 0) {
        ReplLogKey& key = _binlogs[_binlogs.size()-1].getReplLogKey();
        uint16_t oriFlag = static_cast<uint16_t>(key.getFlag());
        oriFlag |= static_cast<uint16_t>(ReplFlag::REPL_GROUP_END);
        key.setFlag(static_cast<ReplFlag>(oriFlag));
    }
    for (auto& v : _binlogs) {
        auto strPair = v.encode();
        auto s = _txn->Put(strPair.first, strPair.second);
        if (!s.ok()) {
            return {ErrorCodes::ERR_INTERNAL, s.ToString()};
        }
    }
    TEST_SYNC_POINT("RocksOptTxn::commit()::1");
    TEST_SYNC_POINT("RocksOptTxn::commit()::2");
    auto s = _txn->Commit();
    if (s.ok()) {
        return _txnId;
    } else if (s.IsBusy() || s.IsTryAgain()) {
        return {ErrorCodes::ERR_COMMIT_RETRY, s.ToString()};
    } else {
        return {ErrorCodes::ERR_INTERNAL, s.ToString()};
    }
}

Status RocksOptTxn::rollback() {
    INVARIANT(!_done);
    _done = true;

    const auto guard = MakeGuard([this] {
        _txn.reset();
        _store->markCommitted(_txnId);
    });

    if (_txn == nullptr) {
        return {ErrorCodes::ERR_OK, ""};
    }
    auto s = _txn->Rollback();
    if (s.ok()) {
        return {ErrorCodes::ERR_OK, ""};
    } else {
        return {ErrorCodes::ERR_INTERNAL, s.ToString()};
    }
}

void RocksOptTxn::ensureTxn() {
    INVARIANT(!_done);
    if (_txn != nullptr) {
        return;
    }
    rocksdb::WriteOptions writeOpts;
    rocksdb::OptimisticTransactionOptions txnOpts;

    // NOTE(deyukong): the optimistic_txn won't save a snapshot
    // (mainly for read in our cases) automaticly.
    // We must set_snapshot manually.
    // if set_snapshot == false, the RC-level is guaranteed.
    // if set_snapshot == true, the RR-level is guaranteed.
    // Of course we need RR-level, not RC-level.

    // refer to rocks' document, even if set_snapshot == true,
    // the uncommitted data in this txn's writeBatch are still
    // visible to reads, and this behavior is what we need.
    txnOpts.set_snapshot = true;
    auto db = _store->getUnderlayerDB();
    if (!db) {
        LOG(FATAL) << "BUG: rocksKVStore underLayerDB nil";
    }
    _txn.reset(db->BeginTransaction(writeOpts, txnOpts));
    INVARIANT(_txn != nullptr);
}

uint64_t RocksOptTxn::getTxnId() const {
    return _txnId;
}

Expected<std::string> RocksOptTxn::getKV(const std::string& key) {
    ensureTxn();
    rocksdb::ReadOptions readOpts;
    std::string value;
    auto s = _txn->Get(readOpts, key, &value);
    if (s.ok()) {
        return value;
    }
    if (s.IsNotFound()) {
        return {ErrorCodes::ERR_NOTFOUND, s.ToString()};
    }
    return {ErrorCodes::ERR_INTERNAL, s.ToString()};
}

Status RocksOptTxn::setKV(const std::string& key,
                          const std::string& val,
                          bool withLog) {
    ensureTxn();
    auto s = _txn->Put(key, val);
    if (!s.ok()) {
        return {ErrorCodes::ERR_INTERNAL, s.ToString()};
    }
    if (!withLog) {
        return {ErrorCodes::ERR_OK, ""};
    }

    if (_binlogs.size() >= std::numeric_limits<uint16_t>::max()) {
        return {ErrorCodes::ERR_BUSY, "txn max ops reached"};
    }
    ReplLogKey logKey(_txnId, _binlogs.size(),
                ReplFlag::REPL_GROUP_MID, sinceEpoch());
    ReplLogValue logVal(ReplOp::REPL_OP_SET, key, val);
    if (_binlogs.size() == 0) {
        uint16_t oriFlag = static_cast<uint16_t>(logKey.getFlag());
        oriFlag |= static_cast<uint16_t>(ReplFlag::REPL_GROUP_START);
        logKey.setFlag(static_cast<ReplFlag>(oriFlag));
    }
    _binlogs.emplace_back(
            std::move(
                ReplLog(std::move(logKey), std::move(logVal))));
    return {ErrorCodes::ERR_OK, ""};
}

Status RocksOptTxn::delKV(const std::string& key, bool withLog) {
    ensureTxn();
    auto s = _txn->Delete(key);
    if (!s.ok()) {
        return {ErrorCodes::ERR_INTERNAL, s.ToString()};
    }
    if (!withLog) {
        return {ErrorCodes::ERR_OK, ""};
    }

    if (_binlogs.size() >= std::numeric_limits<uint16_t>::max()) {
        return {ErrorCodes::ERR_BUSY, "txn max ops reached"};
    }
    ReplLogKey logKey(_txnId, _binlogs.size(),
                ReplFlag::REPL_GROUP_MID, sinceEpoch());
    ReplLogValue logVal(ReplOp::REPL_OP_DEL, key, "");
    if (_binlogs.size() == 0) {
        uint16_t oriFlag = static_cast<uint16_t>(logKey.getFlag());
        oriFlag |= static_cast<uint16_t>(ReplFlag::REPL_GROUP_START);
        logKey.setFlag(static_cast<ReplFlag>(oriFlag));
    }
    _binlogs.emplace_back(
        std::move(
            ReplLog(std::move(logKey), std::move(logVal))));
    return {ErrorCodes::ERR_OK, ""};
}

RocksOptTxn::~RocksOptTxn() {
    if (_done) {
        return;
    }
    _txn.reset();
    _store->markCommitted(_txnId);
}

rocksdb::Options RocksKVStore::options() {
    rocksdb::Options options;
    rocksdb::BlockBasedTableOptions table_options;
    table_options.block_cache = _blockCache;
    table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
    table_options.block_size = 16 * 1024;  // 16KB
    table_options.format_version = 2;
    // let index and filters pining in mem forever
    table_options.cache_index_and_filter_blocks = false;
    options.table_factory.reset(
        rocksdb::NewBlockBasedTableFactory(table_options));
    options.write_buffer_size = 64 * 1024 * 1024;  // 64MB
    // level_0 max size: 8*64MB = 512MB
    options.level0_slowdown_writes_trigger = 8;
    options.max_write_buffer_number = 4;
    options.max_write_buffer_number_to_maintain = 1;
    options.max_background_compactions = 8;
    options.max_background_flushes = 2;
    options.target_file_size_base = 64 * 1024 * 1024;  // 64MB
    options.level_compaction_dynamic_level_bytes = true;
    // level_1 max size: 512MB, in fact, things are more complex
    // since we set level_compaction_dynamic_level_bytes = true
    options.max_bytes_for_level_base = 512 * 1024 * 1024;  // 512 MB
    options.max_open_files = -1;
    // if we have no 'empty reads', we can disable bottom
    // level's bloomfilters
    options.optimize_filters_for_hits = false;
    // TODO(deyukong): we should have our own compaction factory
    // options.compaction_filter_factory.reset(
    //     new PrefixDeletingCompactionFilterFactory(this));
    options.enable_thread_tracking = true;
    options.compression_per_level.resize(7);
    options.compression_per_level[0] = rocksdb::kNoCompression;
    options.compression_per_level[1] = rocksdb::kNoCompression;
    options.compression_per_level[2] = rocksdb::kSnappyCompression;
    options.compression_per_level[3] = rocksdb::kSnappyCompression;
    options.compression_per_level[4] = rocksdb::kSnappyCompression;
    options.compression_per_level[5] = rocksdb::kSnappyCompression;
    options.compression_per_level[6] = rocksdb::kSnappyCompression;
    options.statistics = _stats;
    options.create_if_missing = true;

    options.max_total_wal_size = uint64_t(4294967296);  // 4GB
    return options;
}

bool RocksKVStore::isRunning() const {
    std::lock_guard<std::mutex> lk(_mutex);
    return _isRunning;
}

Status RocksKVStore::stop() {
    std::lock_guard<std::mutex> lk(_mutex);
    if (_aliveTxns.size() != 0) {
        return {ErrorCodes::ERR_INTERNAL,
            "it's upperlayer's duty to guarantee no pinning txns alive"};
    }
    _isRunning = false;
    _db.reset();
    return {ErrorCodes::ERR_OK, ""};
}

Status RocksKVStore::clear() {
    std::lock_guard<std::mutex> lk(_mutex);
    if (_isRunning) {
        return {ErrorCodes::ERR_INTERNAL, "should stop before clear"};
    }
    try {
        const std::string path = dbPath() + "/" + dbId();
        if (!filesystem::exists(path)) {
            return {ErrorCodes::ERR_OK, ""};
        }
        auto n = filesystem::remove_all(dbPath() + "/" + dbId());
        LOG(INFO) << "dbId:" << dbId() << " cleared " << n << " files/dirs";
    } catch(std::exception& ex) {
        LOG(WARNING) << "dbId:" << dbId()
                     << " clear failed:" << ex.what();
        return {ErrorCodes::ERR_INTERNAL, ex.what()};
    }
    return {ErrorCodes::ERR_OK, ""};
}

Expected<uint64_t> RocksKVStore::restart(bool restore) {
    std::lock_guard<std::mutex> lk(_mutex);
    if (_isRunning) {
        return {ErrorCodes::ERR_INTERNAL, "already running"};
    }
    std::string dbname = dbPath() + "/" + dbId();
    if (restore) {
        try {
            const std::string path = dbPath() + "/" + dbId();
            if (filesystem::exists(path)) {
                std::stringstream ss;
                ss << "path:" << path
                    << " should not exist when restore";
                return {ErrorCodes::ERR_INTERNAL, ss.str()};
            }
            if (!filesystem::exists(backupDir())) {
                std::stringstream ss;
                ss << "recover path:" << backupDir()
                    << " not exist when restore";
                return {ErrorCodes::ERR_INTERNAL, ss.str()};
            }
            filesystem::rename(backupDir(), path);
        } catch(std::exception& ex) {
            LOG(WARNING) << "dbId:" << dbId()
                        << "restore exception" << ex.what();
            return {ErrorCodes::ERR_INTERNAL, ex.what()};
        }
    }

    try {
        // this happens due to a bad terminate
        if (filesystem::exists(backupDir())) {
            LOG(WARNING) << backupDir() << " exists, remove it";
            filesystem::remove_all(backupDir());
        }
    } catch (const std::exception& ex) {
        return {ErrorCodes::ERR_INTERNAL, ex.what()};
    }

    rocksdb::OptimisticTransactionDB *tmpDb;
    rocksdb::Options dbOpts = options();
    auto status = rocksdb::OptimisticTransactionDB::Open(
        dbOpts, dbname, &tmpDb);
    if (!status.ok()) {
        return {ErrorCodes::ERR_INTERNAL, status.ToString()};
    }

    // NOTE(deyukong): during starttime, mutex is held and
    // no need to consider visibility
    // TODO(deyukong): use BinlogCursor to rewrite 
    rocksdb::ReadOptions readOpts;
    auto iter = std::unique_ptr<rocksdb::Iterator>(
        tmpDb->GetBaseDB()->NewIterator(readOpts));
    RocksKVCursor cursor(std::move(iter));

    cursor.seekToLast();
    Expected<Record> expRcd = cursor.next();

    uint64_t maxCommitId = Transaction::TXNID_UNINITED;
    if (expRcd.ok()) {
        const RecordKey& rk = expRcd.value().getRecordKey();
        if (rk.getRecordType() == RecordType::RT_BINLOG) {
            auto explk = ReplLogKey::decode(rk);
            if (!explk.ok()) {
                return explk.status();
            } else {
                LOG(INFO) << "store:" << dbId()
                          << " nextSeq change from:" << _nextTxnSeq
                          << " to:" << explk.value().getTxnId() + 1;
                maxCommitId = explk.value().getTxnId();
                _nextTxnSeq = maxCommitId+1;
                _highestVisible = maxCommitId;
            }
        } else {
            _nextTxnSeq = Transaction::MIN_VALID_TXNID;
            LOG(INFO) << "store:" << dbId() << ' ' << rk.getPrimaryKey()
                    << " have no binlog, set nextSeq to " << _nextTxnSeq;
            _highestVisible = Transaction::TXNID_UNINITED;
            INVARIANT(_highestVisible < _nextTxnSeq);
        }
    } else if (expRcd.status().code() == ErrorCodes::ERR_EXHAUST) {
        _nextTxnSeq = Transaction::MIN_VALID_TXNID;
        LOG(INFO) << "store:" << dbId()
                  << " all empty, set nextSeq to " << _nextTxnSeq;
        _highestVisible = Transaction::TXNID_UNINITED;
        INVARIANT(_highestVisible < _nextTxnSeq);
    } else {
        return expRcd.status();
    }

    _db.reset(tmpDb);
    _isRunning = true;
    return maxCommitId;
}

RocksKVStore::RocksKVStore(const std::string& id,
            const std::shared_ptr<ServerParams>& cfg,
            std::shared_ptr<rocksdb::Cache> blockCache)
        :KVStore(id, cfg->dbPath),
         _isRunning(false),
         _hasBackup(false),
         _db(nullptr),
         _stats(rocksdb::CreateDBStatistics()),
         _blockCache(blockCache),
         _nextTxnSeq(0),
         _highestVisible(Transaction::TXNID_UNINITED) {
    Expected<uint64_t> s = restart(false);
    if (!s.ok()) {
        LOG(FATAL) << "opendb:" << cfg->dbPath << "/" << id
                    << ", failed info:" << s.status().toString();
    }
}

Status RocksKVStore::releaseBackup() {
    try {
        if (!filesystem::exists(backupDir())) {
            return {ErrorCodes::ERR_OK, ""};
        }
        filesystem::remove_all(backupDir());
    } catch (const std::exception& ex) {
        LOG(FATAL) << "remove " << backupDir() << " ex:" << ex.what();
    }
    {
        std::lock_guard<std::mutex> lk(_mutex);
        if (_hasBackup) {
            _hasBackup = false;
        }
        return {ErrorCodes::ERR_OK, ""};
    }
}

// this function guarantees that if backup is failed,
// there should be no remaining dirs left to clean.
Expected<BackupInfo> RocksKVStore::backup() {
    {
        std::lock_guard<std::mutex> lk(_mutex);
        if (_hasBackup) {
            return {ErrorCodes::ERR_INTERNAL, "already have backup"};
        }
        _hasBackup = true;
    }
    bool succ = false;
    auto guard = MakeGuard([this, &succ]() {
        if (succ) {
            return;
        }
        try {
            if (!filesystem::exists(backupDir())) {
                return;
            }
            filesystem::remove_all(backupDir());
        } catch (const std::exception& ex) {
            LOG(FATAL) << "remove " << backupDir() << " ex:" << ex.what();
        }
    });

    // NOTE(deyukong): we should get highVisible before making a ckpt
    BackupInfo result;
    uint64_t highVisible = getHighestVisibleTxnId();
    if (highVisible == Transaction::TXNID_UNINITED) {
        return {ErrorCodes::ERR_INTERNAL, "highVisible not inited"};
    }
    result.setBinlogPos(highVisible);

    rocksdb::Checkpoint* checkpoint;    
    auto s = rocksdb::Checkpoint::Create(_db->GetBaseDB(), &checkpoint);
    if (!s.ok()) {
        return {ErrorCodes::ERR_INTERNAL, s.ToString()};
    }
    s = checkpoint->CreateCheckpoint(backupDir());
    if (!s.ok()) {
        return {ErrorCodes::ERR_INTERNAL, s.ToString()};
    }

    std::map<std::string, uint64_t> flist;
    try {
        for (auto& p : filesystem::recursive_directory_iterator(backupDir())) {
            const filesystem::path& path = p.path();
            if (!filesystem::is_regular_file(p)) {
                LOG(INFO) << "backup ignore:" << p.path();
                continue;
            }
            size_t filesize = filesystem::file_size(path);
            // assert path with backupDir prefix
            INVARIANT(path.string().find(backupDir()) == 0);
            std::string relative = path.string().erase(0, backupDir().size());
            flist[relative] = filesize;
        }
    } catch (const std::exception& ex) {
        return {ErrorCodes::ERR_INTERNAL, ex.what()};
    }
    succ = true;
    result.setFileList(flist);
    return result;
}

Expected<std::unique_ptr<Transaction>> RocksKVStore::createTransaction() {
    std::lock_guard<std::mutex> lk(_mutex);
    if (!_isRunning) {
        return {ErrorCodes::ERR_INTERNAL, "db stopped!"};
    }
    uint64_t txnId = _nextTxnSeq++;
    auto ret = std::unique_ptr<Transaction>(new RocksOptTxn(this, txnId));
    addUnCommitedTxnInLock(txnId);
    return std::move(ret);
}

rocksdb::OptimisticTransactionDB* RocksKVStore::getUnderlayerDB() {
    return _db.get();
}

uint64_t RocksKVStore::getHighestVisibleTxnId() const {
    std::lock_guard<std::mutex> lk(_mutex);
    return _highestVisible;
}

void RocksKVStore::addUnCommitedTxnInLock(uint64_t txnId) {
    // TODO(deyukong): need a better mutex mechnism to assert held
    if (_aliveTxns.find(txnId) != _aliveTxns.end()) {
        LOG(FATAL) << "BUG: txnid:" << txnId << " double add uncommitted";
    }
    _aliveTxns.insert({txnId, false});
}

void RocksKVStore::markCommitted(uint64_t txnId) {
    std::lock_guard<std::mutex> lk(_mutex);
    markCommittedInLock(txnId);
}

std::set<uint64_t> RocksKVStore::getUncommittedTxns() const {
    std::lock_guard<std::mutex> lk(_mutex);
    std::set<uint64_t> result;
    for (auto& kv : _aliveTxns) {
        if (!kv.second) {
            result.insert(kv.first);
        }
    }
    return result;
}

void RocksKVStore::markCommittedInLock(uint64_t txnId) {
    if (!_isRunning) {
        LOG(FATAL) << "BUG: _uncommittedTxns not empty after stopped";
    }

    // TODO(deyukong): need a better mutex mechnism to assert held
    auto it = _aliveTxns.find(txnId);
    if (it == _aliveTxns.end()) {
        LOG(FATAL) << "BUG: txnid:" << txnId << " not in uncommitted";
    }
    if (it->second) {
        LOG(FATAL) << "BUG: txnid:" << txnId << " already committed";
    }
    it->second = true;

    // NOTE(deyukong): now, we can remove a visibility-hole
    if (it == _aliveTxns.begin()) {
        while (it != _aliveTxns.end()) {
            if (!it->second) {
                break;
            }
            INVARIANT(it->first > _highestVisible);
            _highestVisible = it->first;
            it = _aliveTxns.erase(it);
        }
    }
}

Expected<RecordValue> RocksKVStore::getKV(const RecordKey& key,
                                          Transaction *txn) {
    Expected<std::string> s = txn->getKV(key.encode());
    if (!s.ok()) {
        return s.status();
    }
    return RecordValue::decode(s.value());
}

Status RocksKVStore::setKV(const RecordKey& key,
                           const RecordValue& value,
                           Transaction *txn,
                           bool withLog) {
    return txn->setKV(key.encode(), value.encode(), withLog);
}

Status RocksKVStore::setKV(const Record& kv,
                           Transaction* txn,
                           bool withLog) {
    // TODO(deyukong): statstics and inmemory-accumulative counter
    Record::KV pair = kv.encode();
    return txn->setKV(pair.first, pair.second, withLog);
}

Status RocksKVStore::setKV(const std::string& key,
                           const std::string& val,
                           Transaction *txn,
                           bool withLog) {
    return txn->setKV(key, val, withLog);
}

Status RocksKVStore::delKV(const RecordKey& key,
                           Transaction *txn,
                           bool withLog) {
    // TODO(deyukong): statstics and inmemory-accumulative counter
    return txn->delKV(key.encode(), withLog);
}

void RocksKVStore::appendJSONStat(rapidjson::Writer<rapidjson::StringBuffer>& w) const {
    w.Key("is_running");
    w.Uint64(_isRunning);
    w.Key("has_backup");
    w.Uint64(_hasBackup);
    w.Key("next_txn_seq");
    w.Uint64(_nextTxnSeq);
    {
        std::lock_guard<std::mutex> lk(_mutex);
        w.Key("alive_txns");
        w.Uint64(_aliveTxns.size());
        w.Key("min_alive_txn");
        w.Uint64(_aliveTxns.size() ? _aliveTxns.begin()->first : 0);
        w.Key("max_alive_txn");
        w.Uint64(_aliveTxns.size() ? _aliveTxns.rbegin()->first : 0);
        w.Key("high_visible");
        w.Uint64(_highestVisible);
    }
}

}  // namespace tendisplus