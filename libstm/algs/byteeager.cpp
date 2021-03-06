/**
 *  Copyright (C) 2011
 *  University of Rochester Department of Computer Science
 *    and
 *  Lehigh University Department of Computer Science and Engineering
 *
 * License: Modified BSD
 *          Please see the file LICENSE.RSTM for licensing information
 */

/**
 *  ByteEager Implementation
 *
 *    This is a good-faith implementation of the TLRW algorithm by Dice and
 *    Shavit, from SPAA 2010.  We use bytelocks, eager acquire, and in-place
 *    update, with timeout for deadlock avoidance.
 */

#include "../profiling.hpp"
#include "algs.hpp"

using stm::UNRECOVERABLE;
using stm::TxThread;
using stm::ByteLockList;
using stm::bytelock_t;
using stm::get_bytelock;
using stm::UndoLogEntry;

#define BYTEEAGER_VERSIONING

/**
 *  Declare the functions that we're going to implement, so that we can avoid
 *  circular dependencies.
 */
namespace {
  struct ByteEager
  {
      static TM_FASTCALL bool begin(TxThread*);
      static TM_FASTCALL uintptr_t read_ro(STM_READ_SIG(,,));
      static TM_FASTCALL uintptr_t read_rw(STM_READ_SIG(,,));
      static TM_FASTCALL void write_ro(STM_WRITE_SIG(,,,));
      static TM_FASTCALL void write_rw(STM_WRITE_SIG(,,,));
      static TM_FASTCALL void read_reserve(STM_READ_RESERVE_SIG(,,));
      static TM_FASTCALL void write_reserve(STM_WRITE_RESERVE_SIG(,,));
      static TM_FASTCALL void release(STM_RELEASE_SIG(,,));
      static TM_FASTCALL void commit_ro(TxThread*);
      static TM_FASTCALL void commit_rw(TxThread*);

      static stm::scope_t* rollback(STM_ROLLBACK_SIG(,,));
      static bool irrevoc(TxThread*);
      static void onSwitchTo();
  };

  /**
   *  These defines are for tuning backoff behavior
   */
#if defined(STM_CPU_SPARC)
#  define READ_TIMEOUT        32
#  define ACQUIRE_TIMEOUT     128
#  define DRAIN_TIMEOUT       1024
#else // STM_CPU_X86
#  define READ_TIMEOUT        32
#  define ACQUIRE_TIMEOUT     128
#  define DRAIN_TIMEOUT       256
#endif

  /**
   *  ByteEager begin:
   */
  bool
  ByteEager::begin(TxThread* tx)
  {
      tx->allocator.onTxBegin();
      return false;
  }

  /**
   *  ByteEager commit (read-only):
   */
  void
  ByteEager::commit_ro(TxThread* tx)
  {
      // read-only... release read locks
      foreach (ByteLockList, i, tx->r_bytelocks) {
          (*i)->reader[tx->id-1] = 0;
#ifdef BYTEEAGER_VERSIONING
          (*i)->reader_version[tx->id-1] = 0;
#endif
      }

      tx->r_bytelocks.reset();
      OnReadOnlyCommit(tx);
  }

  /**
   *  ByteEager commit (writing context):
   */
  void
  ByteEager::commit_rw(TxThread* tx)
  {
      // release write locks, then read locks
      foreach (ByteLockList, i, tx->w_bytelocks) {
          (*i)->owner = 0;
      }
      foreach (ByteLockList, i, tx->r_bytelocks) {
          (*i)->reader[tx->id-1] = 0;
#ifdef BYTEEAGER_VERSIONING
          (*i)->reader_version[tx->id-1] = 0;
#endif
      }

      // clean-up
      tx->r_bytelocks.reset();
      tx->w_bytelocks.reset();
      tx->undo_log.reset();
      OnReadWriteCommit(tx, read_ro, write_ro, commit_ro);
  }

  /**
   *  ByteEager read (read-only transaction)
   */
  uintptr_t
  ByteEager::read_ro(STM_READ_SIG(tx,addr,))
  {
      uint32_t tries = 0;
      bytelock_t* lock = get_bytelock(addr);

      // do I have a read lock?
      if (lock->reader[tx->id-1] == 1) {
          return *addr;
      }

      // log this location if new
#ifdef BYTEEAGER_VERSIONING
      if (lock->reader_version[tx->id-1] == 0) {
#endif
          tx->r_bytelocks.insert(lock);
#ifdef BYTEEAGER_VERSIONING
      }
#endif

      // now try to get a read lock
      while (true) {
          // mark my reader byte
          lock->set_read_byte(tx->id-1);

          // if nobody has the write lock, we're done
          if (__builtin_expect(lock->owner == 0, true)) {
#ifdef BYTEEAGER_VERSIONING
              // If this is the first read, log the version.
              // Else abort if version changed
              if (lock->reader_version[tx->id-1] == 0) {
                  lock->reader_version[tx->id-1] = lock->version;
              } else if (lock->reader_version[tx->id-1] != lock->version) {
                  tx->abort();
              }
#endif
              return *addr;
          }

          // drop read lock, wait (with timeout) for lock release
          lock->reader[tx->id-1] = 0;
          while (lock->owner != 0) {
              if (++tries > READ_TIMEOUT) {
                  tx->abort();
              }
          }
      }
  }

  /**
   *  ByteEager read (writing transaction)
   */
  uintptr_t
  ByteEager::read_rw(STM_READ_SIG(tx,addr,))
  {
      uint32_t tries = 0;
      bytelock_t* lock = get_bytelock(addr);

      // do I have the write lock?
      if (lock->owner == tx->id) {
          return *addr;
      }

      // do I have a read lock?
      if (lock->reader[tx->id-1] == 1) {
          return *addr;
      }

      // log this location if new
#ifdef BYTEEAGER_VERSIONING
      if (lock->reader_version[tx->id-1] == 0) {
#endif
          tx->r_bytelocks.insert(lock);
#ifdef BYTEEAGER_VERSIONING
      }
#endif

      // now try to get a read lock
      while (true) {
          // mark my reader byte
          lock->set_read_byte(tx->id-1);
          // if nobody has the write lock, we're done
          if (__builtin_expect(lock->owner == 0, true)) {
#ifdef BYTEEAGER_VERSIONING
              // If this is the first read, log the version.
              // Else abort if version changed
              if (lock->reader_version[tx->id-1] == 0) {
                  lock->reader_version[tx->id-1] = lock->version;
              } else if (lock->reader_version[tx->id-1] != lock->version) {
                  tx->abort();
              }
#endif
              return *addr;
          }

          // drop read lock, wait (with timeout) for lock release
          lock->reader[tx->id-1] = 0;
          while (lock->owner != 0) {
              if (++tries > READ_TIMEOUT) {
                  tx->abort();
              }
          }
      }
  }

  /**
   *  ByteEager write (read-only context)
   */
  void
  ByteEager::write_ro(STM_WRITE_SIG(tx,addr,val,mask))
  {
      uint32_t tries = 0;
      bytelock_t* lock = get_bytelock(addr);

      // get the write lock, with timeout
      while (!bcas32(&(lock->owner), 0u, tx->id)) {
          if (++tries > ACQUIRE_TIMEOUT) {
              tx->abort();
          }
      }

      // log the lock, drop any read locks I have
      tx->w_bytelocks.insert(lock);
      lock->reader[tx->id-1] = 0;

#ifdef BYTEEAGER_VERSIONING
      // If lock was previously read, check that the version has not changed
      if (lock->reader_version[tx->id-1]
          && (lock->reader_version[tx->id-1] != lock->version)) {
              tx->abort();
      }
#endif

      // wait (with timeout) for readers to drain out
      // (read 4 bytelocks at a time)
      volatile uint32_t* lock_alias = (volatile uint32_t*)&lock->reader[0];
      for (int i = 0; i < 16; ++i) {
          tries = 0;
          while (lock_alias[i] != 0) {
              if (++tries > DRAIN_TIMEOUT) {
                  tx->abort();
              }
          }
      }

#ifdef BYTEEAGER_VERSIONING
      // Increment the version on each successful ownership
      ++lock->version;
#endif

      // add to undo log, do in-place write
      tx->undo_log.insert(UndoLogEntry(STM_UNDO_LOG_ENTRY(addr, *addr, mask)));
      STM_DO_MASKED_WRITE(addr, val, mask);

      OnFirstWrite(tx, read_rw, write_rw, commit_rw);
  }

  /**
   *  ByteEager write (writing context)
   */
  void
  ByteEager::write_rw(STM_WRITE_SIG(tx,addr,val,mask))
  {
      uint32_t tries = 0;
      bytelock_t* lock = get_bytelock(addr);

      // If I have the write lock, add to undo log, do write, return
      if (lock->owner == tx->id) {
          tx->undo_log.insert(UndoLogEntry(STM_UNDO_LOG_ENTRY(addr, *addr, mask)));
          STM_DO_MASKED_WRITE(addr, val, mask);
          return;
      }

      // get the write lock, with timeout
      while (!bcas32(&(lock->owner), 0u, tx->id)) {
          if (++tries > ACQUIRE_TIMEOUT) {
              tx->abort();
          }
      }

      // log the lock, drop any read locks I have
      tx->w_bytelocks.insert(lock);
      lock->reader[tx->id-1] = 0;

#ifdef BYTEEAGER_VERSIONING
      // If lock was previously read, check that the version has not changed
      if (lock->reader_version[tx->id-1]
          && (lock->reader_version[tx->id-1] != lock->version)) {
              tx->abort();
      }
#endif

      // wait (with timeout) for readers to drain out
      // (read 4 bytelocks at a time)
      volatile uint32_t* lock_alias = (volatile uint32_t*)&lock->reader[0];
      for (int i = 0; i < 16; ++i) {
          tries = 0;
          while (lock_alias[i] != 0) {
              if (++tries > DRAIN_TIMEOUT) {
                  tx->abort();
              }
          }
      }

#ifdef BYTEEAGER_VERSIONING
      // Increment the version on each successful ownership
      ++lock->version;
#endif

      // add to undo log, do in-place write
      tx->undo_log.insert(UndoLogEntry(STM_UNDO_LOG_ENTRY(addr, *addr, mask)));
      STM_DO_MASKED_WRITE(addr, val, mask);
  }

  /**
   *  ByteEager read reserve
   */
  void
  ByteEager::read_reserve(STM_READ_RESERVE_SIG(tx,addr,))
  {
      uint32_t tries = 0;
      bytelock_t* lock = get_bytelock(addr);

      // do I have the write lock?
      if (lock->owner == tx->id) {
          return;
      }

      // do I have a read lock?
      if (lock->reader[tx->id-1] == 1) {
          return;
      }

      // log this location if new
#ifdef BYTEEAGER_VERSIONING
      if (lock->reader_version[tx->id-1] == 0) {
#endif
          tx->r_bytelocks.insert(lock);
#ifdef BYTEEAGER_VERSIONING
      }
#endif

      // now try to get a read lock
      while (true) {
          // mark my reader byte
          lock->set_read_byte(tx->id-1);
          // if nobody has the write lock, we're done
          if (__builtin_expect(lock->owner == 0, true)) {
#ifdef BYTEEAGER_VERSIONING
              // If this is the first read, log the version.
              // Else abort if version changed
              if (lock->reader_version[tx->id-1] == 0) {
                  lock->reader_version[tx->id-1] = lock->version;
              } else if (lock->reader_version[tx->id-1] != lock->version) {
                  tx->abort();
              }
#endif
              return;
          }

          // drop read lock, wait (with timeout) for lock release
          lock->reader[tx->id-1] = 0;
          while (lock->owner != 0) {
              if (++tries > READ_TIMEOUT) {
                  tx->abort();
              }
          }
      }
  }

  /**
   *  ByteEager write reserve
   */
  void
  ByteEager::write_reserve(STM_WRITE_RESERVE_SIG(tx,addr,))
  {
      uint32_t tries = 0;
      bytelock_t* lock = get_bytelock(addr);

      // If I have the write lock, add to undo log, do write, return
      if (lock->owner == tx->id) {
          tx->undo_log.insert(UndoLogEntry(STM_UNDO_LOG_ENTRY(addr, *addr, mask)));
          return;
      }

      // get the write lock, with timeout
      while (!bcas32(&(lock->owner), 0u, tx->id)) {
          if (++tries > ACQUIRE_TIMEOUT) {
              tx->abort();
          }
      }

      // log the lock, drop any read locks I have
      tx->w_bytelocks.insert(lock);
      lock->reader[tx->id-1] = 0;

#ifdef BYTEEAGER_VERSIONING
      // If lock was previously read, check that the version has not changed
      if (lock->reader_version[tx->id-1]
          && (lock->reader_version[tx->id-1] != lock->version)) {
              tx->abort();
      }
#endif

      // wait (with timeout) for readers to drain out
      // (read 4 bytelocks at a time)
      volatile uint32_t* lock_alias = (volatile uint32_t*)&lock->reader[0];
      for (int i = 0; i < 16; ++i) {
          tries = 0;
          while (lock_alias[i] != 0) {
              if (++tries > DRAIN_TIMEOUT) {
                  tx->abort();
              }
          }
      }

#ifdef BYTEEAGER_VERSIONING
      // Increment the version on each successful ownership
      ++lock->version;
#endif

      // add to undo log, do in-place write
      tx->undo_log.insert(UndoLogEntry(STM_UNDO_LOG_ENTRY(addr, *addr, mask)));
      if (tx->w_bytelocks.size() == 1) {
      OnFirstWrite(tx, read_rw, write_rw, commit_rw);
}
  }

  /**
   *  ByteEager release
   */
  void
  ByteEager::release(STM_RELEASE_SIG(tx,addr,))
  {
#ifdef BYTEEAGER_VERSIONING
      bytelock_t* lock = get_bytelock(addr);

      if (lock->owner != tx->id) {
          lock->reader[tx->id-1] = 0;
      }
#endif
  }

  /**
   *  ByteEager unwinder:
   */
  stm::scope_t*
  ByteEager::rollback(STM_ROLLBACK_SIG(tx, except, len))
  {
      PreRollback(tx);

      // Undo the writes, while at the same time watching out for the exception
      // object.
      STM_UNDO(tx->undo_log, except, len);

      // release write locks, then read locks
      foreach (ByteLockList, i, tx->w_bytelocks) {
          (*i)->owner = 0;
      }
      foreach (ByteLockList, i, tx->r_bytelocks) {
          (*i)->reader[tx->id-1] = 0;
#ifdef BYTEEAGER_VERSIONING
          (*i)->reader_version[tx->id-1] = 0;
#endif
      }

      // reset lists
      tx->r_bytelocks.reset();
      tx->w_bytelocks.reset();
      tx->undo_log.reset();

      // randomized exponential backoff
      exp_backoff(tx);

      return PostRollback(tx, read_ro, write_ro, commit_ro);
  }

  /**
   *  ByteEager in-flight irrevocability:
   */
  bool ByteEager::irrevoc(TxThread*)
  {
      return false;
  }

  /**
   *  Switch to ByteEager:
   */
  void ByteEager::onSwitchTo() {
  }
}

namespace stm {
  /**
   *  ByteEager initialization
   */
  template<>
  void initTM<ByteEager>()
  {
      // set the name
      stms[ByteEager].name      = "ByteEager";

      // set the pointers
      stms[ByteEager].begin     = ::ByteEager::begin;
      stms[ByteEager].commit    = ::ByteEager::commit_ro;
      stms[ByteEager].read      = ::ByteEager::read_ro;
      stms[ByteEager].write     = ::ByteEager::write_ro;
      stms[ByteEager].read_reserve  = ::ByteEager::read_reserve;
      stms[ByteEager].write_reserve = ::ByteEager::write_reserve;
      stms[ByteEager].release   = ::ByteEager::release;
      stms[ByteEager].rollback  = ::ByteEager::rollback;
      stms[ByteEager].irrevoc   = ::ByteEager::irrevoc;
      stms[ByteEager].switcher  = ::ByteEager::onSwitchTo;
      stms[ByteEager].privatization_safe = true;
  }
}
