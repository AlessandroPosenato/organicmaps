#include "indexer/feature_data.hpp"

#include "indexer/classificator.hpp"
#include "indexer/feature.hpp"
#include "indexer/ftypes_matcher.hpp"

#include "base/assert.hpp"
#include "base/macros.hpp"
#include "base/stl_helpers.hpp"
#include "base/string_utils.hpp"

#include <algorithm>
#include <functional>
#include <sstream>
#include <vector>

using namespace feature;

////////////////////////////////////////////////////////////////////////////////////
// TypesHolder implementation
////////////////////////////////////////////////////////////////////////////////////

namespace feature
{
using namespace std;

template <class ContT> string TypesToString(ContT const & holder)
{
  Classificator const & c = classif();
  string s;
  for (uint32_t const type : holder)
    s += c.GetReadableObjectName(type) + " ";
  if (!s.empty())
    s.pop_back();
  return s;
}

std::string DebugPrint(TypesHolder const & holder)
{
  return TypesToString(holder);
}

TypesHolder::TypesHolder(FeatureType & f) : m_size(0), m_geomType(f.GetGeomType())
{
  f.ForEachType([this](uint32_t type)
  {
    Add(type);
  });
}

bool TypesHolder::HasWithSubclass(uint32_t type) const
{
  uint8_t const level = ftype::GetLevel(type);
  for (uint32_t t : *this)
  {
    ftype::TruncValue(t, level);
    if (t == type)
      return true;
  }
  return false;
}

void TypesHolder::Remove(uint32_t type)
{
  UNUSED_VALUE(RemoveIf(base::EqualFunctor<uint32_t>(type)));
}

bool TypesHolder::Equals(TypesHolder const & other) const
{
  if (m_size != other.m_size)
    return false;

  // Dynamic vector + sort for kMaxTypesCount array is a huge overhead.

  auto const b = begin();
  auto const e = end();
  for (auto t : other)
  {
    if (std::find(b, e, t) == e)
      return false;
  }
  return true;
}

namespace
{
class UselessTypesChecker
{
public:
  static UselessTypesChecker const & Instance()
  {
    static UselessTypesChecker const inst;
    return inst;
  }

  /// @return Type score, less is better.
  uint8_t Score(uint32_t t) const
  {
    ftype::TruncValue(t, 2);
    if (IsIn(2, t))
      return 3;

    ftype::TruncValue(t, 1);
    if (IsIn(1, t))
      return 2;

    if (IsIn(0, t))
      return 1;

    return 0;
  }

  template <class ContT> void SortUselessToEnd(ContT & cont) const
  {
    // Put "very common" types to the end of possible PP-description types.
    std::stable_sort(cont.begin(), cont.end(), [this](uint32_t t1, uint32_t t2)
    {
      return Score(t1) < Score(t2);
    });
  }

private:
  UselessTypesChecker()
  {
    // Fill types that will be taken into account last,
    // when we have many types for POI.
    base::StringIL const types1[] = {
        // 1-arity
        {"building:part"},
        {"hwtag"},
        {"psurface"},
        {"internet_access"},
        {"organic"},
        {"wheelchair"},
        {"cuisine"},
        {"recycling"},
        {"area:highway"},
        {"fee"},
    };

    Classificator const & c = classif();

    m_types[0].push_back(c.GetTypeByPath({"building"}));

    m_types[1].reserve(std::size(types1));
    for (auto const & type : types1)
      m_types[1].push_back(c.GetTypeByPath(type));

    // Put _most_ useless types here, that are not fit in the arity logic above.
    // This change is for generator, to eliminate "lit" type first when max types count exceeded.
    m_types[2].push_back(c.GetTypeByPath({"hwtag", "lit"}));

    for (auto & v : m_types)
      std::sort(v.begin(), v.end());
  }

  bool IsIn(uint8_t idx, uint32_t t) const
  {
    return std::binary_search(m_types[idx].begin(), m_types[idx].end(), t);
  }

  vector<uint32_t> m_types[3];
};
} // namespace

uint8_t CalculateHeader(size_t const typesCount, HeaderGeomType const headerGeomType,
                        FeatureParamsBase const & params)
{
  ASSERT(typesCount != 0, ("Feature should have at least one type."));
  ASSERT_LESS_OR_EQUAL(typesCount, kMaxTypesCount, ());
  uint8_t header = static_cast<uint8_t>(typesCount - 1);

  if (!params.name.IsEmpty())
    header |= HEADER_MASK_HAS_NAME;

  if (params.layer != LAYER_EMPTY)
    header |= HEADER_MASK_HAS_LAYER;

  header |= static_cast<uint8_t>(headerGeomType);

  // Geometry type for additional info is only one.
  switch (headerGeomType)
  {
  case HeaderGeomType::Point:
    if (params.rank != 0)
      header |= HEADER_MASK_HAS_ADDINFO;
    break;
  case HeaderGeomType::Line:
    if (!params.ref.empty())
      header |= HEADER_MASK_HAS_ADDINFO;
    break;
  case HeaderGeomType::Area:
  case HeaderGeomType::PointEx:
    if (!params.house.IsEmpty())
      header |= HEADER_MASK_HAS_ADDINFO;
    break;
  }

  return header;
}

void TypesHolder::SortByUseless()
{
  UselessTypesChecker::Instance().SortUselessToEnd(*this);
}

void TypesHolder::SortBySpec()
{
  auto const & cl = classif();
  auto const getPriority = [&cl](uint32_t type)
  {
    return cl.GetObject(type)->GetMaxOverlaysPriority();
  };

  std::stable_sort(begin(), end(), [&getPriority](uint32_t t1, uint32_t t2)
  {
    return getPriority(t1) > getPriority(t2);
  });
}

vector<string> TypesHolder::ToObjectNames() const
{
  Classificator const & c = classif();
  vector<string> result;
  for (uint32_t const type : *this)
    result.push_back(c.GetReadableObjectName(type));
  return result;
}
}  // namespace feature

////////////////////////////////////////////////////////////////////////////////////
// FeatureParamsBase implementation
////////////////////////////////////////////////////////////////////////////////////

void FeatureParamsBase::MakeZero()
{
  layer = 0;
  rank = 0;
  ref.clear();
  house.Clear();
  name.Clear();
}

bool FeatureParamsBase::operator == (FeatureParamsBase const & rhs) const
{
  return (name == rhs.name && house == rhs.house && ref == rhs.ref &&
          layer == rhs.layer && rank == rhs.rank);
}

bool FeatureParamsBase::IsValid() const
{
  return layer >= LAYER_LOW && layer <= LAYER_HIGH;
}

string FeatureParamsBase::DebugString() const
{
  string const utf8name = DebugPrint(name);
  return ((!utf8name.empty() ? "Name:" + utf8name : "") +
          (layer != LAYER_EMPTY ? " Layer:" + DebugPrint((int)layer) : "") +
          (rank != 0 ? " Rank:" + DebugPrint((int)rank) : "") +
          (!house.IsEmpty() ? " House:" + house.Get() : "") +
          (!ref.empty() ? " Ref:" + ref : ""));
}

bool FeatureParamsBase::IsEmptyNames() const
{
  return name.IsEmpty() && house.IsEmpty() && ref.empty();
}

namespace
{

bool IsDummyName(string_view s)
{
  return s.empty();
}

} // namespace

/////////////////////////////////////////////////////////////////////////////////////////
// FeatureParams implementation
/////////////////////////////////////////////////////////////////////////////////////////

void FeatureParams::ClearName()
{
  name.Clear();
}

bool FeatureParams::AddName(string_view lang, string_view s)
{
  if (IsDummyName(s))
    return false;

  // The "default" new name will replace the old one if any (e.g. from AddHouseName call).
  name.AddString(lang, s);
  return true;
}

bool FeatureParams::AddHouseName(string const & s)
{
  if (IsDummyName(s) || name.FindString(s) != StringUtf8Multilang::kUnsupportedLanguageCode)
    return false;

  // Most names are house numbers by statistics.
  if (house.IsEmpty() && AddHouseNumber(s))
    return true;

  // If we got a clear number, replace the house number with it.
  // Example: housename=16th Street, housenumber=34
  if (strings::IsASCIINumeric(s))
  {
    string housename(house.Get());
    if (AddHouseNumber(s))
    {
      // Duplicating code to avoid changing the method header.
      string_view dummy;
      if (!name.GetString(StringUtf8Multilang::kDefaultCode, dummy))
        name.AddString(StringUtf8Multilang::kDefaultCode, housename);
      return true;
    }
  }

  // Add as a default name if we don't have it yet.
  string_view dummy;
  if (!name.GetString(StringUtf8Multilang::kDefaultCode, dummy))
  {
    name.AddString(StringUtf8Multilang::kDefaultCode, s);
    return true;
  }

  return false;
}

bool FeatureParams::AddHouseNumber(string houseNumber)
{
  ASSERT(!houseNumber.empty(), ("This check should be done by the caller."));
  ASSERT_NOT_EQUAL(houseNumber.front(), ' ', ("Trim should be done by the caller."));

  // Negative house numbers are not supported.
  if (houseNumber.front() == '-' || houseNumber.find("－") == 0)
    return false;

  // Replace full-width digits, mostly in Japan, by ascii-ones.
  strings::NormalizeDigits(houseNumber);

  // Remove leading zeroes from house numbers.
  // It's important for debug checks of serialized-deserialized feature.
  size_t i = 0;
  while (i + 1 < houseNumber.size() && houseNumber[i] == '0')
    ++i;
  houseNumber.erase(0, i);

  if (any_of(houseNumber.cbegin(), houseNumber.cend(), &strings::IsASCIIDigit))
  {
    house.Set(houseNumber);
    return true;
  }
  return false;
}

void FeatureParams::SetGeomType(feature::GeomType t)
{
  switch (t)
  {
  case GeomType::Point: m_geomType = HeaderGeomType::Point; break;
  case GeomType::Line: m_geomType = HeaderGeomType::Line; break;
  case GeomType::Area: m_geomType = HeaderGeomType::Area; break;
  default: ASSERT(false, ());
  }
}

void FeatureParams::SetGeomTypePointEx()
{
  ASSERT(m_geomType == HeaderGeomType::Point || m_geomType == HeaderGeomType::PointEx, ());
  ASSERT(!house.IsEmpty(), ());

  m_geomType = HeaderGeomType::PointEx;
}

feature::GeomType FeatureParams::GetGeomType() const
{
  CHECK(IsValid(), ());
  switch (*m_geomType)
  {
  case HeaderGeomType::Line: return GeomType::Line;
  case HeaderGeomType::Area: return GeomType::Area;
  default: return GeomType::Point;
  }
}

HeaderGeomType FeatureParams::GetHeaderGeomType() const
{
  CHECK(IsValid(), ());
  return *m_geomType;
}

void FeatureParams::SetRwSubwayType(char const * cityName)
{
  Classificator const & c = classif();

  static uint32_t const src = c.GetTypeByPath({"railway", "station"});
  uint32_t const dest = c.GetTypeByPath({"railway", "station", "subway", cityName});

  for (size_t i = 0; i < m_types.size(); ++i)
  {
    uint32_t t = m_types[i];
    ftype::TruncValue(t, 2);
    if (t == src)
    {
      m_types[i] = dest;
      break;
    }
  }
}

FeatureParams::TypesResult FeatureParams::FinishAddingTypesEx()
{
  base::SortUnique(m_types);

  TypesResult res = TYPES_GOOD;

  if (m_types.size() > kMaxTypesCount)
  {
    UselessTypesChecker::Instance().SortUselessToEnd(m_types);

    m_types.resize(kMaxTypesCount);
    sort(m_types.begin(), m_types.end());

    res = TYPES_EXCEED_MAX;
  }

  // Patch fix that removes house number from localities.
  /// @todo move this fix elsewhere (osm2type.cpp?)
  if (!house.IsEmpty() && ftypes::IsLocalityChecker::Instance()(m_types))
    house.Clear();

  return (m_types.empty() ? TYPES_EMPTY : res);
}

std::string FeatureParams::PrintTypes()
{
  base::SortUnique(m_types);
  UselessTypesChecker::Instance().SortUselessToEnd(m_types);
  return TypesToString(m_types);
}

void FeatureParams::SetType(uint32_t t)
{
  m_types.clear();
  m_types.push_back(t);
}

bool FeatureParams::PopAnyType(uint32_t & t)
{
  CHECK(!m_types.empty(), ());
  t = m_types.back();
  m_types.pop_back();
  return m_types.empty();
}

bool FeatureParams::PopExactType(uint32_t t)
{
  m_types.erase(remove(m_types.begin(), m_types.end(), t), m_types.end());
  return m_types.empty();
}

bool FeatureParams::IsTypeExist(uint32_t t) const
{
  return base::IsExist(m_types, t);
}

bool FeatureParams::IsTypeExist(uint32_t comp, uint8_t level) const
{
  return FindType(comp, level) != ftype::GetEmptyValue();
}

uint32_t FeatureParams::FindType(uint32_t comp, uint8_t level) const
{
  for (uint32_t const type : m_types)
  {
    uint32_t t = type;
    ftype::TruncValue(t, level);
    if (t == comp)
      return type;
  }
  return ftype::GetEmptyValue();
}

bool FeatureParams::IsValid() const
{
  if (m_types.empty() || m_types.size() > kMaxTypesCount || !m_geomType)
    return false;

  return FeatureParamsBase::IsValid();
}

uint8_t FeatureParams::GetHeader() const
{
  return CalculateHeader(m_types.size(), GetHeaderGeomType(), *this);
}

uint32_t FeatureParams::GetIndexForType(uint32_t t)
{
  return classif().GetIndexForType(t);
}

uint32_t FeatureParams::GetTypeForIndex(uint32_t i)
{
  return classif().GetTypeForIndex(i);
}

void FeatureBuilderParams::AddStreet(string s)
{
  // Replace \n with spaces because we write addresses to txt file.
  replace(s.begin(), s.end(), '\n', ' ');

  m_addrTags.Add(AddressData::Type::Street, s);
}

void FeatureBuilderParams::AddPostcode(string const & s)
{
  m_addrTags.Add(AddressData::Type::Postcode, s);
}

namespace
{

// Define types that can't live together in a feature.
class YesNoTypes
{
  std::vector<std::pair<uint32_t, uint32_t>> m_types;

public:
  YesNoTypes()
  {
    // Remain first type and erase second in case of conflict.
    base::StringIL arr[][2] = {
      {{"hwtag", "yescar"}, {"hwtag", "nocar"}},
      {{"hwtag", "yesfoot"}, {"hwtag", "nofoot"}},
      {{"hwtag", "yesbicycle"}, {"hwtag", "nobicycle"}},
      {{"hwtag", "nobicycle"}, {"hwtag", "bidir_bicycle"}},
      {{"hwtag", "nobicycle"}, {"hwtag", "onedir_bicycle"}},
      {{"hwtag", "bidir_bicycle"}, {"hwtag", "onedir_bicycle"}},
      {{"wheelchair", "yes"}, {"wheelchair", "no"}},
    };

    auto const & cl = classif();
    for (auto const & p : arr)
      m_types.emplace_back(cl.GetTypeByPath(p[0]), cl.GetTypeByPath(p[1]));
  }

  bool RemoveInconsistent(std::vector<uint32_t> & types) const
  {
    size_t const szBefore = types.size();
    for (auto const & p : m_types)
    {
      uint32_t skip;
      bool found1 = false, found2 = false;
      for (uint32_t t : types)
      {
        if (t == p.first)
          found1 = true;
        if (t == p.second)
        {
          found2 = true;
          skip = t;
        }
      }

      if (found1 && found2)
        base::EraseIf(types, [skip](uint32_t t) { return skip == t; });
    }

    return szBefore != types.size();
  }
};

} // namespace

bool FeatureBuilderParams::RemoveInconsistentTypes()
{
  static YesNoTypes ynTypes;
  return ynTypes.RemoveInconsistent(m_types);
}

string DebugPrint(FeatureParams const & p)
{
  string res = "Types: " + TypesToString(p.m_types) + "; ";
  return (res + p.DebugString());
}

string DebugPrint(FeatureBuilderParams const & p)
{
  ostringstream oss;
  oss << "ReversedGeometry: " << (p.GetReversedGeometry() ? "true" : "false") << "; ";
  oss << DebugPrint(p.GetMetadata()) << "; ";
  oss << DebugPrint(p.GetAddressData()) << "; ";
  oss << DebugPrint(static_cast<FeatureParams const &>(p));
  return oss.str();
}
