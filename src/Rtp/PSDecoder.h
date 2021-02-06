﻿/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_PSDECODER_H
#define ZLMEDIAKIT_PSDECODER_H

#if defined(ENABLE_RTPPROXY)
#include <stdint.h>
#include "Decoder.h"
namespace mediakit{

//ps解析器
class PSDecoder : public Decoder {
public:
    PSDecoder();
    ~PSDecoder();
    size_t input(const uint8_t* data, size_t bytes) override;
    void setOnDecode(onDecode cb) override;
    void setOnStream(onStream cb) override;

private:
    void *_ps_demuxer = nullptr;
    onDecode _on_decode;
    onStream _on_stream;
};

}//namespace mediakit
#endif//defined(ENABLE_RTPPROXY)
#endif //ZLMEDIAKIT_PSDECODER_H
