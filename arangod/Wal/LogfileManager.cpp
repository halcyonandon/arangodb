////////////////////////////////////////////////////////////////////////////////
/// @brief Write-ahead log logfile manager
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2004-2013 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Jan Steemann
/// @author Copyright 2011-2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "LogfileManager.h"
#include "BasicsC/hashes.h"
#include "BasicsC/json.h"
#include "BasicsC/logging.h"
#include "Basics/Exceptions.h"
#include "Basics/FileUtils.h"
#include "Basics/JsonHelper.h"
#include "Basics/MutexLocker.h"
#include "Basics/ReadLocker.h"
#include "Basics/StringUtils.h"
#include "Basics/WriteLocker.h"
#include "VocBase/server.h"
#include "Wal/AllocatorThread.h"
#include "Wal/CollectorThread.h"
#include "Wal/Slots.h"
#include "Wal/SynchroniserThread.h"

using namespace triagens::wal;

////////////////////////////////////////////////////////////////////////////////
/// @brief the logfile manager singleton
////////////////////////////////////////////////////////////////////////////////

static LogfileManager* Instance = nullptr;

// -----------------------------------------------------------------------------
// --SECTION--                                                  helper functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief minimum logfile size
////////////////////////////////////////////////////////////////////////////////

constexpr uint32_t MinFileSize () {
  return 8 * 1024 * 1024;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get the maximum size of a logfile entry
////////////////////////////////////////////////////////////////////////////////

constexpr uint32_t MaxEntrySize () {
  return 2 << 30; // 2 GB
}

////////////////////////////////////////////////////////////////////////////////
/// @brief minimum number of slots
////////////////////////////////////////////////////////////////////////////////

constexpr uint32_t MinSlots () {
  return 1024 * 8;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief maximum number of slots
////////////////////////////////////////////////////////////////////////////////

constexpr uint32_t MaxSlots () {
  return 1024 * 1024 * 16;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief callback to handle one marker during recovery
////////////////////////////////////////////////////////////////////////////////

static bool ScanMarker (TRI_df_marker_t const* marker, 
                        void* data, 
                        TRI_datafile_t* datafile) { 
  RecoverState* state = reinterpret_cast<RecoverState*>(data);

  TRI_ASSERT(marker != nullptr);

  // note the marker's tick
  TRI_ASSERT(marker->_tick >= state->lastTick);
  state->lastTick = marker->_tick;

  switch (marker->_type) {
    case TRI_WAL_MARKER_ATTRIBUTE: {
      attribute_marker_t const* m = reinterpret_cast<attribute_marker_t const*>(marker);
      TRI_voc_cid_t collectionId = m->_collectionId;
      TRI_voc_tick_t databaseId = m->_databaseId;
      state->collections[collectionId] = databaseId;
      break;
    }
    
    case TRI_WAL_MARKER_SHAPE: {
      shape_marker_t const* m = reinterpret_cast<shape_marker_t const*>(marker);
      TRI_voc_cid_t collectionId = m->_collectionId;
      TRI_voc_tick_t databaseId = m->_databaseId;
      state->collections[collectionId] = databaseId;
      break;
    }
    
    case TRI_WAL_MARKER_DOCUMENT: {
      document_marker_t const* m = reinterpret_cast<document_marker_t const*>(marker);
      TRI_voc_cid_t collectionId = m->_collectionId;
      state->collections[collectionId] = m->_databaseId;
      break;
    }
    
    case TRI_WAL_MARKER_EDGE: {
      edge_marker_t const* m = reinterpret_cast<edge_marker_t const*>(marker);
      TRI_voc_cid_t collectionId = m->_collectionId;
      state->collections[collectionId] = m->_databaseId;
      break;
    }
    
    case TRI_WAL_MARKER_REMOVE: {
      remove_marker_t const* m = reinterpret_cast<remove_marker_t const*>(marker);
      TRI_voc_cid_t collectionId = m->_collectionId;
      state->collections[collectionId] = m->_databaseId;
      break;
    }

    case TRI_WAL_MARKER_BEGIN_TRANSACTION: {
      transaction_begin_marker_t const* m = reinterpret_cast<transaction_begin_marker_t const*>(marker);
      // insert this transaction into the list of failed transactions
      // we do this because if we don't find a commit marker for this transaction, 
      // we'll have it in the failed list at the end of the scan and can ignore it
      state->failedTransactions.insert(m->_transactionId);
      break;
    }
    case TRI_WAL_MARKER_COMMIT_TRANSACTION: {
      transaction_commit_marker_t const* m = reinterpret_cast<transaction_commit_marker_t const*>(marker);
      // remove this transaction from the list of failed transactions
      state->failedTransactions.erase(m->_transactionId);
      break;
    }

    case TRI_WAL_MARKER_ABORT_TRANSACTION: {
      // insert this transaction into the list of failed transactions
      transaction_abort_marker_t const* m = reinterpret_cast<transaction_abort_marker_t const*>(marker);
      state->failedTransactions.insert(m->_transactionId);
      break;
    }

    case TRI_WAL_MARKER_DROP_COLLECTION: {
      collection_drop_marker_t const* m = reinterpret_cast<collection_drop_marker_t const*>(marker);
      // note that the collection was dropped and doesn't need to be recovered
      state->droppedCollections.insert(m->_collectionId);
    }

    case TRI_WAL_MARKER_DROP_DATABASE: {
      database_drop_marker_t const* m = reinterpret_cast<database_drop_marker_t const*>(marker);
      // note that the database was dropped and doesn't need to be recovered
      state->droppedDatabases.insert(m->_databaseId);
    }
  }

  return true;
}

// -----------------------------------------------------------------------------
// --SECTION--                                              class LogfileManager
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                      constructors and destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief create the logfile manager
////////////////////////////////////////////////////////////////////////////////

LogfileManager::LogfileManager (TRI_server_t* server,
                                std::string* databasePath)
  : ApplicationFeature("logfile-manager"),
    _server(server),
    _databasePath(databasePath),
    _directory(),
    _filesize(32 * 1024 * 1024),
    _reserveLogfiles(4),
    _historicLogfiles(10),
    _maxOpenLogfiles(10),
    _numberOfSlots(1048576),
    _syncInterval(100),
    _allowOversizeEntries(true),
    _allowWrites(false), // start in read-only mode
    _hasFoundLastTick(false),
    _inRecovery(true),
    _slots(nullptr),
    _synchroniserThread(nullptr),
    _allocatorThread(nullptr),
    _collectorThread(nullptr),
    _logfilesLock(),
    _lastCollectedId(0),
    _lastSealedId(0),
    _logfiles(),
    _transactions(),
    _failedTransactions(),
    _droppedCollections(),
    _droppedDatabases(),
    _filenameRegex(),
    _shutdown(0) {
  
  LOG_TRACE("creating WAL logfile manager");

  int res = regcomp(&_filenameRegex, "^logfile-([0-9][0-9]*)\\.db$", REG_EXTENDED);

  if (res != 0) {
    THROW_INTERNAL_ERROR("could not compile regex"); 
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief destroy the logfile manager
////////////////////////////////////////////////////////////////////////////////

LogfileManager::~LogfileManager () {
  LOG_TRACE("shutting down WAL logfile manager");

  stop();

  regfree(&_filenameRegex);

  if (_slots != nullptr) {
    delete _slots;
    _slots = nullptr;
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief get the logfile manager instance
////////////////////////////////////////////////////////////////////////////////

LogfileManager* LogfileManager::instance () {
  TRI_ASSERT(Instance != nullptr);
  return Instance;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief initialise the logfile manager instance
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::initialise (std::string* path, 
                                 TRI_server_t* server) {
  TRI_ASSERT(Instance == nullptr);

  Instance = new LogfileManager(server, path);
}

// -----------------------------------------------------------------------------
// --SECTION--                                        ApplicationFeature methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::setupOptions (std::map<std::string, triagens::basics::ProgramOptionsDescription>& options) {
  options["Write-ahead log options:help-wal"]
    ("wal.allow-oversize-entries", &_allowOversizeEntries, "allow entries that are bigger than --wal.logfile-size")
    ("wal.directory", &_directory, "logfile directory")
    ("wal.logfile-size", &_filesize, "size of each logfile")
    ("wal.historic-logfiles", &_historicLogfiles, "maximum number of historic logfiles to keep after collection")
    ("wal.reserve-logfiles", &_reserveLogfiles, "maximum number of reserve logfiles to maintain")
    ("wal.open-logfiles", &_maxOpenLogfiles, "maximum number of parallel open logfiles")
    ("wal.slots", &_numberOfSlots, "number of logfile slots to use")
    ("wal.sync-interval", &_syncInterval, "interval for automatic, non-requested disk syncs (in milliseconds)")
  ;
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

bool LogfileManager::prepare () {
  static bool Prepared = false;

  if (Prepared) {
    return true;
  }

  Prepared = true;

  if (_directory.empty()) {
    // use global configuration variable
    _directory = (*_databasePath);

    if (! basics::FileUtils::isDirectory(_directory)) {
      LOG_FATAL_AND_EXIT("database directory '%s' does not exist.", _directory.c_str());
    }

    // append "/journals"
    if (_directory[_directory.size() - 1] != TRI_DIR_SEPARATOR_CHAR) {
      // append a trailing slash to directory name
      _directory.push_back(TRI_DIR_SEPARATOR_CHAR);
    }
    _directory.append("journals");
  }
    
  if (_directory.empty()) {
    LOG_FATAL_AND_EXIT("no directory specified for WAL logfiles. Please use the --wal.directory option");
  }

  if (_directory[_directory.size() - 1] != TRI_DIR_SEPARATOR_CHAR) {
    // append a trailing slash to directory name
    _directory.push_back(TRI_DIR_SEPARATOR_CHAR);
  }

  if (_filesize < MinFileSize()) {
    // minimum filesize per logfile
    LOG_FATAL_AND_EXIT("invalid logfile size. Please use a value of at least %lu", (unsigned long) MinFileSize());
  }
  
  _filesize = (uint32_t) (((_filesize + PageSize - 1) / PageSize) * PageSize);
 
  if (_numberOfSlots < MinSlots() || _numberOfSlots > MaxSlots()) { 
    // invalid number of slots
    LOG_FATAL_AND_EXIT("invalid slots size. Please use a value between %lu and %lu", (unsigned long) MinSlots(), (unsigned long) MaxSlots());
  }

  if (_syncInterval < 5) {
    LOG_FATAL_AND_EXIT("invalid sync interval.");
  } 

  // sync interval is specified in milliseconds by the user, but internally
  // we use microseconds
  _syncInterval = _syncInterval * 1000;

  _slots = new Slots(this, _numberOfSlots, 0);

  return true;
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

bool LogfileManager::start () {
  static bool started = false;
  
  if (started) {
    // we were already started
    return true;
  }

  int res = inventory();

  if (res != TRI_ERROR_NO_ERROR) {
    LOG_ERROR("could not create WAL logfile inventory: %s", TRI_errno_string(res));
    return false;
  }
 
  std::string const shutdownFile = shutdownFilename();
  bool const shutdownFileExists = basics::FileUtils::exists(shutdownFile); 

  if (shutdownFileExists) {
    res = readShutdownInfo();
  
    if (res != TRI_ERROR_NO_ERROR) {
      LOG_ERROR("could not open shutdown file '%s': %s", 
                shutdownFile.c_str(), 
                TRI_errno_string(res));
      return false;
    }
  }

  res = openLogfiles();
  
  if (res != TRI_ERROR_NO_ERROR) {
    LOG_ERROR("could not open WAL logfiles: %s", TRI_errno_string(res));
    return false;
  }
  
  res = startAllocatorThread();

  if (res != TRI_ERROR_NO_ERROR) {
    LOG_ERROR("could not start WAL allocator thread: %s", TRI_errno_string(res));
    return false;
  }
  
  res = startSynchroniserThread();

  if (res != TRI_ERROR_NO_ERROR) {
    LOG_ERROR("could not start WAL synchroniser thread: %s", TRI_errno_string(res));
    return false;
  }

  started = true;

  LOG_TRACE("WAL logfile manager configuration: historic logfiles: %lu, reserve logfiles: %lu, filesize: %lu, sync interval: %lu",
            (unsigned long) _historicLogfiles,
            (unsigned long) _reserveLogfiles,
            (unsigned long) _filesize,
            (unsigned long) _syncInterval);

  return true;
}
  
////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

bool LogfileManager::open () {
  static bool opened = false;
  
  if (opened) {
    // we were already started
    return true;
  }

  opened = true;
  
  return runRecovery();
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::close () {
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::stop () {
  if (_shutdown > 0) {
    return;
  }

  _shutdown = 1;
  
  LOG_TRACE("shutting down WAL");
 
  // set WAL to read-only mode
  allowWrites(false);

  // stop threads
  LOG_TRACE("stopping collector thread");
  stopCollectorThread();

  this->flush(true, false);
  
  LOG_TRACE("stopping allocator thread");
  stopAllocatorThread();
  
  LOG_TRACE("stopping synchroniser thread");
  stopSynchroniserThread();
  
  // close all open logfiles
  LOG_TRACE("closing logfiles");
  closeLogfiles();

  int res = writeShutdownInfo(true);

  if (res != TRI_ERROR_NO_ERROR) {
    LOG_ERROR("could not write WAL shutdown info: %s", TRI_errno_string(res));
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief registers a transaction
////////////////////////////////////////////////////////////////////////////////
  
bool LogfileManager::registerTransaction (TRI_voc_tid_t id) {
  {
    WRITE_LOCKER(_logfilesLock);

    _transactions.insert(make_pair(id, make_pair(_lastCollectedId, _lastSealedId)));
    TRI_ASSERT(_lastCollectedId <= _lastSealedId);
  }

  return true;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief unregisters a transaction
////////////////////////////////////////////////////////////////////////////////
        
void LogfileManager::unregisterTransaction (TRI_voc_tid_t id,
                                            bool markAsFailed) {
  {
    WRITE_LOCKER(_logfilesLock);
    _transactions.erase(id);

    if (markAsFailed) {
      _failedTransactions.insert(id);
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return the set of failed transactions
////////////////////////////////////////////////////////////////////////////////

std::unordered_set<TRI_voc_tid_t> LogfileManager::getFailedTransactions () {
  std::unordered_set<TRI_voc_tid_t> failedTransactions;

  {
    READ_LOCKER(_logfilesLock);
    failedTransactions = _failedTransactions;
  }

  return failedTransactions;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return the set of dropped collections
/// this is used during recovery and not used afterwards
////////////////////////////////////////////////////////////////////////////////

std::unordered_set<TRI_voc_cid_t> LogfileManager::getDroppedCollections () {
  std::unordered_set<TRI_voc_cid_t> droppedCollections;

  {
    READ_LOCKER(_logfilesLock);
    droppedCollections = _droppedCollections;
  }

  return droppedCollections;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return the set of dropped databases
/// this is used during recovery and not used afterwards
////////////////////////////////////////////////////////////////////////////////

std::unordered_set<TRI_voc_tick_t> LogfileManager::getDroppedDatabases () {
  std::unordered_set<TRI_voc_tick_t> droppedDatabases;

  {
    READ_LOCKER(_logfilesLock);
    droppedDatabases = _droppedDatabases;
  }

  return droppedDatabases;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief unregister a list of failed transactions
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::unregisterFailedTransactions (std::unordered_set<TRI_voc_tid_t> const& failedTransactions) {
  WRITE_LOCKER(_logfilesLock);
 
  std::for_each(failedTransactions.begin(), failedTransactions.end(), [&] (TRI_voc_tid_t id) {
    _failedTransactions.erase(id);
  });
}

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not it is currently allowed to create an additional 
/// logfile
////////////////////////////////////////////////////////////////////////////////

bool LogfileManager::logfileCreationAllowed (uint32_t size) {
  if (size + Logfile::overhead() > filesize()) {
    // oversize entry. this is always allowed because otherwise everything would
    // lock
    return true;
  }

  uint32_t numberOfLogfiles = 0;

  // note: this information could also be cached instead of being recalculated
  // everytime
  READ_LOCKER(_logfilesLock);
     
  for (auto it = _logfiles.begin(); it != _logfiles.end(); ++it) {
    Logfile* logfile = (*it).second;
  
    TRI_ASSERT(logfile != nullptr);

    if (logfile->status() == Logfile::StatusType::OPEN ||
        logfile->status() == Logfile::StatusType::SEAL_REQUESTED) { 
      ++numberOfLogfiles;
    }
  }

  return (numberOfLogfiles <= _maxOpenLogfiles);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not there are reserve logfiles
////////////////////////////////////////////////////////////////////////////////

bool LogfileManager::hasReserveLogfiles () {
  uint32_t numberOfLogfiles = 0;

  // note: this information could also be cached instead of being recalculated
  // everytime
  READ_LOCKER(_logfilesLock);
   
  // reverse-scan the logfiles map  
  for (auto it = _logfiles.rbegin(); it != _logfiles.rend(); ++it) {
    Logfile* logfile = (*it).second;
  
    TRI_ASSERT(logfile != nullptr);

    if (logfile->freeSize() > 0 && ! logfile->isSealed()) {
      if (++numberOfLogfiles >= reserveLogfiles()) {
        return true;
      }
    }
  }

  return false;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief signal that a sync operation is required
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::signalSync () {
  _synchroniserThread->signalSync();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief allocate space in a logfile for later writing
////////////////////////////////////////////////////////////////////////////////

SlotInfo LogfileManager::allocate (uint32_t size) {
  if (! _allowWrites) {
    return SlotInfo(TRI_ERROR_ARANGO_READ_ONLY);
  }

  if (size > MaxEntrySize()) {
    // entry is too big
    return SlotInfo(TRI_ERROR_ARANGO_DOCUMENT_TOO_LARGE);
  }
      
  if (size > _filesize && ! _allowOversizeEntries) {
    // entry is too big for a logfile
    return SlotInfo(TRI_ERROR_ARANGO_DOCUMENT_TOO_LARGE);
  }

  return _slots->nextUnused(size);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief finalise a log entry
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::finalise (SlotInfo& slotInfo,
                               bool waitForSync) {
  _slots->returnUsed(slotInfo, waitForSync);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief write data into the logfile
/// this is a convenience function that combines allocate, memcpy and finalise
////////////////////////////////////////////////////////////////////////////////

SlotInfoCopy LogfileManager::allocateAndWrite (void* src,
                                               uint32_t size,
                                               bool waitForSync) {

  SlotInfo slotInfo = allocate(size);
 
  if (slotInfo.errorCode != TRI_ERROR_NO_ERROR) {
    return SlotInfoCopy(slotInfo.errorCode);
  }

  TRI_ASSERT(slotInfo.slot != nullptr);

  try {
    slotInfo.slot->fill(src, size);
 
    // we must copy the slotinfo because finalise() will set its internal to 0 again
    SlotInfoCopy copy(slotInfo.slot);
  
    finalise(slotInfo, waitForSync);
    return copy;
  }
  catch (...) {
    // if we don't return the slot we'll run into serious problems later
    finalise(slotInfo, false);
    
    return SlotInfoCopy(TRI_ERROR_INTERNAL);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief finalise and seal the currently open logfile
/// this is useful to ensure that any open writes up to this point have made
/// it into a logfile
////////////////////////////////////////////////////////////////////////////////

int LogfileManager::flush (bool waitForSync,
                           bool writeShutdownFile) {
  LOG_TRACE("about to flush active WAL logfile");

  int res = _slots->flush(waitForSync);

  if (res == TRI_ERROR_NO_ERROR && writeShutdownFile) {
    // update the file with the last tick, last sealed etc.  
    return writeShutdownInfo(false);
  }

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief re-inserts a logfile back into the inventory only
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::relinkLogfile (Logfile* logfile) {
  Logfile::IdType const id = logfile->id();

  WRITE_LOCKER(_logfilesLock);
  _logfiles.insert(make_pair(id, logfile));
}

////////////////////////////////////////////////////////////////////////////////
/// @brief remove a logfile from the inventory only
////////////////////////////////////////////////////////////////////////////////

bool LogfileManager::unlinkLogfile (Logfile* logfile) {
  Logfile::IdType const id = logfile->id();

  WRITE_LOCKER(_logfilesLock);
  auto it = _logfiles.find(id);

  if (it == _logfiles.end()) {
    return false;
  }

  _logfiles.erase(it);
  
  return true;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief remove a logfile from the inventory only
////////////////////////////////////////////////////////////////////////////////

Logfile* LogfileManager::unlinkLogfile (Logfile::IdType id) {
  WRITE_LOCKER(_logfilesLock);
  auto it = _logfiles.find(id);

  if (it == _logfiles.end()) {
    return nullptr;
  }

  _logfiles.erase(it);
  
  return (*it).second;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief remove a logfile from the inventory and in the file system
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::removeLogfile (Logfile* logfile,
                                    bool unlink) {
  if (unlink) {
    unlinkLogfile(logfile);
  }

  // old filename
  Logfile::IdType const id = logfile->id();
  std::string const filename = logfileName(id);
  
  LOG_TRACE("removing logfile '%s'", filename.c_str());

  // now close the logfile
  delete logfile;
  
  int res = TRI_ERROR_NO_ERROR;
  // now physically remove the file

  if (! basics::FileUtils::remove(filename, &res)) {
    LOG_ERROR("unable to remove logfile '%s': %s", 
              filename.c_str(), 
              TRI_errno_string(res));
    return;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief sets the status of a logfile to open
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::setLogfileOpen (Logfile* logfile) {
  TRI_ASSERT(logfile != nullptr);

  WRITE_LOCKER(_logfilesLock);
  logfile->setStatus(Logfile::StatusType::OPEN);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief sets the status of a logfile to seal-requested
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::setLogfileSealRequested (Logfile* logfile) {
  TRI_ASSERT(logfile != nullptr);

  WRITE_LOCKER(_logfilesLock);
  logfile->setStatus(Logfile::StatusType::SEAL_REQUESTED);
  signalSync();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief sets the status of a logfile to sealed
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::setLogfileSealed (Logfile* logfile) {
  TRI_ASSERT(logfile != nullptr);

  setLogfileSealed(logfile->id());
}

////////////////////////////////////////////////////////////////////////////////
/// @brief sets the status of a logfile to sealed
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::setLogfileSealed (Logfile::IdType id) {
  WRITE_LOCKER(_logfilesLock);

  auto it = _logfiles.find(id);

  if (it == _logfiles.end()) {
    return;
  }

  (*it).second->setStatus(Logfile::StatusType::SEALED);
  _lastSealedId = id;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return the status of a logfile
////////////////////////////////////////////////////////////////////////////////

Logfile::StatusType LogfileManager::getLogfileStatus (Logfile::IdType id) {
  READ_LOCKER(_logfilesLock);
  auto it = _logfiles.find(id);

  if (it == _logfiles.end()) {
    return Logfile::StatusType::UNKNOWN;
  }
  return (*it).second->status();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return the file descriptor of a logfile
////////////////////////////////////////////////////////////////////////////////

int LogfileManager::getLogfileDescriptor (Logfile::IdType id) {
  READ_LOCKER(_logfilesLock);
  auto it = _logfiles.find(id);

  if (it == _logfiles.end()) {
    // error
    LOG_ERROR("could not find logfile %llu", (unsigned long long) id);
    return -1;
  }

  Logfile* logfile = (*it).second;
  TRI_ASSERT(logfile != nullptr);

  return logfile->fd();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get a logfile for writing. this may return nullptr
////////////////////////////////////////////////////////////////////////////////

Logfile* LogfileManager::getWriteableLogfile (uint32_t size,
                                              Logfile::StatusType& status) {
  static const uint64_t SleepTime = 10 * 1000;
  static const uint64_t MaxIterations = 1000;
  size_t iterations = 0;

  while (++iterations < 1000) {
    {
      WRITE_LOCKER(_logfilesLock);
      auto it = _logfiles.begin();

      while (it != _logfiles.end()) {
        Logfile* logfile = (*it).second;

        TRI_ASSERT(logfile != nullptr);
        
        if (logfile->isWriteable(size)) {
          // found a logfile, update the status variable and return the logfile
          status = logfile->status();
          return logfile;
        }

        if (logfile->status() == Logfile::StatusType::EMPTY && 
            ! logfile->isWriteable(size)) {
          // we found an empty logfile, but the entry won't fit

          // delete the logfile from the sequence of logfiles
          _logfiles.erase(it++);

          // and physically remove the file
          // note: this will also delete the logfile object!
          removeLogfile(logfile, false);

        }
        else {
          ++it;
        }
      }
    }

    // signal & sleep outside the lock
    _allocatorThread->signal(size);
    usleep(SleepTime);
  }
  
  LOG_WARNING("unable to acquire writeable WAL logfile after %llu ms", (unsigned long long) (MaxIterations * SleepTime) / 1000);

  return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get a logfile to collect. this may return nullptr
////////////////////////////////////////////////////////////////////////////////

Logfile* LogfileManager::getCollectableLogfile () {
  // iterate over all active readers and find their minimum used logfile id
  Logfile::IdType minId = UINT64_MAX;

  READ_LOCKER(_logfilesLock);
  
  // iterate over all active transactions and find their minimum used logfile id
  for (auto it = _transactions.begin(); it != _transactions.end(); ++it) {
    Logfile::IdType lastWrittenId = (*it).second.second;

    if (lastWrittenId < minId) {
      minId = lastWrittenId;
    }
  }

  for (auto it = _logfiles.begin(); it != _logfiles.end(); ++it) {
    Logfile* logfile = (*it).second;

    if (logfile != nullptr) {
      if (logfile->id() <= minId && logfile->canBeCollected()) {
        return logfile;
      }
      else if (logfile->id() > minId) {
        // abort early
        break;
      }
    }
  }

  return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get a logfile to remove. this may return nullptr
////////////////////////////////////////////////////////////////////////////////

Logfile* LogfileManager::getRemovableLogfile () {
  Logfile::IdType minId = UINT64_MAX;

  uint32_t numberOfLogfiles = 0;
  Logfile* first = nullptr;

  READ_LOCKER(_logfilesLock);

  // iterate over all active readers and find their minimum used logfile id
  for (auto it = _transactions.begin(); it != _transactions.end(); ++it) {
    Logfile::IdType lastCollectedId = (*it).second.first;

    if (lastCollectedId < minId) {
      minId = lastCollectedId;
    }
  }

  for (auto it = _logfiles.begin(); it != _logfiles.end(); ++it) {
    Logfile* logfile = (*it).second;

    // find the first logfile that can be safely removed
    if (logfile != nullptr && logfile->id() <= minId && logfile->canBeRemoved()) {
      if (first == nullptr) {
        first = logfile;
      }

      if (++numberOfLogfiles > historicLogfiles()) { 
        TRI_ASSERT(first != nullptr);
        return first;
      }
    }
  }

  return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief increase the number of collect operations for a logfile
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::increaseCollectQueueSize (Logfile* logfile) {
  logfile->increaseCollectQueueSize();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief decrease the number of collect operations for a logfile
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::decreaseCollectQueueSize (Logfile* logfile) {
  logfile->decreaseCollectQueueSize();
}
  
////////////////////////////////////////////////////////////////////////////////
/// @brief mark a file as being requested for collection
////////////////////////////////////////////////////////////////////////////////
  
void LogfileManager::setCollectionRequested (Logfile* logfile) {
  TRI_ASSERT(logfile != nullptr);

  {
    WRITE_LOCKER(_logfilesLock);
    logfile->setStatus(Logfile::StatusType::COLLECTION_REQUESTED);
  }
  
  if (! _inRecovery) {
    _collectorThread->signal();
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief mark a file as being done with collection
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::setCollectionDone (Logfile* logfile) {
  TRI_ASSERT(logfile != nullptr);

  {
    WRITE_LOCKER(_logfilesLock);
    logfile->setStatus(Logfile::StatusType::COLLECTED);

    _lastCollectedId = logfile->id();
  }

  if (! _inRecovery) {
    _collectorThread->signal();
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                                   private methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief scan a single logfile
////////////////////////////////////////////////////////////////////////////////

bool LogfileManager::scanLogfile (Logfile const* logfile,
                                  RecoverState& state) {
  TRI_ASSERT(logfile != nullptr);

  LOG_TRACE("scanning logfile %llu (%s)", (unsigned long long) logfile->id(), logfile->statusText().c_str());
  
  bool result = TRI_IterateDatafile(logfile->df(), &ScanMarker, static_cast<void*>(&state));

  return result;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief run the recovery procedure
////////////////////////////////////////////////////////////////////////////////

bool LogfileManager::runRecovery () {
  LOG_TRACE("running WAL recovery");
  
  RecoverState state;
  state.lastTick = 0;

  int logfilesToCollect = 0;

  // we're the only ones that access the logfiles at this point
  // nevertheless, get the read-lock
  {
    READ_LOCKER(_logfilesLock);

    // first iterate the logfiles in order and track which collections are in use
    // this also populates a list of failed transactions
    for (auto it = _logfiles.begin(); it != _logfiles.end(); ++it) {
      Logfile* logfile = (*it).second;

      if (logfile != nullptr) {
        if (logfile->status() == Logfile::StatusType::OPEN ||
            logfile->status() == Logfile::StatusType::SEALED) {
          ++logfilesToCollect;
        }

        if (! scanLogfile(logfile, state)) {
          LOG_TRACE("WAL recovery failed when scanning logfile '%s'", logfile->filename().c_str());
          return false;
        }
      }
    }
  }

  // update the tick with the max tick we found in the WAL
  TRI_UpdateTickServer(state.lastTick);

  // note all failed transactions that we found plus the list
  // of collections and databases that we can ignore
  {
    WRITE_LOCKER(_logfilesLock);
    _failedTransactions = state.failedTransactions;
    _droppedDatabases   = state.droppedDatabases;
    _droppedCollections = state.droppedCollections;
  }

  int res = startCollectorThread();

  if (res != TRI_ERROR_NO_ERROR) {
    LOG_ERROR("could not start WAL collector thread: %s", TRI_errno_string(res));
    return false;
  }

  TRI_ASSERT(_collectorThread != nullptr);
  
  // flush any open logfiles so the collector can copy over everything
  this->flush(true, false);
  
  
  // wait for the collector thread to finish the collection
  while (_collectorThread->hasQueuedOperations()) {
    LOG_INFO("waiting for collector thread to finish");
    usleep(100 * 1000);
  }
  
  {
    // reset the list of failed transactions
    WRITE_LOCKER(_logfilesLock);
    _failedTransactions.clear();
    _droppedDatabases.clear();
    _droppedCollections.clear();
  }

  // tell the collector that the recovery is over now
  _inRecovery = false;
  _collectorThread->recoveryDone();
  _allocatorThread->recoveryDone();
  
  // from now on, we allow writes to the logfile
  allowWrites(true);

  if (logfilesToCollect > 0) {
    LOG_INFO("WAL recovery finished successfully");
  }

  return true;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief closes all logfiles
////////////////////////////////////////////////////////////////////////////////
  
void LogfileManager::closeLogfiles () {
  WRITE_LOCKER(_logfilesLock);

  for (auto it = _logfiles.begin(); it != _logfiles.end(); ++it) {
    Logfile* logfile = (*it).second;

    if (logfile != nullptr) {
      delete logfile;
    }
  }

  _logfiles.clear();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief reads the shutdown information
////////////////////////////////////////////////////////////////////////////////

int LogfileManager::readShutdownInfo () {
  std::string const filename = shutdownFilename();

  TRI_json_t* json = TRI_JsonFile(TRI_UNKNOWN_MEM_ZONE, filename.c_str(), nullptr); 

  if (json == nullptr) {
    return TRI_ERROR_INTERNAL;
  }

  uint64_t lastTick = basics::JsonHelper::stringUInt64(json, "tick");
  TRI_UpdateTickServer(static_cast<TRI_voc_tick_t>(lastTick));

  if (lastTick > 0) {
    _hasFoundLastTick = true;
  }
  
  // read id of last collected logfile (maybe 0)
  uint64_t lastCollectedId = basics::JsonHelper::stringUInt64(json, "lastCollected");
  
  // read if of last sealed logfile (maybe 0)
  uint64_t lastSealedId = basics::JsonHelper::stringUInt64(json, "lastSealed");

  if (lastSealedId < lastCollectedId) {
    // should not happen normally
    lastSealedId = lastCollectedId;
  }
  
  TRI_FreeJson(TRI_UNKNOWN_MEM_ZONE, json);
  
  {
    WRITE_LOCKER(_logfilesLock);
    _lastCollectedId = static_cast<Logfile::IdType>(lastCollectedId);
    _lastSealedId = static_cast<Logfile::IdType>(lastSealedId);
  }
  
  return TRI_ERROR_NO_ERROR; 
}

////////////////////////////////////////////////////////////////////////////////
/// @brief writes the shutdown information
/// this function is called at shutdown and at every logfile flush request
////////////////////////////////////////////////////////////////////////////////

int LogfileManager::writeShutdownInfo (bool writeShutdownTime) {
  std::string const filename = shutdownFilename();

  std::string content;
  content.append("{\"tick\":\"");
  content.append(basics::StringUtils::itoa(TRI_CurrentTickServer()));
  content.append("\",\"lastCollected\":\"");
  content.append(basics::StringUtils::itoa(_lastCollectedId));
  content.append("\",\"lastSealed\":\"");
  content.append(basics::StringUtils::itoa(_lastSealedId));

  if (writeShutdownTime) {
    content.append("\",\"shutdownTime\":\"");
    content.append(getTimeString());
  }

  content.append("\"}\n");

  try {
    basics::FileUtils::spit(filename, content);
  }
  catch (std::exception& ex) {
    LOG_ERROR("unable to write WAL state file '%s'", filename.c_str());
    return TRI_ERROR_CANNOT_WRITE_FILE;
  }
    
  return TRI_ERROR_NO_ERROR; 
}

////////////////////////////////////////////////////////////////////////////////
/// @brief start the synchroniser thread
////////////////////////////////////////////////////////////////////////////////

int LogfileManager::startSynchroniserThread () {
  _synchroniserThread = new SynchroniserThread(this, _syncInterval);

  if (_synchroniserThread == nullptr) {
    return TRI_ERROR_INTERNAL;
  }

  if (! _synchroniserThread->start()) {
    delete _synchroniserThread;
    return TRI_ERROR_INTERNAL;
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief stop the synchroniser thread
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::stopSynchroniserThread () {
  if (_synchroniserThread != nullptr) {
    LOG_TRACE("stopping WAL synchroniser thread");

    _synchroniserThread->stop();
    _synchroniserThread->shutdown();

    delete _synchroniserThread;
    _synchroniserThread = nullptr;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief start the allocator thread
////////////////////////////////////////////////////////////////////////////////

int LogfileManager::startAllocatorThread () {
  _allocatorThread = new AllocatorThread(this);

  if (_allocatorThread == nullptr) {
    return TRI_ERROR_INTERNAL;
  }

  if (! _allocatorThread->start()) {
    delete _allocatorThread;
    return TRI_ERROR_INTERNAL;
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief stop the allocator thread
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::stopAllocatorThread () {
  if (_allocatorThread != nullptr) {
    LOG_TRACE("stopping WAL allocator thread");

    _allocatorThread->stop();
    _allocatorThread->shutdown();

    delete _allocatorThread;
    _allocatorThread = nullptr;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief start the collector thread
////////////////////////////////////////////////////////////////////////////////

int LogfileManager::startCollectorThread () {
  _collectorThread = new CollectorThread(this, _server);

  if (_collectorThread == nullptr) {
    return TRI_ERROR_INTERNAL;
  }

  if (! _collectorThread->start()) {
    delete _collectorThread;
    return TRI_ERROR_INTERNAL;
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief stop the collector thread
////////////////////////////////////////////////////////////////////////////////

void LogfileManager::stopCollectorThread () {
  if (_collectorThread != nullptr) {
    LOG_TRACE("stopping WAL collector thread");

    _collectorThread->stop();
    _collectorThread->shutdown();

    delete _collectorThread;
    _collectorThread = nullptr;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief check which logfiles are present in the log directory
////////////////////////////////////////////////////////////////////////////////

int LogfileManager::inventory () {
  int res = ensureDirectory();

  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }

  LOG_TRACE("scanning WAL directory: '%s'", _directory.c_str());

  std::vector<std::string> files = basics::FileUtils::listFiles(_directory);

  for (auto it = files.begin(); it != files.end(); ++it) {
    regmatch_t matches[2];
    std::string const file = (*it);
    char const* s = file.c_str();

    if (regexec(&_filenameRegex, s, sizeof(matches) / sizeof(matches[1]), matches, 0) == 0) {
      Logfile::IdType const id = basics::StringUtils::uint64(s + matches[1].rm_so, matches[1].rm_eo - matches[1].rm_so);

      if (id == 0) {
        LOG_WARNING("encountered invalid id for logfile '%s'. ids must be > 0", file.c_str());
      }
      else {
        // update global tick
        TRI_UpdateTickServer(static_cast<TRI_voc_tick_t>(id));

        WRITE_LOCKER(_logfilesLock);
        _logfiles.insert(make_pair(id, nullptr));
      }
    }
  }
     
  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief scan the logfiles in the log directory
////////////////////////////////////////////////////////////////////////////////

int LogfileManager::openLogfiles () {
  WRITE_LOCKER(_logfilesLock);

  for (auto it = _logfiles.begin(); it != _logfiles.end(); ) {
    Logfile::IdType const id = (*it).first;
    std::string const filename = logfileName(id);

    TRI_ASSERT((*it).second == nullptr);

    int res = Logfile::judge(filename);

    if (res == TRI_ERROR_ARANGO_DATAFILE_EMPTY) {
      if (basics::FileUtils::remove(filename, 0)) {
        LOG_TRACE("removing empty WAL logfile '%s'", filename.c_str());
      }
      _logfiles.erase(it++);
      continue;
    }

    Logfile* logfile = Logfile::openExisting(filename, id, id <= _lastCollectedId);

    if (logfile == nullptr) {
      _logfiles.erase(it++);
      continue;
    }
  
    if (logfile->status() == Logfile::StatusType::SEALED &&
        id > _lastSealedId) {
      _lastSealedId = id;
    }
      
    (*it).second = logfile;
    ++it;
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief allocates a new reserve logfile
////////////////////////////////////////////////////////////////////////////////

int LogfileManager::createReserveLogfile (uint32_t size) {
  Logfile::IdType const id = nextId();
  std::string const filename = logfileName(id);

  LOG_TRACE("creating empty logfile '%s' with size %lu", 
            filename.c_str(), 
            (unsigned long) size);

  uint32_t realsize;
  if (size > 0 && size > filesize()) {
    // create a logfile with the requested size
    realsize = size + Logfile::overhead();
  }
  else {
    // create a logfile with default size
    realsize = filesize();
  }
    
  Logfile* logfile = Logfile::createNew(filename.c_str(), id, realsize);

  if (logfile == nullptr) {
    int res = TRI_errno();

    LOG_ERROR("unable to create logfile: %s", TRI_errno_string(res));
    return res;
  }
               
  WRITE_LOCKER(_logfilesLock);
  _logfiles.insert(make_pair(id, logfile));

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get an id for the next logfile
////////////////////////////////////////////////////////////////////////////////
        
Logfile::IdType LogfileManager::nextId () {
  return static_cast<Logfile::IdType>(TRI_NewTickServer());
}

////////////////////////////////////////////////////////////////////////////////
/// @brief ensure the wal logfiles directory is actually there
////////////////////////////////////////////////////////////////////////////////

int LogfileManager::ensureDirectory () {
  if (! basics::FileUtils::isDirectory(_directory)) {
    int res;
    
    LOG_INFO("WAL directory '%s' does not exist. creating it...", _directory.c_str());

    if (! basics::FileUtils::createDirectory(_directory, &res)) {
      LOG_ERROR("could not create WAL directory: '%s': %s", _directory.c_str(), TRI_errno_string(res));
      return res;
    }
  }

  if (! basics::FileUtils::isDirectory(_directory)) {
    LOG_ERROR("WAL directory '%s' does not exist", _directory.c_str());
    return TRI_ERROR_FILE_NOT_FOUND;
  }

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return the absolute name of the shutdown file
////////////////////////////////////////////////////////////////////////////////

std::string LogfileManager::shutdownFilename () const {
  return (*_databasePath) + TRI_DIR_SEPARATOR_STR + std::string("SHUTDOWN");
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return an absolute filename for a logfile id
////////////////////////////////////////////////////////////////////////////////

std::string LogfileManager::logfileName (Logfile::IdType id) const {
  return _directory + std::string("logfile-") + basics::StringUtils::itoa(id) + std::string(".db");
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return the current time as a string
////////////////////////////////////////////////////////////////////////////////
  
std::string LogfileManager::getTimeString () {
  char buffer[32];
  size_t len;
  time_t tt = time(0);
  struct tm tb;
  TRI_gmtime(tt, &tb);
  len = strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tb);

  return std::string(buffer, len);
}

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|/// @page\\|// --SECTION--\\|/// @\\}"
// End:
