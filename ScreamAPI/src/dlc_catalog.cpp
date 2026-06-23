#include "pch.h"
#include "dlc_catalog.h"
#include <Logger.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

namespace {

// Extracts the string value starting just after an opening quote.
// Handles \-escape sequences. Stops at the closing quote.
static std::string parse_json_string(const std::string& json, size_t start){
    std::string result;
    result.reserve(64);
    for(size_t i = start; i < json.size(); i++){
        if(json[i] == '\\' && i + 1 < json.size()){
            ++i;
            switch(json[i]){
                case '"':  result += '"';  break;
                case '\\': result += '\\'; break;
                case '/':  result += '/';  break;
                case 'n':  result += '\n'; break;
                case 'r':  result += '\r'; break;
                case 't':  result += '\t'; break;
                default:   result += json[i]; break;
            }
        } else if(json[i] == '"'){
            break;
        } else {
            result += json[i];
        }
    }
    return result;
}

// Parses the GraphQL response and builds a map of item_id -> offer_title.
//
// Actual response structure (verified from Epic's live API):
//   {
//     "data": {
//       "Catalog": {
//         "catalogOffers": {
//           "elements": [
//             { "title": "DLC Name", "items": [ { "id": "catalog-item-id" } ] }
//           ]
//         }
//       }
//     }
//   }
//
// This function locates the innermost "elements" array and parses its contents.
// Uses two sequential finds instead of a single combined key so that whitespace
// between tokens (e.g. "catalogOffers": { "elements": [) is handled correctly.
static DlcCatalog::EntitlementMap parse_catalog_response(const std::string& json){
    DlcCatalog::EntitlementMap result;

    // Step 1: find "catalogOffers"
    auto co_pos = json.find("\"catalogOffers\"");
    if(co_pos == std::string::npos){
        Logger::warn("parse_catalog_response: 'catalogOffers' not found");
        return result;
    }

    // Step 2: find "elements" after it
    auto el_pos = json.find("\"elements\"", co_pos);
    if(el_pos == std::string::npos){
        Logger::warn("parse_catalog_response: 'elements' not found");
        return result;
    }

    // Step 3: skip to the opening '[' of the array
    auto bracket = json.find('[', el_pos);
    if(bracket == std::string::npos){
        Logger::warn("parse_catalog_response: elements '[' not found");
        return result;
    }
    size_t pos = bracket + 1; // pos now points to first char after '['

    // Parse each offer object inside the elements array
    while(pos < json.size()){
        // Advance to next offer object (or end of array)
        while(pos < json.size() && json[pos] != '{' && json[pos] != ']') pos++;
        if(pos >= json.size() || json[pos] == ']') break;

        // Find the extent of this offer object by tracking brace depth
        int depth = 0;
        size_t obj_end = pos;
        for(size_t i = pos; i < json.size(); i++){
            if     (json[i] == '{') depth++;
            else if(json[i] == '}'){
                if(--depth == 0){ obj_end = i; break; }
            }
        }
        if(obj_end == pos){ pos++; continue; }
        const std::string obj = json.substr(pos, obj_end - pos + 1);

        // Extract offer-level title (whitespace-tolerant: find key, then colon, then opening quote)
        std::string title;
        auto tk = obj.find("\"title\"");
        if(tk != std::string::npos){
            auto colon = obj.find(':', tk);
            if(colon != std::string::npos){
                auto quote = obj.find('"', colon + 1);
                if(quote != std::string::npos)
                    title = parse_json_string(obj, quote + 1);
            }
        }

        // Map every item.id inside "items":[...] to this offer's title
        if(!title.empty()){
            auto items_kw = obj.find("\"items\"");
            if(items_kw != std::string::npos){
                auto iopen = obj.find('[', items_kw);
                if(iopen != std::string::npos){
                    size_t ipos = iopen + 1;
                    while(ipos < obj.size()){
                        while(ipos < obj.size() && obj[ipos] != '{' && obj[ipos] != ']') ipos++;
                        if(ipos >= obj.size() || obj[ipos] == ']') break;

                        // Find "id" key, then colon, then opening quote (whitespace-tolerant)
                        auto ik = obj.find("\"id\"", ipos);
                        if(ik == std::string::npos) break;
                        auto colon = obj.find(':', ik);
                        if(colon == std::string::npos) break;
                        auto quote = obj.find('"', colon + 1);
                        if(quote == std::string::npos) break;
                        std::string item_id = parse_json_string(obj, quote + 1);
                        if(!item_id.empty())
                            result[item_id] = title;

                        // Skip past this item object
                        while(ipos < obj.size() && obj[ipos] != '}') ipos++;
                        ipos++;
                    }
                }
            }
        }

        pos = obj_end + 1;
    }

    return result;
}

// Synchronous HTTPS POST via WinHTTP. Returns response body or nullopt.
static std::optional<std::string> https_post(
    const std::wstring& host,
    const std::wstring& path,
    const std::string&  body,
    const std::wstring& user_agent
){
    HINTERNET hSession = WinHttpOpen(
        user_agent.c_str(),
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if(!hSession){ Logger::warn("DlcCatalog: WinHttpOpen failed (%lu)", GetLastError()); return std::nullopt; }

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if(!hConnect){
        Logger::warn("DlcCatalog: WinHttpConnect failed (%lu)", GetLastError());
        WinHttpCloseHandle(hSession); return std::nullopt;
    }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"POST", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if(!hRequest){
        Logger::warn("DlcCatalog: WinHttpOpenRequest failed (%lu)", GetLastError());
        WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return std::nullopt;
    }

    WinHttpAddRequestHeaders(hRequest,
        L"Content-Type: application/json", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    BOOL ok = WinHttpSendRequest(hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);

    if(!ok || !WinHttpReceiveResponse(hRequest, nullptr)){
        Logger::warn("DlcCatalog: request failed (%lu)", GetLastError());
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return std::nullopt;
    }

    std::string response;
    DWORD available = 0;
    while(WinHttpQueryDataAvailable(hRequest, &available) && available > 0){
        std::string chunk(available, '\0');
        DWORD read = 0;
        if(!WinHttpReadData(hRequest, chunk.data(), available, &read)) break;
        chunk.resize(read);
        response += chunk;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return response;
}

static std::string build_payload(const std::string& namespace_id){
    // title lives on the OFFER (CatalogOffer), not on the nested item (CatalogItem).
    // item.id is what EOS_Ecom_QueryOwnership returns, so that is our map key.
    return
        R"({"query":"query($namespace: String!) { Catalog { catalogOffers(namespace: $namespace, params: { count: 1000 }) { elements { title items { id } } } } }","variables":{"namespace":")"
        + namespace_id + R"("}})";
}

} // namespace

namespace DlcCatalog {

std::optional<EntitlementMap> fetch(const std::string& namespace_id){
    if(namespace_id.empty()){
        Logger::warn("DlcCatalog::fetch: namespace_id is empty");
        return std::nullopt;
    }

    Logger::dlc("DlcCatalog: fetching catalog for namespace '%s'", namespace_id.c_str());

    const std::string payload = build_payload(namespace_id);
    const std::wstring user_agent = L"EpicGamesLauncher/18.9.0-45233261+++Portal+Release-Live";

    // Only the launcher.store.epicgames.com endpoint works as of June 2026.
    // The graphql.unrealengine.com endpoint returns 410 Gone and is no longer used.
    const std::wstring host = L"launcher.store.epicgames.com";
    const std::wstring path = L"/graphql";

    auto response = https_post(host, path, payload, user_agent);
    if(!response.has_value() || response->empty()){
        Logger::warn("DlcCatalog: request failed for namespace '%s'", namespace_id.c_str());
        return std::nullopt;
    }

    auto map = parse_catalog_response(*response);
    if(map.empty()){
        Logger::warn("DlcCatalog: response had no items for namespace '%s'", namespace_id.c_str());
        return std::nullopt;
    }

    Logger::dlc("DlcCatalog: fetched %zu item(s) from Epic catalog", map.size());
    return map;
}

} // namespace DlcCatalog