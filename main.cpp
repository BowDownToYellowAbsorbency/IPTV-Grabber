#include <iostream>
#include <ostream>
#include <string>
#include <string_view>
#include <map>
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <execution>

#define BOOST_ALLOW_DEPRECATED_HEADERS

#include "curl_easy.h"
#include "curl_form.h"
#include "curl_ios.h"
#include "curl_exception.h"
#include "boost/algorithm/string/find.hpp"
#include "boost/progress.hpp"
#include "boost/range/irange.hpp"
#include "boost/numeric/conversion/cast.hpp"

#pragma comment(lib, "urlmon.lib")

std::string getPageContent(std::string_view url)
{
    std::stringstream str;
    curl::curl_ios<std::stringstream> writer(str);

    curl::curl_easy easy(writer);

    easy.add<CURLOPT_URL>(url.data());
    easy.add<CURLOPT_FOLLOWLOCATION>(1L);
    try {
        easy.perform();
        return str.str();
    }
    catch (curl::curl_easy_exception error) {
        curl::curlcpp_traceback errors = error.get_traceback();
        error.print_traceback();
    }

    return "";
}

std::string BrowseFolder(std::string saved_path)
{
    TCHAR path[MAX_PATH];
    std::wstring wsaved_path(saved_path.begin(), saved_path.end());
    const wchar_t* path_param = wsaved_path.c_str();

    BROWSEINFO bi = { 0 };
    bi.lpszTitle = ("Browse for folder...");
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lParam = (LPARAM)path_param;
    LPITEMIDLIST pidl = SHBrowseForFolder(&bi);

    if (pidl != 0) {
        //get the name of the folder and put it in path
        SHGetPathFromIDList(pidl, path);

        //free memory used
        IMalloc* imalloc = 0;
        if (SUCCEEDED(SHGetMalloc(&imalloc))) {
            imalloc->Free(pidl);
            imalloc->Release();
        }

        return path;
    }

    return "";
}

size_t write_data(void* ptr, size_t size, size_t nmemb, FILE* stream)
{
    size_t written;
    written = fwrite(ptr, size, nmemb, stream);
    return written;
}

long GetFileSize(std::string filename)
{
    struct stat stat_buf;
    int rc = stat(filename.c_str(), &stat_buf);
    return rc == 0 ? stat_buf.st_size : -1;
}

std::string ReadNthLine(const std::string& filename, int N)
{
    std::ifstream in(filename.c_str());

    std::string s;
    //for performance
    s.reserve(255);

    //skip N lines
    for (int i = 0; i < N; ++i)
        std::getline(in, s);

    std::getline(in, s);
    return s;
}

int main()
{
    std::cout << "Select a folder in which you want to save the exported playlists to.." << std::endl;
    std::string outputDir = BrowseFolder(getenv("USERPROFILE"));
    std::string response = getPageContent(std::string_view("http://www.oneplaylist.space/database/exportall"));
    if (outputDir != "") {
        std::vector<std::string> playlists;
        int breaks = 0, nPos = 0;
        while ((nPos = response.find("<br />", nPos)) != std::string::npos) {
            breaks++;
            nPos += 6;
        }
        std::cout << "URL grabbing operation in progress.." << std::endl;
        boost::progress_display progress(breaks);
        auto grabStart = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < breaks; i += 2) {
            int breakIndex = distance(response.begin(), boost::algorithm::find_nth(response, "<br />", i).begin());
            int breakIndex2 = distance(response.begin(), boost::algorithm::find_nth(response, "<br />", i + 1).begin());
            std::string EXTINF = response.substr(breakIndex + 6, breakIndex2 - breakIndex - 6);
            if (EXTINF.find("get.php") != std::string::npos) {
                playlists.push_back(EXTINF);
            }
            progress += 2;
        }
        int grabOpTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - grabStart).count();
        std::cout << "\n\nURL grabbing operation finished in " << grabOpTime << " milliseconds!"
            << "\nPlaylist downloading operation in progress.." << std::endl;
        boost::progress_display progress2(playlists.size());
        auto downloadStart = std::chrono::high_resolution_clock::now();
        boost::integer_range<int> playlistsRange = boost::irange(0, boost::numeric_cast<int>(playlists.size()));
        std::for_each_n(std::execution::par, playlistsRange.begin(), boost::size(playlistsRange), [&](int i) {
            CURL* curl;
            FILE* fp;
            CURLcode res;
            curl = curl_easy_init();
            if (curl) {
                std::string finalFile = outputDir + "\\" + std::to_string(i) + ".m3u";
                fp = fopen(finalFile.c_str(), "wb");
                curl_easy_setopt(curl, CURLOPT_URL, playlists[i].c_str());
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
                curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
                curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
                curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 1);
                curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, 200);
                res = curl_easy_perform(curl);
                curl_easy_cleanup(curl);
                fclose(fp);
            }
            progress2 += 1;
        });
        int downloadOpTime = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now() - downloadStart).count();
        std::cout << "\n\nPlaylist downloading operation finished in " << downloadOpTime << " seconds!"
            << "\nCleaning out garbage downloaded files..." << std::endl;
        auto cleanStart = std::chrono::high_resolution_clock::now();
        boost::progress_display progress3(playlists.size());
        std::for_each_n(std::execution::par, playlistsRange.begin(), boost::size(playlistsRange), [&](int i) {
            std::string finalFile = outputDir + "\\" + std::to_string(i) + ".m3u";
            std::ifstream file(finalFile);
            if (GetFileSize(finalFile) < 500) {
                file.close();
                remove(finalFile.c_str());
            }
            else {
                std::string extm3u = ReadNthLine(finalFile, 2);
                getline(file, extm3u);
                if (extm3u.find("EXTM3U") == std::string::npos) {
                    file.close();
                    remove(finalFile.c_str());
                }
            }
            progress3 += 1;
        });
        int cleanOpTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - cleanStart).count();
        std::cout << "\n\nCleaning operation finished in " << cleanOpTime << " milliseconds!" << std::endl;
        std::cout << "In total, the whole operation finished in " << (grabOpTime / 1000 + downloadOpTime + cleanOpTime / 1000) << " seconds." << std::endl;
    }
    std::cout << "Done! Press any key to exit.." << std::endl;
    system("pause");
    return 0;
}