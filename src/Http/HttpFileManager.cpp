﻿/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include <sys/stat.h>
#if !defined(_WIN32)
#include <dirent.h>
#endif //!defined(_WIN32)
#include <iomanip>
#include "HttpFileManager.h"
#include "Util/File.h"
#include "HttpConst.h"
#include "HttpSession.h"
#include "Record/HlsMediaSource.h"

namespace mediakit {

// hls的播放cookie缓存时间默认60秒，
// 每次访问一次该cookie，那么将重新刷新cookie有效期
// 假如播放器在60秒内都未访问该cookie，那么将重新触发hls播放鉴权
static int kHlsCookieSecond = 60;
static const string kCookieName = "ZL_COOKIE";
static const string kHlsSuffix = "/hls.m3u8";

class HttpCookieAttachment {
public:
    HttpCookieAttachment() {};
    ~HttpCookieAttachment() {};
public:
    //cookie生效作用域，本cookie只对该目录下的文件生效
    string _path;
    //上次鉴权失败信息,为空则上次鉴权成功
    string _err_msg;
    //本cookie是否为hls直播的
    bool _is_hls = false;
    //hls直播时的其他一些信息，主要用于播放器个数计数以及流量计数
    HlsCookieData::Ptr _hls_data;
    //如果是hls直播，那么判断该cookie是否使用过MediaSource::findAsync查找过
    //如果程序未正常退出，会残余上次的hls文件，所以判断hls直播是否存在的关键不是文件存在与否
    //而是应该判断HlsMediaSource是否已注册，但是这样会每次获取m3u8文件时都会用MediaSource::findAsync判断一次
    //会导致程序性能低下，所以我们应该在cookie声明周期的第一次判断HlsMediaSource是否已经注册，后续通过文件存在与否判断
    bool _have_find_media_source = false;
};

const string &HttpFileManager::getContentType(const char *name) {
    return getHttpContentType(name);
}

static string searchIndexFile(const string &dir){
    DIR *pDir;
    dirent *pDirent;
    if ((pDir = opendir(dir.data())) == NULL) {
        return "";
    }
    set<string> setFile;
    while ((pDirent = readdir(pDir)) != NULL) {
        static set<const char *, StrCaseCompare> indexSet = {"index.html", "index.htm", "index"};
        if (indexSet.find(pDirent->d_name) != indexSet.end()) {
            string ret = pDirent->d_name;
            closedir(pDir);
            return ret;
        }
    }
    closedir(pDir);
    return "";
}

static bool makeFolderMenu(const string &httpPath, const string &strFullPath, string &strRet) {
    GET_CONFIG(bool, dirMenu, Http::kDirMenu);
    if (!dirMenu) {
        //不允许浏览文件夹
        return false;
    }
    string strPathPrefix(strFullPath);
    string last_dir_name;
    if (strPathPrefix.back() == '/') {
        strPathPrefix.pop_back();
    } else {
        last_dir_name = split(strPathPrefix, "/").back();
    }

    if (!File::is_dir(strPathPrefix.data())) {
        return false;
    }
    stringstream ss;
    ss <<   "<html>\r\n"
            "<head>\r\n"
            "<title>文件索引</title>\r\n"
            "</head>\r\n"
            "<body>\r\n"
            "<h1>文件索引:";

    ss << httpPath;
    ss << "</h1>\r\n";
    if (httpPath != "/") {
        ss << "<li><a href=\"";
        ss << "/";
        ss << "\">";
        ss << "根目录";
        ss << "</a></li>\r\n";

        ss << "<li><a href=\"";
        if(!last_dir_name.empty()){
            ss << "./";
        }else{
            ss << "../";
        }
        ss << "\">";
        ss << "上级目录";
        ss << "</a></li>\r\n";
    }

    DIR *pDir;
    dirent *pDirent;
    if ((pDir = opendir(strPathPrefix.data())) == NULL) {
        return false;
    }
    set<string> setFile;
    while ((pDirent = readdir(pDir)) != NULL) {
        if (File::is_special_dir(pDirent->d_name)) {
            continue;
        }
        if (pDirent->d_name[0] == '.') {
            continue;
        }
        setFile.emplace(pDirent->d_name);
    }
    int i = 0;
    for (auto &strFile :setFile) {
        string strAbsolutePath = strPathPrefix + "/" + strFile;
        bool isDir = File::is_dir(strAbsolutePath.data());
        ss << "<li><span>" << i++ << "</span>\t";
        ss << "<a href=\"";
        if (!last_dir_name.empty()) {
            ss << last_dir_name << "/" << strFile;
        } else {
            ss << strFile;
        }

        if (isDir) {
            ss << "/";
        }
        ss << "\">";
        ss << strFile;
        if (isDir) {
            ss << "/</a></li>\r\n";
            continue;
        }
        //是文件
        struct stat fileData;
        if (0 == stat(strAbsolutePath.data(), &fileData)) {
            auto &fileSize = fileData.st_size;
            if (fileSize < 1024) {
                ss << " (" << fileData.st_size << "B)" << endl;
            } else if (fileSize < 1024 * 1024) {
                ss << fixed << setprecision(2) << " (" << fileData.st_size / 1024.0 << "KB)";
            } else if (fileSize < 1024 * 1024 * 1024) {
                ss << fixed << setprecision(2) << " (" << fileData.st_size / 1024 / 1024.0 << "MB)";
            } else {
                ss << fixed << setprecision(2) << " (" << fileData.st_size / 1024 / 1024 / 1024.0 << "GB)";
            }
        }
        ss << "</a></li>\r\n";
    }
    closedir(pDir);
    ss << "<ul>\r\n";
    ss << "</ul>\r\n</body></html>";
    ss.str().swap(strRet);
    return true;
}

//拦截hls的播放请求
static bool emitHlsPlayed(const Parser &parser, const MediaInfo &mediaInfo, const HttpSession::HttpAccessPathInvoker &invoker,TcpSession &sender){
    //访问的hls.m3u8结尾，我们转换成kBroadcastMediaPlayed事件
    Broadcast::AuthInvoker auth_invoker = [invoker](const string &err) {
        //cookie有效期为kHlsCookieSecond
        invoker(err, "", kHlsCookieSecond);
    };
    bool flag = NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastMediaPlayed, mediaInfo, auth_invoker, static_cast<SockInfo &>(sender));
    if (!flag) {
        //未开启鉴权，那么允许播放
        auth_invoker("");
    }
    return flag;
}

class SockInfoImp : public SockInfo{
public:
    typedef std::shared_ptr<SockInfoImp> Ptr;
    SockInfoImp() = default;
    ~SockInfoImp() override = default;

    string get_local_ip() override {
        return _local_ip;
    }

    uint16_t get_local_port() override {
        return _local_port;
    }

    string get_peer_ip() override {
        return _peer_ip;
    }

    uint16_t get_peer_port() override {
        return _peer_port;
    }

    string getIdentifier() const override {
        return _identifier;
    }

    string _local_ip;
    string _peer_ip;
    string _identifier;
    uint16_t _local_port;
    uint16_t _peer_port;
};

/**
 * 判断http客户端是否有权限访问文件的逻辑步骤
 * 1、根据http请求头查找cookie，找到进入步骤3
 * 2、根据http url参数查找cookie，如果还是未找到cookie则进入步骤5
 * 3、cookie标记是否有权限访问文件，如果有权限，直接返回文件
 * 4、cookie中记录的url参数是否跟本次url参数一致，如果一致直接返回客户端错误码
 * 5、触发kBroadcastHttpAccess事件
 */
static void canAccessPath(TcpSession &sender, const Parser &parser, const MediaInfo &mediaInfo, bool is_dir,
                          const function<void(const string &errMsg, const HttpServerCookie::Ptr &cookie)> &callback) {
    //获取用户唯一id
    auto uid = parser.Params();
    auto path = parser.Url();

    //先根据http头中的cookie字段获取cookie
    HttpServerCookie::Ptr cookie = HttpCookieManager::Instance().getCookie(kCookieName, parser.getHeader());
    //如果不是从http头中找到的cookie,我们让http客户端设置下cookie
    bool cookie_from_header = true;
    if (!cookie && !uid.empty()) {
        //客户端请求中无cookie,再根据该用户的用户id获取cookie
        cookie = HttpCookieManager::Instance().getCookieByUid(kCookieName, uid);
        cookie_from_header = false;
    }

    if (cookie) {
        //找到了cookie，对cookie上锁先
        auto lck = cookie->getLock();
        auto attachment = (*cookie)[kCookieName].get<HttpCookieAttachment>();
        if (path.find(attachment._path) == 0) {
            //上次cookie是限定本目录
            if (attachment._err_msg.empty()) {
                //上次鉴权成功
                if (attachment._is_hls) {
                    //如果播放的是hls，那么刷新hls的cookie(获取ts文件也会刷新)
                    cookie->updateTime();
                    cookie_from_header = false;
                }
                callback("", cookie_from_header ? nullptr : cookie);
                return;
            }
            //上次鉴权失败，但是如果url参数发生变更，那么也重新鉴权下
            if (parser.Params().empty() || parser.Params() == cookie->getUid()) {
                //url参数未变，或者本来就没有url参数，那么判断本次请求为重复请求，无访问权限
                callback(attachment._err_msg, cookie_from_header ? nullptr : cookie);
                return;
            }
        }
        //如果url参数变了或者不是限定本目录，那么旧cookie失效，重新鉴权
        HttpCookieManager::Instance().delCookie(cookie);
    }

    bool is_hls = mediaInfo._schema == HLS_SCHEMA;

    SockInfoImp::Ptr info = std::make_shared<SockInfoImp>();
    info->_identifier = sender.getIdentifier();
    info->_peer_ip = sender.get_peer_ip();
    info->_peer_port = sender.get_peer_port();
    info->_local_ip = sender.get_local_ip();
    info->_local_port = sender.get_local_port();

    //该用户从来未获取过cookie，这个时候我们广播是否允许该用户访问该http目录
    HttpSession::HttpAccessPathInvoker accessPathInvoker = [callback, uid, path, is_dir, is_hls, mediaInfo, info]
            (const string &errMsg, const string &cookie_path_in, int cookieLifeSecond) {
        HttpServerCookie::Ptr cookie;
        if (cookieLifeSecond) {
            //本次鉴权设置了有效期，我们把鉴权结果缓存在cookie中
            string cookie_path = cookie_path_in;
            if (cookie_path.empty()) {
                //如果未设置鉴权目录，那么我们采用当前目录
                cookie_path = is_dir ? path : path.substr(0, path.rfind("/") + 1);
            }

            cookie = HttpCookieManager::Instance().addCookie(kCookieName, uid, cookieLifeSecond);
            //对cookie上锁
            auto lck = cookie->getLock();
            HttpCookieAttachment attachment;
            //记录用户能访问的路径
            attachment._path = cookie_path;
            //记录能否访问
            attachment._err_msg = errMsg;
            //记录访问的是否为hls
            attachment._is_hls = is_hls;
            if (is_hls) {
                //hls相关信息
                attachment._hls_data = std::make_shared<HlsCookieData>(mediaInfo, info);
                //hls未查找MediaSource
                attachment._have_find_media_source = false;
            }
            (*cookie)[kCookieName].set<HttpCookieAttachment>(std::move(attachment));
            callback(errMsg, cookie);
        } else {
            callback(errMsg, nullptr);
        }
    };

    if (is_hls) {
        //是hls的播放鉴权,拦截之
        emitHlsPlayed(parser, mediaInfo, accessPathInvoker, sender);
        return;
    }

    //事件未被拦截，则认为是http下载请求
    bool flag = NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastHttpAccess, parser, path, is_dir, accessPathInvoker, static_cast<SockInfo &>(sender));
    if (!flag) {
        //此事件无人监听，我们默认都有权限访问
        callback("", nullptr);
    }
}

/**
 * 发送404 Not Found
 */
static void sendNotFound(const HttpFileManager::invoker &cb) {
    GET_CONFIG(string, notFound, Http::kNotFound);
    cb(404, "text/html", StrCaseMap(), std::make_shared<HttpStringBody>(notFound));
}

/**
 * 拼接文件路径
 */
static string pathCat(const string &a, const string &b){
    if (a.back() == '/') {
        return a + b;
    }
    return a + '/' + b;
}

/**
 * 访问文件
 * @param sender 事件触发者
 * @param parser http请求
 * @param mediaInfo http url信息
 * @param strFile 文件绝对路径
 * @param cb 回调对象
 */
static void accessFile(TcpSession &sender, const Parser &parser, const MediaInfo &mediaInfo, const string &strFile, const HttpFileManager::invoker &cb) {
    bool is_hls = end_with(strFile, kHlsSuffix);
    bool file_exist = File::is_file(strFile.data());
    if (!is_hls && !file_exist) {
        //文件不存在且不是hls,那么直接返回404
        sendNotFound(cb);
        return;
    }

    if (is_hls) {
        //hls，那么移除掉后缀获取真实的stream_id并且修改协议为HLS
        const_cast<string &>(mediaInfo._schema) = HLS_SCHEMA;
        replace(const_cast<string &>(mediaInfo._streamid), kHlsSuffix, "");
    }

    weak_ptr<TcpSession> weakSession = sender.shared_from_this();
    //判断是否有权限访问该文件
    canAccessPath(sender, parser, mediaInfo, false, [cb, strFile, parser, is_hls, mediaInfo, weakSession , file_exist](const string &errMsg, const HttpServerCookie::Ptr &cookie) {
        auto strongSession = weakSession.lock();
        if (!strongSession) {
            //http客户端已经断开，不需要回复
            return;
        }
        if (!errMsg.empty()) {
            //文件鉴权失败
            StrCaseMap headerOut;
            if (cookie) {
                auto lck = cookie->getLock();
                headerOut["Set-Cookie"] = cookie->getCookie((*cookie)[kCookieName].get<HttpCookieAttachment>()._path);
            }
            cb(401, "text/html", headerOut, std::make_shared<HttpStringBody>(errMsg));
            return;
        }

        auto response_file = [file_exist](const HttpServerCookie::Ptr &cookie, const HttpFileManager::invoker &cb, const string &strFile, const Parser &parser) {
            StrCaseMap httpHeader;
            if (cookie) {
                auto lck = cookie->getLock();
                httpHeader["Set-Cookie"] = cookie->getCookie((*cookie)[kCookieName].get<HttpCookieAttachment>()._path);
            }
            HttpSession::HttpResponseInvoker invoker = [&](int code, const StrCaseMap &headerOut, const HttpBody::Ptr &body) {
                if (cookie && file_exist) {
                    auto lck = cookie->getLock();
                    auto is_hls = (*cookie)[kCookieName].get<HttpCookieAttachment>()._is_hls;
                    if (is_hls) {
                        (*cookie)[kCookieName].get<HttpCookieAttachment>()._hls_data->addByteUsage(body->remainSize());
                    }
                }
                cb(code, HttpFileManager::getContentType(strFile.data()), headerOut, body);
            };
            invoker.responseFile(parser.getHeader(), httpHeader, strFile);
        };

        if (!is_hls) {
            //不是hls,直接回复文件或404
            response_file(cookie, cb, strFile, parser);
            return;
        }

        //是hls直播，判断HLS直播流是否已经注册
        bool have_find_media_src = false;
        if (cookie) {
            auto lck = cookie->getLock();
            have_find_media_src = (*cookie)[kCookieName].get<HttpCookieAttachment>()._have_find_media_source;
            if (!have_find_media_src) {
                (*cookie)[kCookieName].get<HttpCookieAttachment>()._have_find_media_source = true;
            }
        }
        if (have_find_media_src) {
            //之前该cookie已经通过MediaSource::findAsync查找过了，所以现在只以文件系统查找结果为准
            response_file(cookie, cb, strFile, parser);
            return;
        }
        //hls文件不存在，我们等待其生成并延后回复
        MediaSource::findAsync(mediaInfo, strongSession, [response_file, cookie, cb, strFile, parser](const MediaSource::Ptr &src) {
            if (cookie) {
                auto lck = cookie->getLock();
                //尝试添加HlsMediaSource的观看人数(HLS是按需生成的，这样可以触发HLS文件的生成)
                (*cookie)[kCookieName].get<HttpCookieAttachment>()._hls_data->addByteUsage(0);
            }
            if (src && File::is_file(strFile.data())) {
                //流和m3u8文件都存在，那么直接返回文件
                response_file(cookie, cb, strFile, parser);
                return;
            }
            auto hls = dynamic_pointer_cast<HlsMediaSource>(src);
            if (!hls) {
                //流不存在，那么直接返回文件(相当于纯粹的HLS文件服务器,但是会挂起播放器15秒左右(用于等待HLS流的注册))
                response_file(cookie, cb, strFile, parser);
                return;
            }

            //流存在，但是m3u8文件不存在，那么等待生成m3u8文件(HLS源注册后，并不会立即生成HLS文件，有人观看才会按需生成HLS文件)
            hls->waitForFile([response_file, cookie, cb, strFile, parser]() {
                response_file(cookie, cb, strFile, parser);
            });
        });
    });
}

static string getFilePath(const Parser &parser,const MediaInfo &mediaInfo, TcpSession &sender){
    GET_CONFIG(bool, enableVhost, General::kEnableVhost);
    GET_CONFIG(string, rootPath, Http::kRootPath);
    auto ret = File::absolutePath(enableVhost ? mediaInfo._vhost + parser.Url() : parser.Url(), rootPath);
    NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastHttpBeforeAccess, parser, ret, static_cast<SockInfo &>(sender));
    return ret;
}

/**
 * 访问文件或文件夹
 * @param sender 事件触发者
 * @param parser http请求
 * @param cb 回调对象
 */
void HttpFileManager::onAccessPath(TcpSession &sender, Parser &parser, const HttpFileManager::invoker &cb) {
    auto fullUrl = string(HTTP_SCHEMA) + "://" + parser["Host"] + parser.FullUrl();
    MediaInfo mediaInfo(fullUrl);
    auto strFile = getFilePath(parser, mediaInfo, sender);
    //访问的是文件夹
    if (File::is_dir(strFile.data())) {
        auto indexFile = searchIndexFile(strFile);
        if (!indexFile.empty()) {
            //发现该文件夹下有index文件
            strFile = pathCat(strFile, indexFile);
            parser.setUrl(pathCat(parser.Url(), indexFile));
            accessFile(sender, parser, mediaInfo, strFile, cb);
            return;
        }
        string strMenu;
        //生成文件夹菜单索引
        if (!makeFolderMenu(parser.Url(), strFile, strMenu)) {
            //文件夹不存在
            sendNotFound(cb);
            return;
        }
        //判断是否有权限访问该目录
        canAccessPath(sender, parser, mediaInfo, true, [strMenu, cb](const string &errMsg, const HttpServerCookie::Ptr &cookie) {
            if (!errMsg.empty()) {
                const_cast<string &>(strMenu) = errMsg;
            }
            StrCaseMap headerOut;
            if (cookie) {
                headerOut["Set-Cookie"] = cookie->getCookie((*cookie)[kCookieName].get<HttpCookieAttachment>()._path);
            }
            cb(errMsg.empty() ? 200 : 401, "text/html", headerOut, std::make_shared<HttpStringBody>(strMenu));
        });
        return;
    }

    //访问的是文件
    accessFile(sender, parser, mediaInfo, strFile, cb);
};


////////////////////////////////////HttpResponseInvokerImp//////////////////////////////////////

void HttpResponseInvokerImp::operator()(int code, const StrCaseMap &headerOut, const HttpBody::Ptr &body) const{
    if (_lambad) {
        _lambad(code, headerOut, body);
    }
}

void HttpResponseInvokerImp::operator()(int code, const StrCaseMap &headerOut, const string &body) const{
    this->operator()(code, headerOut, std::make_shared<HttpStringBody>(body));
}

HttpResponseInvokerImp::HttpResponseInvokerImp(const HttpResponseInvokerImp::HttpResponseInvokerLambda0 &lambda){
    _lambad = lambda;
}

HttpResponseInvokerImp::HttpResponseInvokerImp(const HttpResponseInvokerImp::HttpResponseInvokerLambda1 &lambda){
    if (!lambda) {
        _lambad = nullptr;
        return;
    }
    _lambad = [lambda](int code, const StrCaseMap &headerOut, const HttpBody::Ptr &body) {
        string str;
        if (body && body->remainSize()) {
            str = body->readData(body->remainSize())->toString();
        }
        lambda(code, headerOut, str);
    };
}

void HttpResponseInvokerImp::responseFile(const StrCaseMap &requestHeader,
                                          const StrCaseMap &responseHeader,
                                          const string &filePath) const {
    StrCaseMap &httpHeader = const_cast<StrCaseMap &>(responseHeader);
    std::shared_ptr<FILE> fp(fopen(filePath.data(), "rb"), [](FILE *fp) {
        if (fp) {
            fclose(fp);
        }
    });

    if (!fp) {
        //打开文件失败
        GET_CONFIG(string, notFound, Http::kNotFound);
        GET_CONFIG(string, charSet, Http::kCharSet);

        auto strContentType = StrPrinter << "text/html; charset=" << charSet << endl;
        httpHeader["Content-Type"] = strContentType;
        (*this)(404, httpHeader, notFound);
        return;
    }

    auto &strRange = const_cast<StrCaseMap &>(requestHeader)["Range"];
    size_t iRangeStart = 0;
    size_t iRangeEnd = 0;
    size_t fileSize = HttpMultiFormBody::fileSize(fp.get());

    int code;
    if (strRange.size() == 0) {
        //全部下载
        code = 200;
        iRangeEnd = fileSize - 1;
    } else {
        //分节下载
        code = 206;
        iRangeStart = atoll(FindField(strRange.data(), "bytes=", "-").data());
        iRangeEnd = atoll(FindField(strRange.data(), "-", "\r\n").data());
        if (iRangeEnd == 0) {
            iRangeEnd = fileSize - 1;
        }
        //分节下载返回Content-Range头
        httpHeader.emplace("Content-Range", StrPrinter << "bytes " << iRangeStart << "-" << iRangeEnd << "/" << fileSize << endl);
    }

    //回复文件
    HttpBody::Ptr fileBody = std::make_shared<HttpFileBody>(fp, iRangeStart, iRangeEnd - iRangeStart + 1);
    (*this)(code, httpHeader, fileBody);
}

HttpResponseInvokerImp::operator bool(){
    return _lambad.operator bool();
}


}//namespace mediakit