#pragma once

#include <cstddef>

// 网络电台静态台表（HLS / m3u8）。台名作为内容数据展示，不做 i18n。
struct RadioStation {
    const char* name;
    const char* url;
};

inline constexpr RadioStation kRadioStations[] = {
    {"中国之声", "http://ngcdn001.cnr.cn/live/zgzs/index.m3u8"},
    {"经济之声", "http://ngcdn002.cnr.cn/live/jjzs/index.m3u8"},
    {"CRI 环球资讯广播 FM90.5", "http://sk.cri.cn/905.m3u8"},
    {"CRI 南海之声", "http://sk.cri.cn/nhzs.m3u8"},
    {"CRI 中文环球(华语环球)", "http://sk.cri.cn/hyhq.m3u8"},
    {"北京新闻广播",
     "http://satellitepull.cnr.cn/live/wxbjxwgb/playlist.m3u8"},
    {"北京文艺广播 FM87.6", "http://live.xmcdn.com/live/94/64.m3u8"},
    {"上海新闻广播",
     "http://satellitepull.cnr.cn/live/wx32shrmgb/playlist.m3u8"},
    {"上海东方广播",
     "http://satellitepull.cnr.cn/live/wx32dfgbdt/playlist.m3u8"},
    {"广东新闻广播",
     "http://satellitepull.cnr.cn/live/wxgdxwgb/playlist.m3u8"},
    {"广东珠江经济台 FM97.4", "http://live.xmcdn.com/live/252/64.m3u8"},
    {"广东音乐之声 FM99.3", "http://live.xmcdn.com/live/74/64.m3u8"},
    {"广州新闻电台 FM96.2", "http://live.xmcdn.com/live/256/64.m3u8"},
    {"广州交通电台 FM106.1", "http://ls.qingting.fm/live/4955.m3u8"},
    {"深圳飞扬971",
     "http://satellitepull.cnr.cn/live/wxszfy971/playlist.m3u8"},
    {"深圳交通频率 快乐1062", "http://ls.qingting.fm/live/1272.m3u8"},
    {"浙江之声", "http://satellitepull.cnr.cn/live/wxzjzs/playlist.m3u8"},
    {"浙江交通之声",
     "http://satellitepull.cnr.cn/live/wxzjjtgb/playlist.m3u8"},
    {"江苏新闻广播",
     "http://satellitepull.cnr.cn/live/wx32jsxwgb/playlist.m3u8"},
    {"江苏交通广播",
     "http://satellitepull.cnr.cn/live/wx32jsjtgb/playlist.m3u8"},
    {"楚天交通广播",
     "http://satellitepull.cnr.cn/live/wx32hubctjtgb/playlist.m3u8"},
    {"湖北之声",
     "http://satellitepull.cnr.cn/live/wx32hubzsgb/playlist.m3u8"},
    {"湖南交通广播",
     "http://satellitepull.cnr.cn/live/wx32hunjtgb/playlist.m3u8"},
    {"湖南新闻广播",
     "http://satellitepull.cnr.cn/live/wx32hunxwgb/playlist.m3u8"},
    {"四川交通广播",
     "http://satellitepull.cnr.cn/live/wxscjtgb/playlist.m3u8"},
    {"重庆音乐广播",
     "http://satellitepull.cnr.cn/live/wxcqyygb/playlist.m3u8"},
    {"山东交通广播",
     "http://satellitepull.cnr.cn/live/wxsdjtgb/playlist.m3u8"},
    {"河北交通广播",
     "http://satellitepull.cnr.cn/live/wxhebjtgb/playlist.m3u8"},
    {"河南交通广播", "http://stream.hndt.com/live/jiaotong/playlist.m3u8"},
    {"陕西交通广播",
     "http://satellitepull.cnr.cn/live/wxsxxjtgb/playlist.m3u8"},
    {"福建交通广播",
     "http://satellitepull.cnr.cn/live/wx32fjdnjtgb/playlist.m3u8"},
    {"辽宁交通广播",
     "http://satellitepull.cnr.cn/live/wxlnjtgb/playlist.m3u8"},
    {"黑龙江交通广播",
     "http://satellitepull.cnr.cn/live/wx32hljjtgb/playlist.m3u8"},
    {"第一财经广播",
     "http://satellitepull.cnr.cn/live/wx32dycjgb/playlist.m3u8"},
    {"东广新闻台 FM90.9", "http://ls.qingting.fm/live/275.m3u8"},
};

inline constexpr size_t kRadioStationCount =
    sizeof(kRadioStations) / sizeof(kRadioStations[0]);

// 默认台：深圳飞扬971（与改版前硬编码流一致）
inline constexpr int kDefaultRadioStationIndex = 14;
