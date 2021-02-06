﻿/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "CommonRtp.h"

CommonRtpDecoder::CommonRtpDecoder(CodecId codec, size_t max_frame_size ){
    _codec = codec;
    _max_frame_size = max_frame_size;
    obtainFrame();
}

CodecId CommonRtpDecoder::getCodecId() const {
    return _codec;
}

void CommonRtpDecoder::obtainFrame() {
    _frame = ResourcePoolHelper<FrameImp>::obtainObj();
    _frame->_buffer.clear();
    _frame->_prefix_size = 0;
    _frame->_dts = 0;
    _frame->_codec_id = _codec;
}

bool CommonRtpDecoder::inputRtp(const RtpPacket::Ptr &rtp, bool){
    auto payload = rtp->data() + rtp->offset;
    auto size = rtp->size() - rtp->offset;
    if (size <= 0) {
        //无实际负载
        return false;
    }

    if (_frame->_dts != rtp->timeStamp || _frame->_buffer.size() > _max_frame_size) {
        //时间戳发生变化或者缓存超过MAX_FRAME_SIZE，则清空上帧数据
        if (!_frame->_buffer.empty()) {
            //有有效帧，则输出
            RtpCodec::inputFrame(_frame);
        }

        //新的一帧数据
        obtainFrame();
        _frame->_dts = rtp->timeStamp;
        _drop_flag = false;
    } else if (_last_seq != 0 && (uint16_t)(_last_seq + 1) != rtp->sequence) {
        //时间戳未发生变化，但是seq却不连续，说明中间rtp丢包了，那么整帧应该废弃
        WarnL << "rtp丢包:" << _last_seq << " -> " << rtp->sequence;
        _drop_flag = true;
        _frame->_buffer.clear();
    }

    if (!_drop_flag) {
        _frame->_buffer.append(payload, size);
    }

    _last_seq = rtp->sequence;
    return false;
}

////////////////////////////////////////////////////////////////

CommonRtpEncoder::CommonRtpEncoder(CodecId codec, uint32_t ssrc, uint32_t mtu_size,
                                   uint32_t sample_rate,  uint8_t payload_type, uint8_t interleaved)
        : CommonRtpDecoder(codec), RtpInfo(ssrc, mtu_size, sample_rate, payload_type, interleaved) {
}

void CommonRtpEncoder::inputFrame(const Frame::Ptr &frame){
    GET_CONFIG(uint32_t, cycleMS, Rtp::kCycleMS);
    auto stamp = frame->dts() % cycleMS;
    auto ptr = frame->data() + frame->prefixSize();
    auto len = frame->size() - frame->prefixSize();
    auto remain_size = len;
    const auto max_rtp_size = _ui32MtuSize - 20;

    while (remain_size > 0) {
        auto rtp_size = remain_size > max_rtp_size ? max_rtp_size : remain_size;
        RtpCodec::inputRtp(makeRtp(getTrackType(), ptr, rtp_size, false, stamp), false);
        ptr += rtp_size;
        remain_size -= rtp_size;
    }
}