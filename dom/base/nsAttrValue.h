/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * A struct that represents the value (type and actual data) of an
 * attribute.
 */

#ifndef nsAttrValue_h___
#define nsAttrValue_h___

#include <type_traits>

#include "nscore.h"
#include "nsString.h"
#include "nsStringBuffer.h"
#include "nsColor.h"
#include "nsCaseTreatment.h"
#include "nsMargin.h"
#include "nsStringFwd.h"
#include "nsTArrayForwardDeclare.h"
#include "nsAtom.h"
#include "mozilla/AtomArray.h"
#include "mozilla/EnumTypeTraits.h"
#include "mozilla/MemoryReporting.h"

class nsIPrincipal;
class nsIURI;
class nsStyledElement;
struct MiscContainer;

namespace mozilla {
class DeclarationBlock;
class ShadowParts;
class SVGAnimatedIntegerPair;
class SVGAnimatedLength;
class SVGAnimatedNumberPair;
class SVGAnimatedOrient;
class SVGAnimatedPreserveAspectRatio;
class SVGAnimatedViewBox;
class SVGLengthList;
class SVGNumberList;
class SVGPathData;
class SVGPointList;
class SVGStringList;
class SVGTransformList;

struct AttrAtomArray {
  AtomArray mArray;
  bool mMayContainDuplicates = false;
  void RemoveDuplicates() {
    if (mMayContainDuplicates) {
      DoRemoveDuplicates();
    }
  }
  AttrAtomArray Clone() const {
    return {mArray.Clone(), mMayContainDuplicates};
  }
  void Clear() {
    mArray.Clear();
    mMayContainDuplicates = false;
  }
  bool operator==(const AttrAtomArray& aOther) const {
    return mArray == aOther.mArray;
  }

 private:
  void DoRemoveDuplicates();
};

namespace dom {
class DOMString;
}
}  // namespace mozilla

#define NS_ATTRVALUE_MAX_STRINGLENGTH_ATOM 12

const uintptr_t NS_ATTRVALUE_BASETYPE_MASK = 3;
#define NS_ATTRVALUE_POINTERVALUE_MASK (~NS_ATTRVALUE_BASETYPE_MASK)

#define NS_ATTRVALUE_INTEGERTYPE_BITS 4
#define NS_ATTRVALUE_INTEGERTYPE_MASK \
  (uintptr_t((1 << NS_ATTRVALUE_INTEGERTYPE_BITS) - 1))
#define NS_ATTRVALUE_INTEGERTYPE_MULTIPLIER (1 << NS_ATTRVALUE_INTEGERTYPE_BITS)
#define NS_ATTRVALUE_INTEGERTYPE_MAXVALUE \
  ((1 << (31 - NS_ATTRVALUE_INTEGERTYPE_BITS)) - 1)
#define NS_ATTRVALUE_INTEGERTYPE_MINVALUE \
  (-NS_ATTRVALUE_INTEGERTYPE_MAXVALUE - 1)

#define NS_ATTRVALUE_ENUMTABLEINDEX_BITS \
  (32 - 16 - NS_ATTRVALUE_INTEGERTYPE_BITS)
#define NS_ATTRVALUE_ENUMTABLE_VALUE_NEEDS_TO_UPPER \
  (1 << (NS_ATTRVALUE_ENUMTABLEINDEX_BITS - 1))
#define NS_ATTRVALUE_ENUMTABLEINDEX_MAXVALUE \
  (NS_ATTRVALUE_ENUMTABLE_VALUE_NEEDS_TO_UPPER - 1)
#define NS_ATTRVALUE_ENUMTABLEINDEX_MASK                      \
  (uintptr_t((((1 << NS_ATTRVALUE_ENUMTABLEINDEX_BITS) - 1) & \
              ~NS_ATTRVALUE_ENUMTABLE_VALUE_NEEDS_TO_UPPER)))

/**
 * A class used to construct a nsString from a nsStringBuffer (we might
 * want to move this to nsString at some point).
 *
 * WARNING: Note that nsCheapString doesn't take an explicit length -- it
 * assumes the string is maximally large, given the nsStringBuffer's storage
 * size.  This means the given string buffer *must* be sized exactly correctly
 * for the string it contains (including one byte for a null terminator).  If
 * it has any unused storage space, then that will result in bogus characters
 * at the end of our nsCheapString.
 */
class nsCheapString : public nsString {
 public:
  explicit nsCheapString(nsStringBuffer* aBuf) {
    if (aBuf) aBuf->ToString(aBuf->StorageSize() / sizeof(char16_t) - 1, *this);
  }
};

class nsAttrValue {
  friend struct MiscContainer;

 public:
  // This has to be the same as in ValueBaseType
  enum ValueType {
    eString = 0x00,   //   00
                      //   01  this value indicates a 'misc' struct
    eAtom = 0x02,     //   10
    eInteger = 0x03,  // 0011
    eColor = 0x07,    // 0111
    eEnum = 0x0B,     // 1011  This should eventually die
    ePercent = 0x0F,  // 1111
    // Values below here won't matter, they'll be always stored in the 'misc'
    // struct.
    eCSSDeclaration = 0x10,
    eURL,
    eImage,
    eAtomArray,
    eDoubleValue,
    // eShadowParts is refcounted in the misc container, as we do copy attribute
    // values quite a bit (for example to process style invalidation), and the
    // underlying value could get expensive to copy.
    eShadowParts,
    eSVGIntegerPair,
    eSVGTypesBegin = eSVGIntegerPair,
    eSVGOrient,
    eSVGLength,
    eSVGLengthList,
    eSVGNumberList,
    eSVGNumberPair,
    eSVGPathData,
    eSVGPointList,
    eSVGPreserveAspectRatio,
    eSVGStringList,
    eSVGTransformList,
    eSVGViewBox,
    eSVGTypesEnd = eSVGViewBox,
  };

  nsAttrValue();
  nsAttrValue(const nsAttrValue& aOther);
  explicit nsAttrValue(const nsAString& aValue);
  explicit nsAttrValue(nsAtom* aValue);
  nsAttrValue(already_AddRefed<mozilla::DeclarationBlock> aValue,
              const nsAString* aSerialized);
  ~nsAttrValue();

  inline const nsAttrValue& operator=(const nsAttrValue& aOther);

  static void Init();
  static void Shutdown();

  inline ValueType Type() const;
  // Returns true when this value is self-contained and does not depend on
  // the state of its associated element.
  // Returns false when this value depends on the state of its associated
  // element and may be invalid if that state has been changed by changes to
  // that element state outside of attribute setting.
  inline bool StoresOwnData() const;

  void Reset();

  void SetTo(const nsAttrValue& aOther);
  void SetTo(const nsAString& aValue);
  void SetTo(nsAtom* aValue);
  void SetTo(int16_t aInt);
  void SetTo(int32_t aInt, const nsAString* aSerialized);
  void SetTo(double aValue, const nsAString* aSerialized);
  void SetTo(already_AddRefed<mozilla::DeclarationBlock> aValue,
             const nsAString* aSerialized);
  void SetTo(nsIURI* aValue, const nsAString* aSerialized);
  void SetTo(const mozilla::SVGAnimatedIntegerPair& aValue,
             const nsAString* aSerialized);
  void SetTo(const mozilla::SVGAnimatedLength& aValue,
             const nsAString* aSerialized);
  void SetTo(const mozilla::SVGAnimatedNumberPair& aValue,
             const nsAString* aSerialized);
  void SetTo(const mozilla::SVGAnimatedOrient& aValue,
             const nsAString* aSerialized);
  void SetTo(const mozilla::SVGAnimatedPreserveAspectRatio& aValue,
             const nsAString* aSerialized);
  void SetTo(const mozilla::SVGAnimatedViewBox& aValue,
             const nsAString* aSerialized);
  void SetTo(const mozilla::SVGLengthList& aValue,
             const nsAString* aSerialized);
  void SetTo(const mozilla::SVGNumberList& aValue,
             const nsAString* aSerialized);
  void SetTo(const mozilla::SVGPathData& aValue, const nsAString* aSerialized);
  void SetTo(const mozilla::SVGPointList& aValue, const nsAString* aSerialized);
  void SetTo(const mozilla::SVGStringList& aValue,
             const nsAString* aSerialized);
  void SetTo(const mozilla::SVGTransformList& aValue,
             const nsAString* aSerialized);

  /**
   * Sets this object with the string or atom representation of aValue.
   *
   * After calling this method, this object will have type eString unless the
   * type of aValue is eAtom, in which case this object will also have type
   * eAtom.
   */
  void SetToSerialized(const nsAttrValue& aValue);

  void SwapValueWith(nsAttrValue& aOther);

  void ToString(nsAString& aResult) const;
  inline void ToString(mozilla::dom::DOMString& aResult) const;

  /**
   * Returns the value of this object as an atom. If necessary, the value will
   * first be serialised using ToString before converting to an atom.
   */
  already_AddRefed<nsAtom> GetAsAtom() const;

  // Methods to get value. These methods do not convert so only use them
  // to retrieve the datatype that this nsAttrValue has.
  inline bool IsEmptyString() const;
  const nsCheapString GetStringValue() const;
  inline nsAtom* GetAtomValue() const;
  inline int32_t GetIntegerValue() const;
  bool GetColorValue(nscolor& aColor) const;
  inline int16_t GetEnumValue() const;
  inline double GetPercentValue() const;
  inline mozilla::AttrAtomArray* GetAtomArrayValue() const;
  inline mozilla::DeclarationBlock* GetCSSDeclarationValue() const;
  inline nsIURI* GetURLValue() const;
  inline double GetDoubleValue() const;
  inline const mozilla::ShadowParts& GetShadowPartsValue() const;

  /**
   * Returns the string corresponding to the stored enum value.
   *
   * @param aResult   the string representing the enum tag
   * @param aRealTag  wheter we want to have the real tag or the saved one
   */
  void GetEnumString(nsAString& aResult, bool aRealTag) const;

  // Methods to get access to atoms we may have
  // Returns the number of atoms we have; 0 if we have none.  It's OK
  // to call this without checking the type first; it handles that.
  uint32_t GetAtomCount() const;
  // Returns the atom at aIndex (0-based).  Do not call this with
  // aIndex >= GetAtomCount().
  nsAtom* AtomAt(int32_t aIndex) const;

  uint32_t HashValue() const;
  bool Equals(const nsAttrValue& aOther) const;
  // aCaseSensitive == eIgnoreCase means ASCII case-insenstive matching
  bool Equals(const nsAString& aValue, nsCaseTreatment aCaseSensitive) const;
  bool Equals(const nsAtom* aValue, nsCaseTreatment aCaseSensitive) const;
  bool HasPrefix(const nsAString& aValue, nsCaseTreatment aCaseSensitive) const;
  bool HasSuffix(const nsAString& aValue, nsCaseTreatment aCaseSensitive) const;
  bool HasSubstring(const nsAString& aValue,
                    nsCaseTreatment aCaseSensitive) const;

  /**
   * Compares this object with aOther according to their string representation.
   *
   * For example, when called on an object with type eInteger and value 4, and
   * given aOther of type eString and value "4", EqualsAsStrings will return
   * true (while Equals will return false).
   */
  bool EqualsAsStrings(const nsAttrValue& aOther) const;

  /**
   * Returns true if this AttrValue is equal to the given atom, or is an
   * array which contains the given atom.
   */
  bool Contains(nsAtom* aValue, nsCaseTreatment aCaseSensitive) const;
  /**
   * Returns true if this AttrValue is an atom equal to the given
   * string, or is an array of atoms which contains the given string.
   * This always does a case-sensitive comparison.
   */
  bool Contains(const nsAString& aValue) const;

  void ParseAtom(const nsAString& aValue);
  void ParseAtomArray(const nsAString& aValue);
  void ParseStringOrAtom(const nsAString& aValue);

  /**
   * Parses an exportparts attribute.
   *
   * https://drafts.csswg.org/css-shadow-parts/#parsing-mapping-list
   */
  void ParsePartMapping(const nsAString&);

  /**
   * Structure for a mapping from int (enum) values to strings.  When you use
   * it you generally create an array of them.
   * Instantiate like this:
   * EnumTable myTable[] = {
   *   { "string1", 1 },
   *   { "string2", 2 },
   *   { nullptr, 0 }
   * }
   */
  struct EnumTable {
    // EnumTable can be initialized either with an int16_t value
    // or a value of an enumeration type that can fit within an int16_t.

    constexpr EnumTable(const char* aTag, int16_t aValue)
        : tag(aTag), value(aValue) {}

    template <typename T,
              typename = typename std::enable_if<std::is_enum<T>::value>::type>
    constexpr EnumTable(const char* aTag, T aValue)
        : tag(aTag), value(static_cast<int16_t>(aValue)) {
      static_assert(mozilla::EnumTypeFitsWithin<T, int16_t>::value,
                    "aValue must be an enum that fits within int16_t");
      // TODO: statically assert there are no duplicate values, otherwise
      // `GetEnumString()` above will return wrong values.
    }

    /** The string the value maps to */
    const char* tag;
    /** The enum value that maps to this string */
    int16_t value;
  };

  /**
   * Parse into an enum value.
   *
   * @param aValue the string to find the value for
   * @param aTable the enumeration to map with
   * @param aCaseSensitive specify if the parsing has to be case sensitive
   * @param aDefaultValue if non-null, this function will always return true.
   *        Failure to parse aValue as one of the values in aTable will just
   *        cause aDefaultValue->value to be stored as the enumeration value.
   * @return whether the enum value was found or not
   */
  bool ParseEnumValue(const nsAString& aValue, const EnumTable* aTable,
                      bool aCaseSensitive,
                      const EnumTable* aDefaultValue = nullptr);

  /**
   * Parse a string into a dimension value.  This is similar to
   * https://html.spec.whatwg.org/multipage/#rules-for-parsing-dimension-values
   * but drops the fractional part of the value for now, until we figure out how
   * to store that in our nsAttrValue.
   *
   * The resulting value (if the parse succeeds) is one of eInteger,
   * eDoubleValue, or ePercent, depending on whether we found a fractional part
   * and whether we found '%' at the end of the value.
   *
   * @param aInput the string to parse
   * @return whether the value could be parsed
   */
  bool ParseHTMLDimension(const nsAString& aInput) {
    return DoParseHTMLDimension(aInput, false);
  }

  /**
   * Parse a string into a nonzero dimension value.  This implements
   * https://html.spec.whatwg.org/multipage/#rules-for-parsing-non-zero-dimension-values
   * subject to the same constraints as ParseHTMLDimension above.
   *
   * @param aInput the string to parse
   * @return whether the value could be parsed
   */
  bool ParseNonzeroHTMLDimension(const nsAString& aInput) {
    return DoParseHTMLDimension(aInput, true);
  }

  /**
   * Parse a string value into an integer.
   *
   * @param aString the string to parse
   * @return whether the value could be parsed
   */
  bool ParseIntValue(const nsAString& aString) {
    return ParseIntWithBounds(aString, INT32_MIN, INT32_MAX);
  }

  /**
   * Parse a string value into an integer with minimum value and maximum value.
   *
   * @param aString the string to parse
   * @param aMin the minimum value (if value is less it will be bumped up)
   * @param aMax the maximum value (if value is greater it will be chopped down)
   * @return whether the value could be parsed
   */
  bool ParseIntWithBounds(const nsAString& aString, int32_t aMin,
                          int32_t aMax = INT32_MAX);

  /**
   * Parse a string value into an integer with a fallback for invalid values.
   * Also allows clamping to a maximum value to support col/colgroup.span (this
   * is not per spec right now).
   *
   * @param aString the string to parse
   * @param aDefault the default value
   * @param aMax the maximum value (if value is greater it will be clamped)
   */
  void ParseIntWithFallback(const nsAString& aString, int32_t aDefault,
                            int32_t aMax = INT32_MAX);

  /**
   * Parse a string value into a non-negative integer.
   * This method follows the rules for parsing non-negative integer from:
   * http://dev.w3.org/html5/spec/infrastructure.html#rules-for-parsing-non-negative-integers
   *
   * @param  aString the string to parse
   * @return whether the value is valid
   */
  bool ParseNonNegativeIntValue(const nsAString& aString);

  /**
   * Parse a string value into a clamped non-negative integer.
   * This method follows the rules for parsing non-negative integer from:
   * https://html.spec.whatwg.org/multipage/infrastructure.html#clamped-to-the-range
   *
   * @param aString the string to parse
   * @param aDefault value to return for negative or invalid values
   * @param aMin minimum value
   * @param aMax maximum value
   */
  void ParseClampedNonNegativeInt(const nsAString& aString, int32_t aDefault,
                                  int32_t aMin, int32_t aMax);

  /**
   * Parse a string value into a positive integer.
   * This method follows the rules for parsing non-negative integer from:
   * http://dev.w3.org/html5/spec/infrastructure.html#rules-for-parsing-non-negative-integers
   * In addition of these rules, the value has to be greater than zero.
   *
   * This is generally used for parsing content attributes which reflecting IDL
   * attributes are limited to only non-negative numbers greater than zero, see:
   * http://dev.w3.org/html5/spec/common-dom-interfaces.html#limited-to-only-non-negative-numbers-greater-than-zero
   *
   * @param aString       the string to parse
   * @return              whether the value was valid
   */
  bool ParsePositiveIntValue(const nsAString& aString);

  /**
   * Parse a string into a color.  This implements what HTML5 calls the
   * "rules for parsing a legacy color value".
   *
   * @param aString the string to parse
   * @return whether the value could be parsed
   */
  bool ParseColor(const nsAString& aString);

  /**
   * Parse a string value into a double-precision floating point value.
   *
   * @param aString the string to parse
   * @return whether the value could be parsed
   */
  bool ParseDoubleValue(const nsAString& aString);

  /**
   * Parse a string into a CSS style rule.
   *
   * @param aString the style attribute value to be parsed.
   * @param aElement the element the attribute is set on.
   * @param aMaybeScriptedPrincipal if available, the scripted principal
   *        responsible for this attribute value, as passed to
   *        Element::ParseAttribute.
   */
  bool ParseStyleAttribute(const nsAString& aString,
                           nsIPrincipal* aMaybeScriptedPrincipal,
                           nsStyledElement* aElement);

  size_t SizeOfExcludingThis(mozilla::MallocSizeOf aMallocSizeOf) const;

  nsAtom* GetStoredAtom() const;
  nsStringBuffer* GetStoredStringBuffer() const;

 private:
  // These have to be the same as in ValueType
  enum ValueBaseType {
    eStringBase = eString,  // 00
    eOtherBase = 0x01,      // 01
    eAtomBase = eAtom,      // 10
    eIntegerBase = 0x03     // 11
  };

  inline ValueBaseType BaseType() const;
  inline bool IsSVGType(ValueType aType) const;

  /**
   * Get the index of an EnumTable in the sEnumTableArray.
   * If the EnumTable is not in the sEnumTableArray, it is added.
   *
   * @param aTable   the EnumTable to get the index of.
   * @return         the index of the EnumTable.
   */
  int16_t GetEnumTableIndex(const EnumTable* aTable);

  inline void SetPtrValueAndType(void* aValue, ValueBaseType aType);
  void SetIntValueAndType(int32_t aValue, ValueType aType,
                          const nsAString* aStringValue);
  // aType can be ePercent or eDoubleValue.
  void SetDoubleValueAndType(double aValue, ValueType aType,
                             const nsAString* aStringValue);
  bool SetColorValue(nscolor aColor, const nsAString& aString);
  void SetMiscAtomOrString(const nsAString* aValue);
  void ResetMiscAtomOrString();
  void SetSVGType(ValueType aType, const void* aValue,
                  const nsAString* aSerialized);
  inline void ResetIfSet();

  inline void* GetPtr() const;
  inline MiscContainer* GetMiscContainer() const;
  inline int32_t GetIntInternal() const;

  // Clears the current MiscContainer.  This will return null if there is no
  // existing container.
  MiscContainer* ClearMiscContainer();
  // Like ClearMiscContainer, except allocates a new container if one does not
  // exist already.
  MiscContainer* EnsureEmptyMiscContainer();
  bool EnsureEmptyAtomArray();
  already_AddRefed<nsStringBuffer> GetStringBuffer(
      const nsAString& aValue) const;
  // Given an enum table and a particular entry in that table, return
  // the actual integer value we should store.
  int32_t EnumTableEntryToValue(const EnumTable* aEnumTable,
                                const EnumTable* aTableEntry);

  template <typename F>
  bool SubstringCheck(const nsAString& aValue,
                      nsCaseTreatment aCaseSensitive) const;

  static MiscContainer* AllocMiscContainer();
  static void DeallocMiscContainer(MiscContainer* aCont);

  static nsTArray<const EnumTable*>* sEnumTableArray;

  /**
   * Helper for ParseHTMLDimension and ParseNonzeroHTMLDimension.
   *
   * @param aInput the string to parse
   * @param aEnsureNonzero whether to fail the parse if the value is 0
   * @return whether the value could be parsed
   */
  bool DoParseHTMLDimension(const nsAString& aInput, bool aEnsureNonzero);

  uintptr_t mBits;
};

inline const nsAttrValue& nsAttrValue::operator=(const nsAttrValue& aOther) {
  SetTo(aOther);
  return *this;
}

inline nsAttrValue::ValueBaseType nsAttrValue::BaseType() const {
  return static_cast<ValueBaseType>(mBits & NS_ATTRVALUE_BASETYPE_MASK);
}

inline void* nsAttrValue::GetPtr() const {
  NS_ASSERTION(BaseType() != eIntegerBase, "getting pointer from non-pointer");
  return reinterpret_cast<void*>(mBits & NS_ATTRVALUE_POINTERVALUE_MASK);
}

inline bool nsAttrValue::IsEmptyString() const { return !mBits; }

#endif
