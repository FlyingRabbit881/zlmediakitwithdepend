﻿/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_AAC_H
#define ZLMEDIAKIT_AAC_H

#include "Frame.h"
#include "Track.h"
#define ADTS_HEADER_LEN 7

namespace mediakit{

string makeAacConfig(const uint8_t *hex, size_t length);
int getAacFrameLength(const uint8_t *hex, size_t length);
int dumpAacConfig(const string &config, size_t length, uint8_t *out, size_t out_size);
bool parseAacConfig(const string &config, int &samplerate, int &channels);

/**
 * aac音频通道
 */
class AACTrack : public AudioTrack{
public:
    typedef std::shared_ptr<AACTrack> Ptr;

    /**
     * 延后获取adts头信息
     * 在随后的inputFrame中获取adts头信息
     */
    AACTrack(){}

    /**
     * 构造aac类型的媒体
     * @param aac_cfg aac配置信息
     */
    AACTrack(const string &aac_cfg){
        setAacCfg(aac_cfg);
    }

    /**
     * 设置aac 配置信息
     */
    void setAacCfg(const string &aac_cfg){
        if (aac_cfg.size() < 2) {
            throw std::invalid_argument("adts配置必须最少2个字节");
        }
        _cfg = aac_cfg;
        onReady();
    }

    /**
     * 获取aac 配置信息
     */
    const string &getAacCfg() const{
        return _cfg;
    }

    /**
     * 返回编码类型
     */
    CodecId getCodecId() const override{
        return CodecAAC;
    }

    /**
     * 在获取aac_cfg前是无效的Track
     */
    bool ready() override {
        return !_cfg.empty();
    }

    /**
     * 返回音频采样率
     */
    int getAudioSampleRate() const override{
        return _sampleRate;
    }

    /**
     * 返回音频采样位数，一般为16或8
     */
    int getAudioSampleBit() const override{
        return _sampleBit;
    }

    /**
     * 返回音频通道数
     */
    int getAudioChannel() const override{
        return _channel;
    }

    /**
     * 输入数据帧,并获取aac_cfg
     * @param frame 数据帧
     */
    void inputFrame(const Frame::Ptr &frame) override{
        if (frame->prefixSize()) {
            //有adts头，尝试分帧
            auto ptr = frame->data();
            auto end = frame->data() + frame->size();
            while (ptr < end) {
                auto frame_len = getAacFrameLength((uint8_t *) ptr, end - ptr);
                if (frame_len < ADTS_HEADER_LEN) {
                    break;
                }

                auto sub_frame = std::make_shared<FrameInternal<FrameFromPtr> >(frame, (char *) ptr, frame_len, ADTS_HEADER_LEN);
                ptr += frame_len;
                sub_frame->setCodecId(CodecAAC);
                inputFrame_l(sub_frame);
            }
        } else {
            inputFrame_l(frame);
        }
    }

private:
    void inputFrame_l(const Frame::Ptr &frame) {
        if (_cfg.empty()) {
            //未获取到aac_cfg信息
            if (frame->prefixSize()) {
                //根据7个字节的adts头生成aac config
                _cfg = makeAacConfig((uint8_t *) (frame->data()), frame->prefixSize());
                onReady();
            } else {
                WarnL << "无法获取adts头!";
            }
        }

        if (frame->size() > frame->prefixSize()) {
            //除adts头外，有实际负载
            AudioTrack::inputFrame(frame);
        }
    }
    /**
     * 解析2个字节的aac配置
     */
    void onReady(){
        if (_cfg.size() < 2) {
            return;
        }
        parseAacConfig(_cfg, _sampleRate, _channel);
    }

    Track::Ptr clone() override {
        return std::make_shared<std::remove_reference<decltype(*this)>::type >(*this);
    }

    //生成sdp
    Sdp::Ptr getSdp() override ;
private:
    string _cfg;
    int _sampleRate = 0;
    int _sampleBit = 16;
    int _channel = 0;
};

/**
 * aac类型SDP
 */
class AACSdp : public Sdp {
public:
    /**
     * 构造函数
     * @param aac_cfg aac两个字节的配置描述
     * @param sample_rate 音频采样率
     * @param payload_type rtp payload type 默认98
     * @param bitrate 比特率
     */
    AACSdp(const string &aac_cfg,
           int sample_rate,
           int channels,
           int bitrate = 128,
           int payload_type = 98) : Sdp(sample_rate,payload_type){
        _printer << "m=audio 0 RTP/AVP " << payload_type << "\r\n";
        if (bitrate) {
            _printer << "b=AS:" << bitrate << "\r\n";
        }
        _printer << "a=rtpmap:" << payload_type << " MPEG4-GENERIC/" << sample_rate << "/" << channels << "\r\n";

        string configStr;
        char buf[4] = {0};
        for(auto &ch : aac_cfg){
            snprintf(buf, sizeof(buf), "%02X", (uint8_t)ch);
            configStr.append(buf);
        }
        _printer << "a=fmtp:" << payload_type << " streamtype=5;profile-level-id=1;mode=AAC-hbr;"
                 << "sizelength=13;indexlength=3;indexdeltalength=3;config=" << configStr << "\r\n";
        _printer << "a=control:trackID=" << (int)TrackAudio << "\r\n";
    }

    string getSdp() const override {
        return _printer;
    }

    CodecId getCodecId() const override {
        return CodecAAC;
    }
private:
    _StrPrinter _printer;
};

}//namespace mediakit
#endif //ZLMEDIAKIT_AAC_H