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
 *  In the types/ folder, we have a lot of data structure implementations.  In
 *  some cases, the optimal implementation will have a 'noinline' function that
 *  is rarely called.  To actually ensure that the 'noinline' behavior is
 *  achieved, we put the implementations of those functions here, in a separate
 *  compilation unit.
 */

#include "stm/metadata.hpp"
#include "stm/MiniVector.hpp"
#include "stm/WriteSet.hpp"
#include "stm/UndoLog.hpp"
#include "stm/ValueList.hpp"
#include "policies/policies.hpp"

namespace
{
  /**
   * We use malloc a couple of times here, and this makes it a bit easier
   */
  template <typename T>
  inline T* typed_malloc(size_t N)
  {
      return static_cast<T*>(malloc(sizeof(T) * N));
  }
}

namespace stm
{
  /**
   * This doubles the size of the index. This *does not* do anything as
   * far as actually doing memory allocation. Callers should delete[] the
   * index table, increment the table size, and then reallocate it.
   */
  inline size_t WriteSet::doubleIndexLength()
  {
      assert(shift != 0 &&
             "ERROR: the writeset doesn't support an index this large");
      shift   -= 1;
      ilength  = 1 << (8 * sizeof(uint32_t) - shift);
      return ilength;
  }

  /***  Writeset constructor.  Note that the version must start at 1. */
  WriteSet::WriteSet(const size_t initial_capacity)
      : index(NULL), shift(8 * sizeof(uint32_t)), ilength(0),
        version(1), list(NULL), capacity(initial_capacity), lsize(0)
  {
      // Find a good index length for the initial capacity of the list.
      while (ilength < 3 * initial_capacity) {
          doubleIndexLength();
      }

      index = new index_t[ilength];
      list  = typed_malloc<WriteSetEntry>(capacity);
  }

  /***  Writeset destructor */
  WriteSet::~WriteSet()
  {
      delete[] index;
      free(list);
  }

  /***  Rebuild the writeset */
  void WriteSet::rebuild()
  {
      assert(version != 0 && "ERROR: the version should *never* be 0");

      // extend the index
      delete[] index;
      index = new index_t[doubleIndexLength()];

      for (size_t i = 0; i < lsize; ++i) {
          const WriteSetEntry& l = list[i];
          size_t h = hash(l.addr);

          // search for the next available slot
          while (index[h].version == version) {
              h = (h + 1) % ilength;
          }

          index[h].address = l.addr;
          index[h].version = version;
          index[h].index   = i;
      }
  }

  /***  Resize the writeset */
  void WriteSet::resize()
  {
      WriteSetEntry* temp  = list;
      capacity     *= 2;
      list          = typed_malloc<WriteSetEntry>(capacity);
      memcpy(list, temp, sizeof(WriteSetEntry) * lsize);
      free(temp);
  }

  /***  Another writeset reset function that we don't want inlined */
  void WriteSet::reset_internal()
  {
      memset(index, 0, sizeof(index_t) * ilength);
      version = 1;
  }

  /**
   * Deal with the actual rollback of log entries, which depends on the
   * STM_ABORT_ON_THROW configuration as well as on the type of write logging
   * we're doing.
   */
#if defined(STM_ABORT_ON_THROW)
  void WriteSet::rollback(uintptr_t* exception, size_t len)
  {
      // early exit if there's no exception
      if (!len) {
          return;
      }

      // for each entry, call rollback with the exception range, which will
      // actually writeback if the entry is in the address range.
      uintptr_t* upper = (uintptr_t*)((uint8_t*)exception + len);
      for (iterator i = begin(), e = end(); i != e; ++i) {
          i->rollback(exception, upper);
      }
  }
#else
  // rollback was inlined
#endif

#if !defined(STM_ABORT_ON_THROW)
  void UndoLog::undo()
  {
      for (iterator i = end() - 1, e = begin(); i >= e; --i) {
          i->undo();
      }
  }
#else
  void UndoLog::undo(uintptr_t* exception, size_t len)
  {
      // don't undo the exception object, if it happens to be logged, also
      // don't branch on the inner loop if there isn't an exception
      //
      // for byte-logging we need to deal with the mask to see if the write
      // is going to be in the exception range
      if (!exception) {  // common case only adds one branch
          for (iterator i = end() - 1, e = begin(); i >= e; --i) {
              i->undo();
          }
          return;
      }

      uintptr_t* upper = (uintptr_t*)((uint8_t*)exception + len);
      for (iterator i = end() - 1, e = begin(); i >= e; --i) {
          if (i->filter(exception, upper)) {
              continue;
          }
          i->undo();
      }
  }
#endif

  /**
   * We outline the slowpath filter. If this /ever/ happens it will be such a
   * corner case that it just doesn't matter. Plus this is an abort path
   * anyway... consider it a contention management technique.
   */
  bool ByteLoggingUndoLogEntry::filterSlow(uintptr_t* lower, uintptr_t* upper)
  {
      // we have some sort of intersection... we start by assuming that it's
      // total.
      if (addr >= lower && addr + 1 < upper) {
          return true;
      }

      // We have a complicated intersection. We'll do a really slow loop
      // through each byte---at this point it doesn't make a difference.
      for (unsigned i = 0; i < sizeof(val); ++i) {
          uintptr_t* a = (uintptr_t*)(byte_addr + i);
          if (a >= lower && a < upper) {
              byte_mask[i] = 0x0;
          }
      }

      // did we filter every byte?
      return (mask == 0x0);
  }
} // namespace stm
