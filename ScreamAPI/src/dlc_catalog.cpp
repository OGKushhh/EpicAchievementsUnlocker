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

// Scans the GraphQL response JSON for all "id":"...","title":"..." pairs.
// Epic's API consistently puts id before title within each item object.
static DlcCatalog::EntitlementMap parse_catalog_response(const std::string& json){
    DlcCatalog::EntitlementMap result;
    size_t pos = 0;

    while(pos < json.size()){
        auto id_key = json.find("\"id\":\"", pos);
        if(id_key == std::string::npos) break;

        size_t id_start = id_key + 6;
        std::string id = parse_json_string(json, id_start);

        size_t after_id = json.find('"', id_start + id.size());
        if(after_id == std::string::npos) break;
        after_id++;

        auto title_key = json.find("\"title\":\"", after_id);
        if(title_key == std::string::npos) break;

        // Guard: if another "id":" appears before this title, it belongs to
        // a different object. Advance to that id and try again.
        auto next_id = json.find("\"id\":\"", after_id);
        if(next_id != std::string::npos && next_id < title_key){
            pos = next_id;
            continue;
        }

        size_t title_start = title_key + 9;
        std::string title = parse_json_string(json, title_start);

        if(!id.empty())
            result[id] = title;

        pos = json.find('"', title_start + title.size());
        if(pos == std::string::npos) break;
        pos++;
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
    return
        R"({"query":"query($namespace: String!) { Catalog { catalogOffers(namespace: $namespace, params: { count: 1000 }) { elements { items { id title } } } } }","variables":{"namespace":")"
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

    struct Endpoint { std::wstring host; std::wstring path; };
    const Endpoint endpoints[] = {
        { L"launcher.store.epicgames.com", L"/graphql"    },
        { L"graphql.unrealengine.com",     L"/ue/graphql" },
    };

    for(auto& ep : endpoints){
        auto response = https_post(ep.host, ep.path, payload, user_agent);
        if(!response.has_value() || response->empty()){
            Logger::warn("DlcCatalog: endpoint failed, trying next...");
            continue;
        }
        auto map = parse_catalog_response(*response);
        if(map.empty()){
            Logger::warn("DlcCatalog: response had no items, trying next endpoint...");
            continue;
        }
        Logger::dlc("DlcCatalog: fetched %zu item(s) from Epic catalog", map.size());
        return map;
    }

    Logger::warn("DlcCatalog: all endpoints failed for namespace '%s'", namespace_id.c_str());
    return std::nullopt;
}

} // namespace DlcCatalog
