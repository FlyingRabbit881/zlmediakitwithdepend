﻿/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "RtmpPusher.h"
#include "Rtmp/utils.h"
#include "Util/util.h"
#include "Util/onceToken.h"
#include "Thread/ThreadPool.h"
using namespace toolkit;
using namespace mediakit::Client;

namespace mediakit {

RtmpPusher::RtmpPusher(const EventPoller::Ptr &poller, const RtmpMediaSource::Ptr &src) : TcpClient(poller){
    _publish_src = src;
}

RtmpPusher::~RtmpPusher() {
    teardown();
    DebugL << endl;
}

void RtmpPusher::teardown() {
    if (alive()) {
        _app.clear();
        _stream_id.clear();
        _tc_url.clear();
        {
            lock_guard<recursive_mutex> lck(_mtx_on_result);
            _map_on_result.clear();
        }
        {
            lock_guard<recursive_mutex> lck(_mtx_on_status);
            _deque_on_status.clear();
        }
        _publish_timer.reset();
        reset();
        shutdown(SockException(Err_shutdown, "teardown"));
    }
}

void RtmpPusher::onPublishResult(const SockException &ex, bool handshake_done) {
    if (ex.getErrCode() == Err_shutdown) {
        //主动shutdown的，不触发回调
        return;
    }
    if (!handshake_done) {
        //播放结果回调
        _publish_timer.reset();
        if (_on_published) {
            _on_published(ex);
        }
    } else {
        //播放成功后异常断开回调
        if (_on_shutdown) {
            _on_shutdown(ex);
        }
    }

    if (ex) {
        shutdown(SockException(Err_shutdown,"teardown"));
    }
}

void RtmpPusher::publish(const string &url)  {
    teardown();
    string host_url = FindField(url.data(), "://", "/");
    _app = FindField(url.data(), (host_url + "/").data(), "/");
    _stream_id = FindField(url.data(), (host_url + "/" + _app + "/").data(), NULL);
    _tc_url = string("rtmp://") + host_url + "/" + _app;

    if (!_app.size() || !_stream_id.size()) {
        onPublishResult(SockException(Err_other, "rtmp url非法"), false);
        return;
    }
    DebugL << host_url << " " << _app << " " << _stream_id;

    auto iPort = atoi(FindField(host_url.data(), ":", NULL).data());
    if (iPort <= 0) {
        //rtmp 默认端口1935
        iPort = 1935;
    } else {
        //服务器域名
        host_url = FindField(host_url.data(), NULL, ":");
    }

    weak_ptr<RtmpPusher> weakSelf = dynamic_pointer_cast<RtmpPusher>(shared_from_this());
    float publishTimeOutSec = (*this)[kTimeoutMS].as<int>() / 1000.0f;
    _publish_timer.reset(new Timer(publishTimeOutSec, [weakSelf]() {
        auto strongSelf = weakSelf.lock();
        if (!strongSelf) {
            return false;
        }
        strongSelf->onPublishResult(SockException(Err_timeout, "publish rtmp timeout"), false);
        return false;
    }, getPoller()));

    if (!(*this)[kNetAdapter].empty()) {
        setNetAdapter((*this)[kNetAdapter]);
    }

    startConnect(host_url, iPort);
}

void RtmpPusher::onErr(const SockException &ex){
    //定时器_pPublishTimer为空后表明握手结束了
    onPublishResult(ex, !_publish_timer);
}

void RtmpPusher::onConnect(const SockException &err){
    if (err) {
        onPublishResult(err, false);
        return;
    }
    weak_ptr<RtmpPusher> weak_self = dynamic_pointer_cast<RtmpPusher>(shared_from_this());
    startClientSession([weak_self]() {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }

        strong_self->sendChunkSize(60000);
        strong_self->send_connect();
    });
}

void RtmpPusher::onRecv(const Buffer::Ptr &buf){
    try {
        onParseRtmp(buf->data(), buf->size());
    } catch (exception &e) {
        SockException ex(Err_other, e.what());
        //定时器_pPublishTimer为空后表明握手结束了
        onPublishResult(ex, !_publish_timer);
    }
}

inline void RtmpPusher::send_connect() {
    AMFValue obj(AMF_OBJECT);
    obj.set("app", _app);
    obj.set("type", "nonprivate");
    obj.set("tcUrl", _tc_url);
    obj.set("swfUrl", _tc_url);
    sendInvoke("connect", obj);
    addOnResultCB([this](AMFDecoder &dec) {
        //TraceL << "connect result";
        dec.load<AMFValue>();
        auto val = dec.load<AMFValue>();
        auto level = val["level"].as_string();
        auto code = val["code"].as_string();
        if (level != "status") {
            throw std::runtime_error(StrPrinter << "connect 失败:" << level << " " << code << endl);
        }
        send_createStream();
    });
}

inline void RtmpPusher::send_createStream() {
    AMFValue obj(AMF_NULL);
    sendInvoke("createStream", obj);
    addOnResultCB([this](AMFDecoder &dec) {
        //TraceL << "createStream result";
        dec.load<AMFValue>();
        _stream_index = dec.load<int>();
        send_publish();
    });
}

inline void RtmpPusher::send_publish() {
    AMFEncoder enc;
    enc << "publish" << ++_send_req_id << nullptr << _stream_id << _app;
    sendRequest(MSG_CMD, enc.data());

    addOnStatusCB([this](AMFValue &val) {
        auto level = val["level"].as_string();
        auto code = val["code"].as_string();
        if (level != "status") {
            throw std::runtime_error(StrPrinter << "publish 失败:" << level << " " << code << endl);
        }
        //start send media
        send_metaData();
    });
}

inline void RtmpPusher::send_metaData(){
    auto src = _publish_src.lock();
    if (!src) {
        throw std::runtime_error("the media source was released");
    }

    AMFEncoder enc;
    enc << "@setDataFrame" << "onMetaData" << src->getMetaData();
    sendRequest(MSG_DATA, enc.data());

    src->getConfigFrame([&](const RtmpPacket::Ptr &pkt) {
        sendRtmp(pkt->type_id, _stream_index, pkt, pkt->time_stamp, pkt->chunk_id);
    });

    _rtmp_reader = src->getRing()->attach(getPoller());
    weak_ptr<RtmpPusher> weak_self = dynamic_pointer_cast<RtmpPusher>(shared_from_this());
    _rtmp_reader->setReadCB([weak_self](const RtmpMediaSource::RingDataType &pkt) {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }

        size_t i = 0;
        auto size = pkt->size();
        strong_self->setSendFlushFlag(false);
        pkt->for_each([&](const RtmpPacket::Ptr &rtmp) {
            if (++i == size) {
                strong_self->setSendFlushFlag(true);
            }
            strong_self->sendRtmp(rtmp->type_id, strong_self->_stream_index, rtmp, rtmp->time_stamp, rtmp->chunk_id);
        });
    });
    _rtmp_reader->setDetachCB([weak_self]() {
        auto strong_self = weak_self.lock();
        if (strong_self) {
            strong_self->onPublishResult(SockException(Err_other, "媒体源被释放"), !strong_self->_publish_timer);
        }
    });
    onPublishResult(SockException(Err_success, "success"), false);
    //提升发送性能
    setSocketFlags();
}

void RtmpPusher::setSocketFlags(){
    GET_CONFIG(int, mergeWriteMS, General::kMergeWriteMS);
    if (mergeWriteMS > 0) {
        //提高发送性能
        setSendFlags(SOCKET_DEFAULE_FLAGS | FLAG_MORE);
        SockUtil::setNoDelay(getSock()->rawFD(), false);
    }
}

void RtmpPusher::onCmd_result(AMFDecoder &dec){
    auto req_id = dec.load<int>();
    lock_guard<recursive_mutex> lck(_mtx_on_result);
    auto it = _map_on_result.find(req_id);
    if (it != _map_on_result.end()) {
        it->second(dec);
        _map_on_result.erase(it);
    } else {
        WarnL << "unhandled _result";
    }
}

void RtmpPusher::onCmd_onStatus(AMFDecoder &dec) {
    AMFValue val;
    while (true) {
        val = dec.load<AMFValue>();
        if (val.type() == AMF_OBJECT) {
            break;
        }
    }
    if (val.type() != AMF_OBJECT) {
        throw std::runtime_error("onStatus:the result object was not found");
    }

    lock_guard<recursive_mutex> lck(_mtx_on_status);
    if (_deque_on_status.size()) {
        _deque_on_status.front()(val);
        _deque_on_status.pop_front();
    } else {
        auto level = val["level"];
        auto code = val["code"].as_string();
        if (level.type() == AMF_STRING) {
            if (level.as_string() != "status") {
                throw std::runtime_error(StrPrinter << "onStatus 失败:" << level.as_string() << " " << code << endl);
            }
        }
    }
}

void RtmpPusher::onRtmpChunk(RtmpPacket &chunk_data) {
    switch (chunk_data.type_id) {
        case MSG_CMD:
        case MSG_CMD3: {
            typedef void (RtmpPusher::*rtmpCMDHandle)(AMFDecoder &dec);
            static unordered_map<string, rtmpCMDHandle> g_mapCmd;
            static onceToken token([]() {
                g_mapCmd.emplace("_error", &RtmpPusher::onCmd_result);
                g_mapCmd.emplace("_result", &RtmpPusher::onCmd_result);
                g_mapCmd.emplace("onStatus", &RtmpPusher::onCmd_onStatus);
            });

            AMFDecoder dec(chunk_data.buffer, 0);
            std::string type = dec.load<std::string>();
            auto it = g_mapCmd.find(type);
            if (it != g_mapCmd.end()) {
                auto fun = it->second;
                (this->*fun)(dec);
            } else {
                WarnL << "can not support cmd:" << type;
            }
            break;
        }

        default:
            //WarnL << "unhandled message:" << (int) chunk_data.type_id << hexdump(chunk_data.buffer.data(), chunk_data.buffer.size());
            break;
    }
}


} /* namespace mediakit */

