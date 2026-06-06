#pragma once
#include <map>
#include <optional>
#include <string>

// Queries Epic's public GraphQL catalog API to discover all DLC items
// for a given namespace (SandboxId). Result is cached after first fetch.
namespace DlcCatalog {

    using EntitlementMap = std::map<std::string, std::string>; // id -> title

    std::optional<EntitlementMap> fetch(const std::string& namespace_id);

}
