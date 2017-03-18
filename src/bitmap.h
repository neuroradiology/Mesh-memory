// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil -*-
/**
 * @file   bitmap.h
 * @brief  A bitmap class, with one bit per element.
 * @author Emery Berger <http://www.cs.umass.edu/~emery>
 * @note   Copyright (C) 2005 by Emery Berger, University of Massachusetts Amherst.
 */

#pragma once
#ifndef MESH__BITMAP_H
#define MESH__BITMAP_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common.h"
#include "static/staticlog.h"

namespace mesh {

template <typename Container, typename size_t>
class BitmapIter : public std::iterator<std::forward_iterator_tag, size_t> {
public:
  BitmapIter(const Container &a, const size_t i) : _i(i), _cont(a) {
  }
  BitmapIter &operator++() {
    auto next = _i + 1;
    for (; next < _cont.bitCount() && !_cont.isSet(next); ++next) {
    }
    // if we stopped at the last element and its not set, bump one
    if (next == _cont.bitCount() - 1 && !_cont.isSet(next)) {
      next++;
      d_assert(next == _cont.bitCount());
    }
    _i = next;
    return *this;
  }
  bool operator==(const BitmapIter &rhs) const {
    return _cont.bitmap() == rhs._cont.bitmap() && _i == rhs._i;
  }
  bool operator!=(const BitmapIter &rhs) const {
    return _cont.bitmap() != rhs._cont.bitmap() || _i != rhs._i;
  }
  size_t &operator*() {
    return _i;
  }

private:
  size_t _i;
  const Container &_cont;
};

/**
 * @class Bitmap
 * @brief Manages a dynamically-sized bitmap.
 * @param Heap  the source of memory for the bitmap.
 */

template <typename Heap>
class Bitmap : private Heap {
private:
  DISALLOW_COPY_AND_ASSIGN(Bitmap);

  /// A synonym for the datatype corresponding to a word.
  typedef size_t word_t;
  enum { WORDBITS = sizeof(word_t) * 8 };
  enum { WORDBYTES = sizeof(word_t) };
  /// The log of the number of bits in a word_t, for shifting.
  enum { WORDBITSHIFT = staticlog(WORDBITS) };

  explicit Bitmap() {
  }

public:
  typedef BitmapIter<Bitmap, size_t> iterator;
  typedef BitmapIter<Bitmap, size_t> const const_iterator;

  explicit Bitmap(size_t nBits) : Bitmap() {
    reserve(nBits);
  }

  explicit Bitmap(const std::string &str) : Bitmap() {
    reserve(str.length());

    for (size_t i = 0; i < str.length(); ++i) {
      char c = str[i];
      d_assert_msg(c == '0' || c == '1', "expected 0 or 1 in bitstring, not %c ('%s')", c, str.c_str());
      if (c == '1')
        tryToSet(i);
    }
  }

  Bitmap(Bitmap &&rhs) {
    _elements = rhs._elements;
    _bitarray = rhs._bitarray;
    rhs._bitarray = nullptr;
  }

  ~Bitmap() {
    if (_bitarray)
      Heap::free(_bitarray);
    _bitarray = nullptr;
  }

  std::string to_string(ssize_t nElements = -1) const {
    if (nElements == -1)
      nElements = _elements;
    d_assert(0 <= nElements && static_cast<size_t>(nElements) <= _elements);

    std::string s(nElements, '0');

    for (ssize_t i = 0; i < nElements; i++) {
      if (isSet(i))
        s[i] = '1';
    }

    return s;
  }

  /// @brief Sets aside space for a certain number of elements.
  /// @param nelts the number of elements needed.
  void reserve(uint64_t nelts) {
    if (_bitarray) {
      Heap::free(_bitarray);
    }
    // Round up the number of elements.
    _elements = nelts;
    // Allocate the right number of bytes.
    _bitarray = reinterpret_cast<word_t *>(Heap::malloc(wordCount()));
    d_assert(_bitarray != nullptr);

    clear();
  }

  // number of machine words (4-byte on 32-bit systems, 8-byte on
  // 64-bit) used to store the bitmap
  inline size_t wordCount() const {
    return WORDBITS * ((_elements + WORDBITS - 1) / WORDBITS) / 8;
  }

  inline size_t bitCount() const {
    return _elements;
  }

  const word_t *bitmap() const {
    return _bitarray;
  }

  /// Clears out the bitmap array.
  void clear(void) {
    if (_bitarray != nullptr) {
      memset(_bitarray, 0, wordCount());  // 0 = false
    }
  }

  inline uint64_t setFirstEmpty(uint64_t startingAt = 0) {
    uint32_t startWord, off;
    computeItemPosition(startingAt, startWord, off);

    const size_t words = wordCount();
    for (size_t i = startWord; i < words; i++) {
      const auto bits = _bitarray[i];
      if (bits == ~0UL) {
        off = 0;
        continue;
      }

      d_assert(off <= 63U);
      word_t unsetBits = ~bits;
      d_assert(unsetBits != 0);

      // if the offset is 3, we want to mark the first 3 bits as 'set'
      // or 'unavailable'.
      unsetBits &= ~((1UL << off) - 1);

      // if, after we've masked off everything below our offset there
      // are no free bits, continue
      if (unsetBits == 0) {
        off = 0;
        continue;
      }

      // debug("unset bits: %zx (off: %u, startingAt: %llu", unsetBits, off, startingAt);

      size_t off = __builtin_ffsll(unsetBits) - 1;
      const bool ok = tryToSetAt(i, off);
      d_assert(ok);

      return WORDBITS * i + off;
    }

    debug("mesh: bitmap completely full, aborting.\n");
    abort();
  }

  /// @return true iff the bit was not set (but it is now).
  inline bool tryToSet(uint64_t index) {
    uint32_t item, position;
    computeItemPosition(index, item, position);
    return tryToSetAt(item, position);
  }

  /// Clears the bit at the given index.
  inline bool unset(uint64_t index) {
    uint32_t item, position;
    computeItemPosition(index, item, position);
    word_t oldvalue = _bitarray[item];
    word_t newvalue = oldvalue & ~(getMask(position));
    _bitarray[item] = newvalue;
    return (oldvalue != newvalue);
  }

  inline bool isSet(uint64_t index) const {
    uint32_t item, position;
    computeItemPosition(index, item, position);
    bool result = _bitarray[item] & getMask(position);
    return result;
  }

  inline uint64_t inUseCount() const {
    uint64_t count = 0;
    for (size_t i = 0; i < _elements / WORDBITS; i++) {
      count += __builtin_popcountl(_bitarray[i]);
    }
    return count;
  }

  iterator begin() {
    return iterator(*this, lowestSetBit());
  }
  iterator end() {
    return iterator(*this, bitCount());
  }
  const_iterator begin() const {
    return iterator(*this, lowestSetBit());
  }
  const_iterator end() const {
    return iterator(*this, bitCount());
  }
  const_iterator cbegin() const {
    return iterator(*this, lowestSetBit());
  }
  const_iterator cend() const {
    return iterator(*this, bitCount());
  }

private:
  size_t lowestSetBit() const {
    const size_t words = wordCount();
    for (size_t i = 0; i < words; i++) {
      if (_bitarray[i] == 0ULL)
        continue;

      const size_t off = __builtin_ffsll(_bitarray[i]) - 1;

      return WORDBITS * i + off;
    }

    return bitCount();
  }

  inline bool tryToSetAt(uint32_t item, uint32_t position) {
    const word_t mask = getMask(position);
    word_t oldvalue = _bitarray[item];
    _bitarray[item] |= mask;
    return !(oldvalue & mask);
  }

  /// Given an index, compute its item (word) and position within the word.
  inline void computeItemPosition(uint64_t index, uint32_t &item, uint32_t &position) const {
    d_assert(index < _elements);
    item = index >> WORDBITSHIFT;
    position = index & (WORDBITS - 1);
    d_assert(position == index - (item << WORDBITSHIFT));
    d_assert(item < _elements / WORDBYTES);
  }

  /// To find the bit in a word, do this: word & getMask(bitPosition)
  /// @return a "mask" for the given position.
  inline static word_t getMask(uint64_t pos) {
    return ((word_t)1) << pos;
  }

  /// The bit array itself.
  word_t *_bitarray{nullptr};

  /// The number of elements (bits) in the array.
  size_t _elements{0};
};
}

#endif  // MESH__BITMAP_H
