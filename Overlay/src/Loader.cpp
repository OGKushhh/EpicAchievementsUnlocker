#include "pch.h"
#include "Loader.h"
#include "Overlay.h"
#include <fstream>
#include <future>
#include <Config.h>

// Instructions on how to build libcurl on Windows can be found here:
// https://www.youtube.com/watch?reload=9&v=q_mXVZ6VJs4
#pragma comment(lib,"Ws2_32.lib")
#pragma comment(lib,"Wldap32.lib")
#pragma comment(lib,"Crypt32.lib")
#pragma comment(lib,"Normaliz.lib")
#define CURL_STATICLIB
#include "curl/curl.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"

namespace Loader{

#define CACHE_DIR ".ScreamApi_Cache"

bool init(){
#pragma warning(suppress: 26812)
	CURLcode errorCode = curl_global_init(CURL_GLOBAL_ALL);
	if(errorCode != CURLE_OK){
		Logger::error("Loader: Failed to initialize curl. Error code: %d", errorCode);
		return false;
	}

	auto success = CreateDirectoryA(CACHE_DIR, NULL);
	if(success || GetLastError() == ERROR_ALREADY_EXISTS){
		Logger::ovrly("Loader: Successfully initialized");
		return true;
	} else{
		Logger::error("Loader: Failed to create '%s' directory. Error code: %d", CACHE_DIR, GetLastError());
		return false;
	}
}

void shutdown(){
	curl_global_cleanup();

	if(!Config::CacheIcons()){
		if(!RemoveDirectoryA(CACHE_DIR))
			Logger::error("Failed to remove %s directory. Error code: %d", CACHE_DIR, GetLastError());
	}

	Logger::ovrly("Loader: Shutdown");
}

std::string getIconPath(Overlay_Achievement& achievement){
	return CACHE_DIR"\\" + std::string(achievement.AchievementId) + ".png";
}

// FIX: guard against null DX11 device — this function is called from both the
// DX11 path (where gD3D11Device is valid) and the DX12 fallback path (where it
// is set before AsyncLoadIcons is called). The guard prevents a null-deref if
// somehow called before the device is available (e.g. a real DX12 game).
void loadIconTexture(Overlay_Achievement& achievement){
	static std::mutex loadIconMutex;
	{
		std::lock_guard<std::mutex> guard(loadIconMutex);

		// Safety: DX12 games do not have a DX11 device — skip icon loading
		if (!Overlay::gD3D11Device || !Overlay::gContext) {
			Logger::ovrly("loadIconTexture: skipping — no DX11 device (DX12 game)");
			return;
		}

		Logger::ovrly("Loading icon texure for achievement: %s", achievement.AchievementId);
		auto iconPath = getIconPath(achievement);

		int image_width = 0;
		int image_height = 0;
		auto* image_data = stbi_load(iconPath.c_str(), &image_width, &image_height, NULL, 4);
		if(image_data == NULL){
			Logger::error("Failed to load icon: %s. Failure reason: %s", iconPath.c_str(), stbi_failure_reason());
			return;
		}

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = image_width;
		desc.Height = image_height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		HRESULT result;
		ID3D11Texture2D* pTexture = nullptr;
		if(FAILED(result = Overlay::gD3D11Device->CreateTexture2D(&desc, nullptr, &pTexture))){
			Logger::error("Failed to load the texture. Error code: 0x%x", result);
			stbi_image_free(image_data);
			return;
		}

		D3D11_BOX box{};
		box.right = image_width;
		box.bottom = image_height;
		box.back = 1;
		Overlay::gContext->UpdateSubresource(pTexture, 0, &box, image_data, desc.Width * 4, desc.Width * 4 * desc.Height);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = desc.MipLevels;
		srvDesc.Texture2D.MostDetailedMip = 0;

		if(FAILED(result = Overlay::gD3D11Device->CreateShaderResourceView(pTexture, &srvDesc, &achievement.IconTexture))){
			Logger::error("Failed to create shader resource view. Error code: %x", result);
		}

		pTexture->Release();
		stbi_image_free(image_data);
	}
}

void downloadFile(const char* url, const char* filename){
	FILE* file_handle;
	CURL* curl_handle = curl_easy_init();

	Logger::ovrly("Downloading icon to: %s", filename);

	errno_t err = fopen_s(&file_handle, filename, "wb");
	if(!err && file_handle != NULL){
		curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, file_handle);
		curl_easy_setopt(curl_handle, CURLOPT_URL, url);
		curl_easy_perform(curl_handle);
		fclose(file_handle);
	} else{
		Logger::error("Failed to open file for writing: %s", filename);
	}

	curl_easy_cleanup(curl_handle);
}

int getLocalFileSize(WIN32_FILE_ATTRIBUTE_DATA& fileInfo){
	LARGE_INTEGER size;
	size.HighPart = fileInfo.nFileSizeHigh;
	size.LowPart = fileInfo.nFileSizeLow;
	return (int)size.QuadPart;
}

int getOnlineFileSize(const char* url){
	CURL* curl_handle = curl_easy_init();
	curl_easy_setopt(curl_handle, CURLOPT_URL, url);
	curl_easy_setopt(curl_handle, CURLOPT_HEADER, 1);
	curl_easy_setopt(curl_handle, CURLOPT_NOBODY, 1);
	curl_easy_setopt(curl_handle, CURLOPT_CUSTOMREQUEST, "GET");
	curl_easy_perform(curl_handle);
	curl_off_t contentLength;
	curl_easy_getinfo(curl_handle, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &contentLength);
	curl_easy_cleanup(curl_handle);

	return (int)contentLength;
}

void downloadIconIfNecessary(Overlay_Achievement& achievement){
	if(std::string(achievement.UnlockedIconURL).rfind("http", 0) == std::string::npos) {
		Logger::ovrly("Ignoring invalid icon URL: %s", achievement.UnlockedIconURL);
		return;
	}

	auto iconPath = getIconPath(achievement);

	WIN32_FILE_ATTRIBUTE_DATA fileInfo;
	auto fileAttributes = GetFileAttributesExA(iconPath.c_str(), GetFileExInfoStandard, &fileInfo);
	if(fileAttributes){
		if(Config::ValidateIcons()){
			if(getLocalFileSize(fileInfo) == getOnlineFileSize(achievement.UnlockedIconURL)) {
				Logger::ovrly("Using cached icon: %s", iconPath.c_str());
			} else{
				downloadFile(achievement.UnlockedIconURL, iconPath.c_str());
			}
		} else {
			Logger::ovrly("Using cached icon: %s", iconPath.c_str());
		}
	} else if(GetLastError() == ERROR_FILE_NOT_FOUND){
		downloadFile(achievement.UnlockedIconURL, iconPath.c_str());
	} else{
		Logger::error("Failed to read file attributes. Error code: %d", GetLastError());
		return;
	}

	loadIconTexture(achievement);

	if(!Config::CacheIcons()){
		if(!DeleteFileA(iconPath.c_str()))
			Logger::error("Failed to remove %s file. Error code: %d", iconPath.c_str(), GetLastError());
	}
}

void AsyncLoadIcons(){
	if(Config::LoadIcons() && init()){
		static std::vector<std::future<void>>asyncJobs;

		for(auto& achievement : *Overlay::achievements){
			asyncJobs.emplace_back(std::async(std::launch::async, downloadIconIfNecessary, std::ref(achievement)));
		}
		static auto awaitFuture = std::async(std::launch::async, [&](){
			for(auto& job : asyncJobs){
				job.wait();
			}
			asyncJobs.clear();
			shutdown();
		});
	}
}

}
