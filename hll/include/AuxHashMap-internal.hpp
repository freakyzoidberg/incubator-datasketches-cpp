/*
 * Copyright 2018, Yahoo! Inc. Licensed under the terms of the
 * Apache License 2.0. See LICENSE file at the project root for terms.
 */

#ifndef _AUXHASHMAP_INTERNAL_HPP_
#define _AUXHASHMAP_INTERNAL_HPP_

#include "HllUtil.hpp"
#include "AuxHashMap.hpp"
#include "PairIterator.hpp"
#include "IntArrayPairIterator.hpp"

#include <cstring>
#include <sstream>
#include <memory>

namespace datasketches {

template<typename A>
AuxHashMap<A>::AuxHashMap(int lgAuxArrInts, int lgConfigK)
  : lgConfigK(lgConfigK),
    lgAuxArrInts(lgAuxArrInts),
    auxCount(0) {
  typedef typename std::allocator_traits<A>::template rebind_alloc<int> intAlloc;
  const int numItems = 1 << lgAuxArrInts;
  auxIntArr = intAlloc().allocate(numItems);
  std::fill(auxIntArr, auxIntArr + numItems, 0);
}

template<typename A>
AuxHashMap<A>* AuxHashMap<A>::newAuxHashMap(int lgAuxArrInts, int lgConfigK) {
  AuxHashMap* map = ahmAlloc().allocate(1);
  ahmAlloc().construct(map, lgAuxArrInts, lgConfigK);
  return map;
}

template<typename A>
AuxHashMap<A>::AuxHashMap(AuxHashMap& that)
  : lgConfigK(that.lgConfigK),
    lgAuxArrInts(that.lgAuxArrInts),
    auxCount(that.auxCount) {
  typedef typename std::allocator_traits<A>::template rebind_alloc<int> intAlloc;
  const int numItems = 1 << lgAuxArrInts;
  auxIntArr = intAlloc().allocate(numItems);
  std::copy(that.auxIntArr, that.auxIntArr + numItems, auxIntArr);
}

template<typename A>
AuxHashMap<A>* AuxHashMap<A>::newAuxHashMap(AuxHashMap& that) {
  AuxHashMap* map = ahmAlloc().allocate(1);
  ahmAlloc().construct(map, that);
  return map;
}

template<typename A>
AuxHashMap<A>* AuxHashMap<A>::deserialize(const void* bytes, size_t len,
                                          int lgConfigK,
                                          int auxCount, int lgAuxArrInts,
                                          bool srcCompact) {
  int lgArrInts = lgAuxArrInts;
  if (srcCompact) { // early compact versions didn't use LgArr byte field so ignore input
    lgArrInts = HllUtil<A>::computeLgArrInts(HLL, auxCount, lgConfigK);
  } else { // updatable
    lgArrInts = lgAuxArrInts;
  }

  AuxHashMap* auxHashMap = ahmAlloc().allocate(1);
  ahmAlloc().construct(auxHashMap, lgArrInts, lgConfigK);
  int configKmask = (1 << lgConfigK) - 1;

  const int* auxPtr = static_cast<const int*>(bytes);
  if (srcCompact) {
    if (len < auxCount * sizeof(int)) {
      throw std::invalid_argument("Input array too small to hold AuxHashMap image");
    }
    for (int i = 0; i < auxCount; ++i) {
      int pair = auxPtr[i];
      int slotNo = HllUtil<A>::getLow26(pair) & configKmask;
      int value = HllUtil<A>::getValue(pair);
      auxHashMap->mustAdd(slotNo, value);
    }
  } else { // updatable
    int itemsToRead = 1 << lgAuxArrInts;
    if (len < itemsToRead * sizeof(int)) {
      throw std::invalid_argument("Input array too small to hold AuxHashMap image");
    }
    for (int i = 0; i < itemsToRead; ++i) {
      int pair = auxPtr[i];
      if (pair == HllUtil<A>::EMPTY) { continue; }
      int slotNo = HllUtil<A>::getLow26(pair) & configKmask;
      int value = HllUtil<A>::getValue(pair);
      auxHashMap->mustAdd(slotNo, value);
    }
  }

  if (auxHashMap->getAuxCount() != auxCount) {
    throw std::invalid_argument("Deserialized AuxHashMap has wrong number of entries");
  }

  return auxHashMap;                                    
}

template<typename A>
AuxHashMap<A>* AuxHashMap<A>::deserialize(std::istream& is, const int lgConfigK,
                                          const int auxCount, const int lgAuxArrInts,
                                          const bool srcCompact) {
  int lgArrInts = lgAuxArrInts;
  if (srcCompact) { // early compact versions didn't use LgArr byte field so ignore input
    lgArrInts = HllUtil<A>::computeLgArrInts(HLL, auxCount, lgConfigK);
  } else { // updatable
    lgArrInts = lgAuxArrInts;
  }
  
  AuxHashMap* auxHashMap = ahmAlloc().allocate(1);
  ahmAlloc().construct(auxHashMap, lgArrInts, lgConfigK);
  int configKmask = (1 << lgConfigK) - 1;

  if (srcCompact) {
    int pair;
    for (int i = 0; i < auxCount; ++i) {
      is.read((char*)&pair, sizeof(pair));
      int slotNo = HllUtil<A>::getLow26(pair) & configKmask;
      int value = HllUtil<A>::getValue(pair);
      auxHashMap->mustAdd(slotNo, value);
    }
  } else { // updatable
    int itemsToRead = 1 << lgAuxArrInts;
    int pair;
    for (int i = 0; i < itemsToRead; ++i) {
      is.read((char*)&pair, sizeof(pair));
      if (pair == HllUtil<A>::EMPTY) { continue; }
      int slotNo = HllUtil<A>::getLow26(pair) & configKmask;
      int value = HllUtil<A>::getValue(pair);
      auxHashMap->mustAdd(slotNo, value);
    }
  }

  if (auxHashMap->getAuxCount() != auxCount) {
    throw std::invalid_argument("Deserialized AuxHashMap has wrong number of entries");
  }

  return auxHashMap;
}

template<typename A>
AuxHashMap<A>::~AuxHashMap<A>() {
  // should be no way to have an object without a valid array
  typedef typename std::allocator_traits<A>::template rebind_alloc<int> intAlloc;
  intAlloc().deallocate(auxIntArr, 1 << lgAuxArrInts);
}

template<typename A>
std::function<void(AuxHashMap<A>*)> AuxHashMap<A>::make_deleter() {
  return [](AuxHashMap<A>* ptr) {    
    ptr->~AuxHashMap();
    ahmAlloc().deallocate(ptr, 1);
  };
}

template<typename A>
AuxHashMap<A>* AuxHashMap<A>::copy() const {
  AuxHashMap<A>* ptr = ahmAlloc().allocate(1);
  ahmAlloc().construct(ptr, *this);
  return ptr;
}

template<typename A>
int AuxHashMap<A>::getAuxCount() const {
  return auxCount;
}

template<typename A>
int* AuxHashMap<A>::getAuxIntArr(){
  return auxIntArr;
}

template<typename A>
int AuxHashMap<A>::getLgAuxArrInts() const {
  return lgAuxArrInts;
}

template<typename A>
int AuxHashMap<A>::getCompactSizeBytes() const {
  return auxCount << 2;
}

template<typename A>
int AuxHashMap<A>::getUpdatableSizeBytes() const {
  return 4 << lgAuxArrInts;
}

template<typename A>
std::unique_ptr<PairIterator<A>> AuxHashMap<A>::getIterator() const {
  // TODO: use allocator
  typedef typename std::allocator_traits<A>::template rebind_alloc<IntArrayPairIterator<A>> iapiAlloc;
  PairIterator<A>* itr = iapiAlloc().allocate(1);
  iapiAlloc().construct(itr, auxIntArr, 1 << lgAuxArrInts, lgConfigK);
  return std::unique_ptr<PairIterator<A>>(
    itr,
    [](IntArrayPairIterator<A>* ptr) {
      ptr->~IntArrayPairIterator();
      iapiAlloc().deallocate(ptr, 1);
    }
    );
}

template<typename A>
void AuxHashMap<A>::mustAdd(const int slotNo, const int value) {
  const int index = find(auxIntArr, lgAuxArrInts, lgConfigK, slotNo);
  const int entry_pair = HllUtil<A>::pair(slotNo, value);
  if (index >= 0) {
    throw std::invalid_argument("Found a slotNo that should not be there: SlotNo: "
                                + std::to_string(slotNo) + ", Value: " + std::to_string(value));
  }

  // found empty entry
  auxIntArr[~index] = entry_pair;
  ++auxCount;
  checkGrow();
}

template<typename A>
int AuxHashMap<A>::mustFindValueFor(const int slotNo) {
  const int index = find(auxIntArr, lgAuxArrInts, lgConfigK, slotNo);
  if (index >= 0) {
    return HllUtil<A>::getValue(auxIntArr[index]);
  }

  throw std::invalid_argument("slotNo not found: " + std::to_string(slotNo));
}

template<typename A>
void AuxHashMap<A>::mustReplace(const int slotNo, const int value) {
  const int idx = find(auxIntArr, lgAuxArrInts, lgConfigK, slotNo);
  if (idx >= 0) {
    auxIntArr[idx] = HllUtil<A>::pair(slotNo, value);
    return;
  }

  throw std::invalid_argument("Pair not found: SlotNo: " + std::to_string(slotNo)
                              + ", Value: " + std::to_string(value));
}

template<typename A>
void AuxHashMap<A>::checkGrow() {
  if ((HllUtil<A>::RESIZE_DENOM * auxCount) > (HllUtil<A>::RESIZE_NUMER * (1 << lgAuxArrInts))) {
    growAuxSpace();
  }
}

template<typename A>
void AuxHashMap<A>::growAuxSpace() {
  int* oldArray = auxIntArr;
  const int oldArrLen = 1 << lgAuxArrInts;
  const int configKmask = (1 << lgConfigK) - 1;
  const int newArrLen = 1 << ++lgAuxArrInts;
  typedef typename std::allocator_traits<A>::template rebind_alloc<int> intAlloc;
  auxIntArr = intAlloc().allocate(newArrLen);
  std::fill(auxIntArr, auxIntArr + newArrLen, 0);
  for (int i = 0; i < oldArrLen; ++i) {
    const int fetched = oldArray[i];
    if (fetched != HllUtil<A>::EMPTY) {
      // find empty in new array
      const int idx = find(auxIntArr, lgAuxArrInts, lgConfigK, fetched & configKmask);
      auxIntArr[~idx] = fetched;
    }
  }

  intAlloc().deallocate(oldArray, oldArrLen);
}

//Searches the Aux arr hash table for an empty or a matching slotNo depending on the context.
//If entire entry is empty, returns one's complement of index = found empty.
//If entry contains given slotNo, returns its index = found slotNo.
//Continues searching.
//If the probe comes back to original index, throws an exception.
template<typename A>
int AuxHashMap<A>::find(const int* auxArr, const int lgAuxArrInts, const int lgConfigK,
                        const int slotNo) {
  const int auxArrMask = (1 << lgAuxArrInts) - 1;
  const int configKmask = (1 << lgConfigK) - 1;
  int probe = slotNo & auxArrMask;
  const  int loopIndex = probe;
  do {
    const int arrVal = auxArr[probe];
    if (arrVal == HllUtil<A>::EMPTY) { //Compares on entire entry
      return ~probe; //empty
    }
    else if (slotNo == (arrVal & configKmask)) { //Compares only on slotNo
      return probe; //found given slotNo, return probe = index into aux array
    }
    const int stride = (slotNo >> lgAuxArrInts) | 1;
    probe = (probe + stride) & auxArrMask;
  } while (probe != loopIndex);
  throw std::runtime_error("Key not found and no empty slots!");
}

}

#endif // _AUXHASHMAP_INTERNAL_HPP_
