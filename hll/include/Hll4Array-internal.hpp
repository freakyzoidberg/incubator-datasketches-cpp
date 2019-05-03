/*
 * Copyright 2018, Yahoo! Inc. Licensed under the terms of the
 * Apache License 2.0. See LICENSE file at the project root for terms.
 */

#ifndef _HLL4ARRAY_INTERNAL_HPP_
#define _HLL4ARRAY_INTERNAL_HPP_

#include "Hll4Array.hpp"

#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace datasketches {

template<typename A>
Hll4Iterator<A>::Hll4Iterator(const Hll4Array<A>& hllArray, const int lengthPairs)
  : HllPairIterator<A>(lengthPairs),
    hllArray(hllArray)
{}

template<typename A>
Hll4Iterator<A>::~Hll4Iterator() { }

template<typename A>
int Hll4Iterator<A>::value() {
  const int nib = hllArray.getSlot(index);
  if (nib == HllUtil<A>::AUX_TOKEN) {
    // auxHashMap cannot be null here
    return hllArray.getAuxHashMap()->mustFindValueFor(index);
  } else {
    return nib + hllArray.getCurMin();
  }
}

template<typename A>
Hll4Array<A>::Hll4Array(const int lgConfigK) :
    HllArray(lgConfigK, TgtHllType::HLL_4) {
  const int numBytes = hll4ArrBytes(lgConfigK);
  typedef typename std::allocator_traits<A>::template rebind_alloc<uint8_t> uint8Alloc;
  hllByteArr = uint8Alloc().allocate(numBytes);
  std::fill(hllByteArr, hllByteArr + numBytes, 0);
  auxHashMap = nullptr;
}

template<typename A>
Hll4Array<A>::Hll4Array(const Hll4Array<A>& that) :
  HllArray(that)
{
  // can determine hllByteArr size in parent class, no need to allocate here
  // but parent class doesn't handle the auxHashMap
  if (that.auxHashMap != nullptr) {
    auxHashMap = that.auxHashMap->copy();
  } else {
    auxHashMap = nullptr;
  }
}

template<typename A>
Hll4Array<A>::~Hll4Array() {
  // hllByteArr deleted in parent
  if (auxHashMap != nullptr) {
    AuxHashMap<A>::make_deleter()(auxHashMap);
  }
}

template<typename A>
std::function<void(HllSketchImpl<A>*)> Hll4Array<A>::get_deleter() const {
  return [](Hll4Array<A>* ptr) {
    typedef typename std::allocator_traits<A>::template rebind_alloc<Hll4Array> hll4Alloc;
    ptr->~Hll4Array();
    hll4Alloc().deallocate(ptr, 1);
  };
}

template<typename A>
Hll4Array<A>* Hll4Array<A>::copy() const {
  typedef typename std::allocator_traits<A>::template rebind_alloc<Hll4Array> hll4Alloc;
  Hll4Array<A>* hll = hll4Alloc().allocate(1);
  hll4Alloc().construct(hll, *this);  
}

template<typename A>
std::unique_ptr<PairIterator<A>> Hll4Array<A>::getIterator() const {
  typedef typename std::allocator_traits<A>::template rebind_alloc<Hll4Iterator> itrAlloc;
  PairIterator<A>* itr = itrAlloc().allocate(1);
  itrAlloc().construct(itr, *this, 1 << lgConfigK);
  return std::unique_ptr<PairIterator<A>>(
    itr,
    [](Hll4Iterator* ptr) { ptr->~Hll4Iterator(); itrAlloc().deallocate(ptr, 1); }
  );
}

template<typename A>
std::unique_ptr<PairIterator<A>> Hll4Array<A>::getAuxIterator() const {
  if (auxHashMap != nullptr) {
    return auxHashMap->getIterator();
  }
  return nullptr;
}

template<typename A>
int Hll4Array<A>::getUpdatableSerializationBytes() const {
  AuxHashMap* auxHashMap = getAuxHashMap();
  int auxBytes;
  if (auxHashMap == nullptr) {
    auxBytes = 4 << HllUtil<A>::LG_AUX_ARR_INTS[lgConfigK];
  } else {
    auxBytes = 4 << auxHashMap->getLgAuxArrInts();
  }
  return HllUtil<A>::HLL_BYTE_ARR_START + getHllByteArrBytes() + auxBytes;
}

template<typename A>
int Hll4Array<A>::getHllByteArrBytes() const {
  return hll4ArrBytes(lgConfigK);
}

template<typename A>
AuxHashMap<A>* Hll4Array<A>::getAuxHashMap() const {
  return auxHashMap;
}

template<typename A>
void Hll4Array<A>::putAuxHashMap(AuxHashMap<A>* auxHashMap) {
  this->auxHashMap = auxHashMap;
}

template<typename A>
int Hll4Array<A>::getSlot(const int slotNo) const {
  int theByte = hllByteArr[slotNo >> 1];
  if ((slotNo & 1) > 0) { // odd?
    theByte >>= 4;
  }
  return theByte & HllUtil<A>::loNibbleMask;
}

template<typename A>
HllSketchImpl<A>* Hll4Array<A>::couponUpdate(const int coupon) {
  const int newValue = HllUtil<A>::getValue(coupon);
  if (newValue <= 0) {
    throw std::logic_error("newValue must be a posittive integer. Found: " + std::to_string(newValue));
  }

  if (newValue <= curMin) {
    return this; // quick rejection, but only works for large N
  }

  const int configKmask = (1 << lgConfigK) - 1;
  const int slotNo = HllUtil<A>::getLow26(coupon) & configKmask;
  internalHll4Update(slotNo, newValue);
  return this;
}

template<typename A>
void Hll4Array<A>::putSlot(const int slotNo, const int newValue) {
  const int byteno = slotNo >> 1;
  const int oldValue = hllByteArr[byteno];
  if ((slotNo & 1) == 0) { // set low nibble
    hllByteArr[byteno]
      = (uint8_t) ((oldValue & HllUtil<A>::hiNibbleMask) | (newValue & HllUtil<A>::loNibbleMask));
  } else { // set high nibble
    hllByteArr[byteno]
      = (uint8_t) ((oldValue & HllUtil<A>::loNibbleMask) | ((newValue << 4) & HllUtil<A>::hiNibbleMask));
  }
}

//In C: two-registers.c Line 836 in "hhb_abstract_set_slot_if_new_value_bigger" non-sparse
template<typename A>
void Hll4Array<A>::internalHll4Update(const int slotNo, const int newVal) {
  if ((slotNo < 0) || (slotNo >= (1 << lgConfigK))) {
    throw std::logic_error("slotNo must be between 0 and 1<<lgConfigK. Found: " + std::to_string(slotNo));
  }
  if (newVal <= 0) {
    throw std::logic_error("newVal must be a posittive integer. Found: " + std::to_string(newVal));
  }

  const int rawStoredOldValue = getSlot(slotNo); // could be a 0
  // this is provably a LB:
  const int lbOnOldValue = rawStoredOldValue + curMin; // lower bound, could be 0

  if (newVal > lbOnOldValue) { // 842
    // Note: if an AUX_TOKEN exists, then auxHashMap must alraedy exist
    // 846: rawStoredOldValue == AUX_TOKEN
    const int actualOldValue = (rawStoredOldValue < HllUtil<A>::AUX_TOKEN)
       ? (lbOnOldValue) : (auxHashMap->mustFindValueFor(slotNo));

    if (newVal > actualOldValue) { // 848: actualOldValue could still be 0; newValue > 0
      // we know that hte array will change, but we haven't actually updated yet
      hipAndKxQIncrementalUpdate(*this, actualOldValue, newVal);

      if (newVal < curMin) {
        throw std::logic_error("newVal cannot be less than curMin at this point");
      }

      // newVal >= curMin

      const int shiftedNewValue = newVal - curMin; // 874
      // redundant since we know newVal >= curMin,
      // and lgConfigK bounds do not allow overflowing an int
      //assert(shiftedNewValue >= 0);

      if (rawStoredOldValue == HllUtil<A>::AUX_TOKEN) { // 879
        // Given that we have an AUX_TOKEN, tehre are 4 cases for how to
        // actually modify the data structure

        if (shiftedNewValue >= HllUtil<A>::AUX_TOKEN) { // case 1: 881
          // the byte array already contains aux token
          // This is the case where old and new values are both exceptions.
          // The 4-bit array already is AUX_TOKEN, only need to update auxHashMap
          auxHashMap->mustReplace(slotNo, newVal);
        }
        else { // case 2: 885
          // This is the hypothetical case where the old value is an exception and the new one is not,
          // which is impossible given that curMin has not changed here and newVal > oldValue
          throw std::runtime_error("Impossible case");
        }
      }
      else { // rawStoredOldValue != AUX_TOKEN
        if (shiftedNewValue >= HllUtil<A>::AUX_TOKEN) { // case 3: 892
          // This is the case where the old value is not an exception and the new value is.
          // The AUX_TOKEN must be stored in the 4-bit array and the new value
          // added to the exception table
          putSlot(slotNo, HllUtil<A>::AUX_TOKEN);
          if (auxHashMap == nullptr) {
            auxHashMap = AuxHashMap<A>::newAuxHashMap(HllUtil<A>::LG_AUX_ARR_INTS[lgConfigK], lgConfigK);
          }
          auxHashMap->mustAdd(slotNo, newVal);
        }
        else { // case 4: 897
          // This is the case where neither the old value nor the new value is an exception.
          // We just overwrite the 4-bit array with the shifted new value.
          putSlot(slotNo, shiftedNewValue);
        }
      }

      // we just increased a pair value, so it might be time to change curMin
      if (actualOldValue == curMin) { // 908
        if (numAtCurMin < 1) {
          throw std::logic_error("Invalid state with < 1 entry at curMin");
        }
        decNumAtCurMin();
        while (numAtCurMin == 0) {
          shiftToBiggerCurMin(); // increases curMin by 1, builds a new aux table
          // shifts values in 4-bit table and recounts curMin
        }
      }
    } // end newVal <= actualOldValue
  } // end newValue <= lbOnOldValue -> return, no need to update array
}

// This scheme only works with two double registers (2 kxq values).
//   HipAccum, kxq0 and kxq1 remain untouched.
//   This changes curMin, numAtCurMin, hllByteArr and auxMap.
// Entering this routine assumes that all slots have valid values > 0 and <= 15.
// An AuxHashMap must exist if any values in the current hllByteArray are already 15.
// In C: again-two-registers.c Lines 710 "hhb_shift_to_bigger_curmin"
template<typename A>
void Hll4Array<A>::shiftToBiggerCurMin() {
  const int newCurMin = curMin + 1;
  const int configK = 1 << lgConfigK;
  const int configKmask = configK - 1;

  int numAtNewCurMin = 0;
  int numAuxTokens = 0;

  // Walk through the slots of 4-bit array decrementing stored values by one unless it
  // equals AUX_TOKEN, where it is left alone but counted to be checked later.
  // If oldStoredValue is 0 it is an error.
  // If the decremented value is 0, we increment numAtNewCurMin.
  // Because getNibble is masked to 4 bits oldStoredValue can never be > 15 or negative
  for (int i = 0; i < configK; i++) { //724
    int oldStoredValue = getSlot(i);
    if (oldStoredValue == 0) {
      throw std::runtime_error("Array slots cannot be 0 at this point.");
    }
    if (oldStoredValue < HllUtil<A>::AUX_TOKEN) {
      putSlot(i, --oldStoredValue);
      if (oldStoredValue == 0) { numAtNewCurMin++; }
    } else { //oldStoredValue == AUX_TOKEN
      numAuxTokens++;
      if (auxHashMap == nullptr) {
        throw std::logic_error("auxHashMap cannot be null at this point");
      }
    }
  }

  // If old AuxHashMap exists, walk through it updating some slots and build a new AuxHashMap
  // if needed.
  AuxHashMap<A>* newAuxMap = nullptr;
  if (auxHashMap != nullptr) {
    int slotNum;
    int oldActualVal;
    int newShiftedVal;

    std::unique_ptr<PairIterator<A>> itr = auxHashMap->getIterator();
    while (itr->nextValid()) {
      slotNum = itr->getKey() & configKmask;
      oldActualVal = itr->getValue();
      newShiftedVal = oldActualVal - newCurMin;
      if (newShiftedVal < 0) {
        throw std::logic_error("oldActualVal < newCurMin when incrementing curMin");
      }

      if (getSlot(slotNum) != HllUtil<A>::AUX_TOKEN) {
        throw std::logic_error("getSlot(slotNum) != AUX_TOKEN for item in auxiliary hash map");
      }
      // Array slot != AUX_TOKEN at getSlot(slotNum);
      if (newShiftedVal < HllUtil<A>::AUX_TOKEN) { // 756
        if (newShiftedVal != 14) {
          throw std::logic_error("newShiftedVal != 14 for item in old auxHashMap despite curMin increment");
        }
        // The former exception value isn't one anymore, so it stays out of new AuxHashMap.
        // Correct the AUX_TOKEN value in the HLL array to the newShiftedVal (14).
        putSlot(slotNum, newShiftedVal);
        numAuxTokens--;
      }
      else { //newShiftedVal >= AUX_TOKEN
        // the former exception remains an exception, so must be added to the newAuxMap
        if (newAuxMap == nullptr) {
          newAuxMap = AuxHashMap<A>::newAuxHashMap(HllUtil<A>::LG_AUX_ARR_INTS[lgConfigK], lgConfigK);
        }
        newAuxMap->mustAdd(slotNum, oldActualVal);
      }
    } //end scan of oldAuxMap
  } //end if (auxHashMap != null)
  else { // oldAuxMap == null
    if (numAuxTokens != 0) {
      throw std::logic_error("No auxiliary hash map, but numAuxTokens != 0");
    }
  }

  if (newAuxMap != nullptr) {
    if (newAuxMap->getAuxCount() != numAuxTokens) {
      throw std::runtime_error("Inconsistent counts: auxCount: " + std::to_string(newAuxMap->getAuxCount())
                               + ", HLL tokesn: " + std::to_string(numAuxTokens));
    }
  }

  if (auxHashMap != nullptr) {
    AuxHashMap<A>::make_deleter()(auxHashMap);
  }
  auxHashMap = newAuxMap;

  curMin = newCurMin;
  numAtCurMin = numAtNewCurMin;
}

}

#endif // _HLL4ARRAY_INTERNAL_HPP_
