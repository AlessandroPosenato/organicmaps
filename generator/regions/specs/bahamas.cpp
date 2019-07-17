#include "generator/regions/specs/bahamas.hpp"

namespace generator
{
namespace regions
{
namespace specs
{
PlaceLevel BahamasSpecifier::GetSpecificCountryLevel(Region const & region) const
{
  AdminLevel adminLevel = region.GetAdminLevel();
  switch (adminLevel)
  {
  case AdminLevel::Six:
    return PlaceLevel::Region;  // Central government district - New Providence (main island) only
  case AdminLevel::Eight:
    return PlaceLevel::Region;  // Districts of the Bahamas (Local government) (other islands)
  default: break;
  }

  return PlaceLevel::Unknown;
}
}  // namespace specs
}  // namespace regions
}  // namespace generator
